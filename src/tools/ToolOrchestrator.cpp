#include "tools/ToolOrchestrator.h"
#include "hooks/HookExecutor.h"

#include <cstdlib>  // std::getenv, std::atoi

#include "agents/SubAgentManager.h"
#include "mcp/McpClientManager.h"
#include "tools/ToolRegistry.h"
#include "third_party/nlohmann_json.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>   // std::memcmp
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

using json = nlohmann::json;

namespace agent {
namespace tools {

namespace {

static const int kDefaultMaxResultChars = 100000;
static const int kMaxToolResultTruncation = 400000;
static const long long kMaxFullReadBytes = 256 * 1024;
static const wchar_t* kWebUserAgent = L"cpp-agent/1.0";

std::string JoinPath(const std::string& lhs, const std::string& rhs);
bool EnsureDirectoryRecursive(const std::string& path);
std::string ReadFileContent(const std::string& path, std::string* error);
bool WriteFileContent(const std::string& path,
                      const std::string& content,
                      std::string* error);
std::string ToLowerAscii(std::string value);

struct ParsedUrl {
  bool secure = false;
  INTERNET_PORT port = 0;
  std::string host;
  std::string path;
};

struct HttpResponse {
  int statusCode = 0;
  std::string body;
  std::string contentType;
  std::string location;
};

struct ShellToken {
  std::string text;
  bool wasQuoted = false;
};

std::wstring ToWide(const std::string& text) {
  if (text.empty()) return std::wstring();
  int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                 static_cast<int>(text.size()),
                                 nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                      static_cast<int>(text.size()),
                      &wide[0], size);
  return wide;
}

std::string ToUtf8(const std::wstring& text) {
  if (text.empty()) return std::string();
  int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                 static_cast<int>(text.size()),
                                 nullptr, 0, nullptr, nullptr);
  std::string utf8(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                      static_cast<int>(text.size()),
                      &utf8[0], size, nullptr, nullptr);
  return utf8;
}

std::string Trim(const std::string& value) {
  std::size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start]))) {
    ++start;
  }
  std::size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(start, end - start);
}

std::string QuoteForPowerShellSingleQuoted(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);
  for (char ch : value) {
    if (ch == '\'') escaped += "''";
    else escaped.push_back(ch);
  }
  return "'" + escaped + "'";
}

std::vector<ShellToken> TokenizeShellCommand(const std::string& command) {
  std::vector<ShellToken> tokens;
  ShellToken current;
  bool inSingle = false;
  bool inDouble = false;

  auto flush = [&]() {
    if (!current.text.empty() || current.wasQuoted) {
      tokens.push_back(current);
      current = ShellToken();
    }
  };

  for (std::size_t i = 0; i < command.size(); ++i) {
    const char ch = command[i];
    if (!inDouble && ch == '\'') {
      inSingle = !inSingle;
      current.wasQuoted = true;
      continue;
    }
    if (!inSingle && ch == '"') {
      inDouble = !inDouble;
      current.wasQuoted = true;
      continue;
    }
    if (!inSingle && !inDouble &&
        std::isspace(static_cast<unsigned char>(ch))) {
      flush();
      continue;
    }
    if (ch == '\\' && i + 1 < command.size()) {
      const char next = command[i + 1];
      if ((inDouble && (next == '"' || next == '\\')) ||
          (!inSingle && !inDouble &&
           (next == '"' || next == '\'' || next == '\\' ||
            std::isspace(static_cast<unsigned char>(next))))) {
        current.text.push_back(next);
        ++i;
        continue;
      }
    }
    current.text.push_back(ch);
  }
  flush();
  return tokens;
}

bool StartsWithCaseInsensitive(const std::string& value,
                               const std::string& prefix) {
  if (value.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(value[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

std::string NormalizeWindowsShellCommand(const std::string& command) {
  const std::string trimmed = Trim(command);
  if (trimmed.empty()) return trimmed;

  // GEMMA-ENHANCE: Translate common Linux/Bash shell idioms to PowerShell
  // equivalents BEFORE any other normalization. This catches the #1 defect
  // observed in Gemma-4-31B logs: using && and 2>/dev/null which are not
  // valid PowerShell syntax.
  {
    std::string result = trimmed;
    bool shellModified = false;

    // && -> ; (PowerShell statement separator)
    // Must not touch && inside quoted strings.
    {
      std::string out;
      out.reserve(result.size());
      bool inSingle = false;
      bool inDouble = false;
      for (std::size_t i = 0; i < result.size(); ++i) {
        char ch = result[i];
        if (ch == '\'' && !inDouble) {
          inSingle = !inSingle;
          out.push_back(ch);
        } else if (ch == '"' && !inSingle) {
          inDouble = !inDouble;
          out.push_back(ch);
        } else if (!inSingle && !inDouble && ch == '&' &&
                   i + 1 < result.size() && result[i + 1] == '&') {
          // Replace && with ;
          out.push_back(';');
          ++i;  // skip second '&'
          shellModified = true;
        } else {
          out.push_back(ch);
        }
      }
      result = out;
    }

    // 2>/dev/null -> 2>$null
    {
      const std::string devNull = "2>/dev/null";
      const std::string psNull = "2>$null";
      std::size_t pos = 0;
      while ((pos = result.find(devNull, pos)) != std::string::npos) {
        result.replace(pos, devNull.size(), psNull);
        pos += psNull.size();
        shellModified = true;
      }
    }

    // Also handle 1>/dev/null -> $null and >/dev/null -> $null
    {
      const std::string patterns[] = {"1>/dev/null", ">/dev/null"};
      const std::string replacement = "$null";
      for (const auto& pat : patterns) {
        std::size_t pos = 0;
        while ((pos = result.find(pat, pos)) != std::string::npos) {
          result.replace(pos, pat.size(), replacement);
          pos += replacement.size();
          shellModified = true;
        }
      }
    }

    if (shellModified) {
      // After shell syntax translation, recurse to apply other normalizations
      // (e.g., pipe conversions) on the translated command.
      return NormalizeWindowsShellCommand(result);
    }
  }

  // P0-03: Handle piped Unix commands (| grep, | head, | tail) FIRST,
  // before command-specific dispatch. This fixes the bug where commands
  // like "python -m pip list | head -100" would bypass pipe conversion
  // because "python" doesn't match any command-specific handler.
  // Now pipes are converted regardless of the base command name.
  {
    std::string result = trimmed;
    bool modified = false;

    // | grep pattern -> | Select-String -Pattern 'pattern'
    // Also handles grep flags: -i (case insensitive), -E (extended regex),
    // -v (invert match), -w (word match), -F (fixed strings)
    std::size_t grepPos = result.find("| grep ");
    if (grepPos != std::string::npos) {
      // Extract everything after "| grep "
      std::size_t contentStart = grepPos + 7;  // len("| grep ")
      std::size_t patternEnd = result.find(" |", contentStart);
      if (patternEnd == std::string::npos) patternEnd = result.size();
      std::string grepContent = result.substr(contentStart, patternEnd - contentStart);
      
      // Strip grep flags: -i, -E, -v, -w, -F, -iE, -Ei, etc.
      bool caseInsensitive = false;
      bool invertMatch = false;
      std::string cleanedPattern;
      
      // Tokenize the grep content to separate flags from pattern
      std::istringstream iss(grepContent);
      std::string token;
      bool inPattern = false;
      std::string accumulatedPattern;
      while (iss >> token) {
        if (token.size() >= 2 && token[0] == '-' && !inPattern) {
          // It's a flag
          for (size_t ci = 1; ci < token.size(); ++ci) {
            if (token[ci] == 'i' || token[ci] == 'I') caseInsensitive = true;
            if (token[ci] == 'v' || token[ci] == 'V') invertMatch = true;
            // -E, -F, -w are informational; Select-String uses different flags
          }
        } else {
          // It's the search pattern (or start of quoted pattern)
          inPattern = true;
          if (!accumulatedPattern.empty()) accumulatedPattern += " ";
          accumulatedPattern += token;
        }
      }
      
      // If the grep content was quoted (e.g., -iE "pattern|here"),
      // accumulatedPattern may contain the quotes. Strip them.
      cleanedPattern = accumulatedPattern;
      while (!cleanedPattern.empty() && cleanedPattern.back() == ' ') cleanedPattern.pop_back();
      while (!cleanedPattern.empty() && cleanedPattern.front() == ' ') cleanedPattern.erase(0, 1);
      // Remove surrounding quotes if present
      if (cleanedPattern.size() >= 2 &&
          cleanedPattern.front() == '"' && cleanedPattern.back() == '"') {
        cleanedPattern = cleanedPattern.substr(1, cleanedPattern.size() - 2);
      }
      
      if (!cleanedPattern.empty()) {
        std::ostringstream psArgs;
        psArgs << "| Select-String -Pattern "
               << QuoteForPowerShellSingleQuoted(cleanedPattern);
        if (caseInsensitive) {
          psArgs << " -CaseSensitive:$false";
        }
        if (invertMatch) {
          psArgs << " -NotMatch";
        }
        result = result.substr(0, grepPos) + psArgs.str() +
                 result.substr(patternEnd);
        modified = true;
      }
    }

    // | head -N -> | Select-Object -First N
    std::size_t headPos = result.find("| head ");
    if (headPos != std::string::npos) {
      std::size_t numStart = headPos + 7;  // len("| head ")
      // Check if there's a -N flag
      if (numStart < result.size() && result[numStart] == '-') {
        ++numStart;  // skip '-'
        std::size_t numEnd = numStart;
        while (numEnd < result.size() && std::isdigit(static_cast<unsigned char>(result[numEnd]))) ++numEnd;
        if (numEnd > numStart) {
          std::string count = result.substr(numStart, numEnd - numStart);
          result = result.substr(0, headPos) + "| Select-Object -First " + count +
                   result.substr(numEnd);
          modified = true;
        }
      } else {
        // head without -N defaults to 10
        result = result.substr(0, headPos) + "| Select-Object -First 10" +
                 result.substr(numStart);
        modified = true;
      }
    }

    // | tail -N -> | Select-Object -Last N
    std::size_t tailPos = result.find("| tail ");
    if (tailPos != std::string::npos) {
      std::size_t numStart = tailPos + 7;  // len("| tail ")
      if (numStart < result.size() && result[numStart] == '-') {
        ++numStart;
        std::size_t numEnd = numStart;
        while (numEnd < result.size() && std::isdigit(static_cast<unsigned char>(result[numEnd]))) ++numEnd;
        if (numEnd > numStart) {
          std::string count = result.substr(numStart, numEnd - numStart);
          result = result.substr(0, tailPos) + "| Select-Object -Last " + count +
                   result.substr(numEnd);
          modified = true;
        }
      } else {
        result = result.substr(0, tailPos) + "| Select-Object -Last 10" +
                 result.substr(numStart);
        modified = true;
      }
    }

    if (modified) return result;
  }


  const std::vector<ShellToken> tokens = TokenizeShellCommand(trimmed);
  if (tokens.empty()) return trimmed;
  const std::string commandName = ToLowerAscii(tokens[0].text);

  if (commandName == "pwd") {
    return "Get-Location";
  }

  if (commandName == "which" && tokens.size() >= 2) {
    return "Get-Command " + QuoteForPowerShellSingleQuoted(tokens[1].text) + " | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue";
  }

  if (commandName == "rm" && tokens.size() >= 2) {
    bool recurse = false;
    bool force = false;
    std::vector<std::string> paths;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::string& current = tokens[i].text;
      if (!current.empty() && current[0] == '-') {
        for (std::size_t j = 1; j < current.size(); ++j) {
          const char flag = static_cast<char>(std::tolower(static_cast<unsigned char>(current[j])));
          if (flag == 'r' || flag == 'R') recurse = true;
          if (flag == 'f' || flag == 'F') force = true;
        }
        continue;
      }
      paths.push_back(current);
    }
    if (paths.empty()) return trimmed;
    std::ostringstream normalized;
    normalized << "Remove-Item";
    if (recurse) normalized << " -Recurse";
    if (force) normalized << " -Force";
    for (const auto& path : paths) {
      normalized << " -Path " << QuoteForPowerShellSingleQuoted(path);
    }
    return normalized.str();
  }

  if (commandName == "cp" && tokens.size() >= 3) {
    bool recurse = false;
    std::vector<std::string> args;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::string& current = tokens[i].text;
      if (!current.empty() && current[0] == '-') {
        for (std::size_t j = 1; j < current.size(); ++j) {
          const char flag = static_cast<char>(std::tolower(static_cast<unsigned char>(current[j])));
          if (flag == 'r' || flag == 'R') recurse = true;
        }
        continue;
      }
      args.push_back(current);
    }
    if (args.size() < 2) return trimmed;
    std::string dest = args.back();
    args.pop_back();
    std::ostringstream normalized;
    normalized << "Copy-Item";
    if (recurse) normalized << " -Recurse";
    for (const auto& src : args) {
      normalized << " -Path " << QuoteForPowerShellSingleQuoted(src);
    }
    normalized << " -Destination " << QuoteForPowerShellSingleQuoted(dest);
    return normalized.str();
  }

  if (commandName == "mv" && tokens.size() >= 3) {
    std::vector<std::string> args;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::string& current = tokens[i].text;
      if (!current.empty() && current[0] == '-') continue;
      args.push_back(current);
    }
    if (args.size() < 2) return trimmed;
    std::string dest = args.back();
    args.pop_back();
    std::ostringstream normalized;
    normalized << "Move-Item";
    for (const auto& src : args) {
      normalized << " -Path " << QuoteForPowerShellSingleQuoted(src);
    }
    normalized << " -Destination " << QuoteForPowerShellSingleQuoted(dest);
    return normalized.str();
  }

  if (commandName == "ls") {
    bool useForce = false;
    std::vector<std::string> paths;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::string& current = tokens[i].text;
      if (!current.empty() && current[0] == '-') {
        for (std::size_t j = 1; j < current.size(); ++j) {
          const char flag = static_cast<char>(
              std::tolower(static_cast<unsigned char>(current[j])));
          if (flag == 'a') {
            useForce = true;
          } else if (flag == 'l' || flag == 'h') {
            continue;
          } else {
            return trimmed;
          }
        }
        continue;
      }
      paths.push_back(current);
    }

    std::ostringstream normalized;
    normalized << "Get-ChildItem";
    if (useForce) normalized << " -Force";
    for (const auto& path : paths) {
      normalized << " -Path " << QuoteForPowerShellSingleQuoted(path);
    }
    normalized << " | Select-Object Mode,LastWriteTime,Length,Name";
    return normalized.str();
  }

  if (commandName == "dir") {
    bool bareNames = false;
    bool filesOnly = false;
    bool dirsOnly = false;
    bool recurse = false;
    bool useForce = false;
    std::vector<std::string> paths;

    for (std::size_t i = 1; i < tokens.size(); ++i) {
      std::string current = ToLowerAscii(tokens[i].text);
      if (!current.empty() && (current[0] == '/' || current[0] == '-')) {
        if (current == "/b" || current == "-b") {
          bareNames = true;
          continue;
        }
        if (current == "/s" || current == "-s") {
          recurse = true;
          continue;
        }
        if (current == "/a" || current == "-a") {
          useForce = true;
          continue;
        }
        if (current == "/a-d" || current == "-a-d" || current == "/a:-d" ||
            current == "-a:-d") {
          filesOnly = true;
          continue;
        }
        if (current == "/ad" || current == "-ad" || current == "/a:d" ||
            current == "-a:d" || current == "/a+d" || current == "-a+d") {
          dirsOnly = true;
          continue;
        }
        return trimmed;
      }
      paths.push_back(tokens[i].text);
    }

    if (filesOnly && dirsOnly) return trimmed;

    std::ostringstream normalized;
    normalized << "Get-ChildItem";
    if (useForce) normalized << " -Force";
    if (recurse) normalized << " -Recurse";
    if (filesOnly) normalized << " -File";
    if (dirsOnly) normalized << " -Directory";
    for (const auto& path : paths) {
      normalized << " -Path " << QuoteForPowerShellSingleQuoted(path);
    }
    if (bareNames) {
      normalized << " | ForEach-Object { $_.Name }";
    } else {
      normalized << " | Select-Object Mode,LastWriteTime,Length,Name";
    }
    return normalized.str();
  }

  if (commandName == "cat") {
    std::ostringstream normalized;
    normalized << "Get-Content";
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      normalized << " "
                 << QuoteForPowerShellSingleQuoted(tokens[i].text);
    }
    return normalized.str();
  }

  if (commandName == "head" && tokens.size() >= 2) {
    // P0-03: Convert Unix head to PowerShell Select-Object -First N.
    // The head command is commonly used in pipes to limit output.
    int count = 10;  // default head shows first 10 lines
    std::string filePath;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::string& tok = tokens[i].text;
      if (tok.size() >= 2 && tok[0] == '-' && std::isdigit(static_cast<unsigned char>(tok[1]))) {
        count = std::atoi(tok.substr(1).c_str());
      } else if (tok == "-n" && i + 1 < tokens.size()) {
        count = std::atoi(tokens[++i].text.c_str());
      } else {
        filePath = tok;
      }
    }
    if (!filePath.empty()) {
      return "Get-Content " + QuoteForPowerShellSingleQuoted(filePath) +
             " -First " + std::to_string(count);
    }
    return "Select-Object -First " + std::to_string(count);
  }

  if (commandName == "tail" && tokens.size() >= 2) {
    // P0-03: Convert Unix tail to PowerShell Select-Object -Last N.
    int count = 10;  // default tail shows last 10 lines
    std::string filePath;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::string& tok = tokens[i].text;
      if (tok.size() >= 2 && tok[0] == '-' && std::isdigit(static_cast<unsigned char>(tok[1]))) {
        count = std::atoi(tok.substr(1).c_str());
      } else if (tok == "-n" && i + 1 < tokens.size()) {
        count = std::atoi(tokens[++i].text.c_str());
      } else {
        filePath = tok;
      }
    }
    if (!filePath.empty()) {
      return "Get-Content " + QuoteForPowerShellSingleQuoted(filePath) +
             " -Last " + std::to_string(count);
    }
    return "Select-Object -Last " + std::to_string(count);
  }

  if (commandName == "wc" && tokens.size() >= 2) {
    if (tokens[1].text == "-l" && tokens.size() >= 3) {
      return "(Get-Content " + QuoteForPowerShellSingleQuoted(tokens[2].text) +
             " | Measure-Object -Line).Lines";
    }
  }

  if (commandName == "touch" && tokens.size() >= 2) {
    return "New-Item -ItemType File -Force -Path " +
           QuoteForPowerShellSingleQuoted(tokens[1].text) + " | Out-Null";
  }

  // P1-02: Handle mkdir / mkdir -p
  if (commandName == "mkdir" && tokens.size() >= 2) {
    bool recursive = false;
    std::vector<ShellToken> paths;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::string& current = tokens[i].text;
      if (current == "-p" || current == "--parents") {
        recursive = true;
        continue;
      }
      if (!current.empty() && current[0] == '-') continue;
      paths.push_back(tokens[i]);
    }
    if (paths.empty()) return trimmed;
    std::vector<std::string> expandedPaths;
    for (const auto& pathToken : paths) {
      const std::string& path = pathToken.text;
      std::size_t braceStart = path.find('{');
      std::size_t braceEnd = path.find('}');
      if (pathToken.wasQuoted || braceStart == std::string::npos ||
          braceEnd == std::string::npos || braceEnd <= braceStart) {
        expandedPaths.push_back(path);
        continue;
      }
      bool expandedAny = false;
      const std::string prefix = path.substr(0, braceStart);
      const std::string suffix = path.substr(braceEnd + 1);
      const std::string braceContent =
          path.substr(braceStart + 1, braceEnd - braceStart - 1);
      std::size_t commaPos = 0;
      std::size_t searchStart = 0;
      while (true) {
        commaPos = braceContent.find(',', searchStart);
        std::string part = braceContent.substr(
            searchStart, commaPos == std::string::npos
                             ? std::string::npos
                             : commaPos - searchStart);
        part = Trim(part);
        if (!part.empty()) {
          expandedPaths.push_back(prefix + part + suffix);
          expandedAny = true;
        }
        if (commaPos == std::string::npos) break;
        searchStart = commaPos + 1;
      }
      if (!expandedAny) expandedPaths.push_back(path);
    }
    std::ostringstream normalized;
    normalized << "New-Item -ItemType Directory";
    if (recursive) normalized << " -Force";
    for (const auto& path : expandedPaths) {
      normalized << " -Path " << QuoteForPowerShellSingleQuoted(path);
    }
    return normalized.str();
  }

  if (commandName == "find" && tokens.size() >= 2) {
    return "Get-ChildItem -Recurse -Name " +
           QuoteForPowerShellSingleQuoted(tokens[1].text);
  }

  if (commandName == "grep" && tokens.size() >= 2) {
    // P0-03: Convert Unix grep to PowerShell Select-String (jianlai-graph fix).
    // Previously this was left to fail natively, which caused the model to
    // get stuck in a retry loop. Now we convert grep to its PowerShell equivalent.
    std::ostringstream normalized;
    // grep [flags] pattern [file...]
    // -> Select-String -Pattern pattern [-Path file]
    bool caseInsensitive = false;
    bool invertMatch = false;
    std::string pattern;
    std::vector<std::string> paths;
    bool inFlags = true;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::string& tok = tokens[i].text;
      if (inFlags) {
        // Check for standalone flags
        if (tok == "-i" || tok == "--ignore-case") {
          caseInsensitive = true; continue;
        }
        if (tok == "-v" || tok == "--invert-match") {
          invertMatch = true; continue;
        }
        // Check for combined short flags: -iE, -vi, -Ei, etc.
        if (tok.size() >= 2 && tok[0] == '-' && tok.find_first_not_of("-iIvVeEwWFf") == std::string::npos) {
          for (size_t ci = 1; ci < tok.size(); ++ci) {
            if (tok[ci] == 'i' || tok[ci] == 'I') caseInsensitive = true;
            if (tok[ci] == 'v' || tok[ci] == 'V') invertMatch = true;
            // -E, -F, -w, -f are accepted (Select-String uses .NET regex by default)
          }
          continue;
        }
        // Not a flag - treat everything from here as pattern and paths
        inFlags = false;
      }
      if (pattern.empty()) {
        pattern = tok;
      } else {
        paths.push_back(tok);
      }
    }
    if (pattern.empty()) return trimmed;
    normalized << "Select-String -Pattern " << QuoteForPowerShellSingleQuoted(pattern);
    if (caseInsensitive) normalized << " -CaseSensitive:$false";
    if (invertMatch) normalized << " -NotMatch";
    if (!paths.empty()) {
      for (const auto& p : paths) {
        normalized << " -Path " << QuoteForPowerShellSingleQuoted(p);
      }
    }
    return normalized.str();
  }

  // P0-03: /dev/null ->  rewrite (simple text substitution for PowerShell)
  if (command.find("/dev/null") != std::string::npos) {
    std::string rewritten = command;
    for (size_t pos = 0; (pos = rewritten.find("/dev/null", pos)) != std::string::npos; pos += 5) {
      rewritten.replace(pos, 9, "");
    }
    return rewritten;
  }

  return trimmed;
}

std::string ParentPath(const std::string& path) {
  const std::size_t pos = path.find_last_of("\\/");
  if (pos == std::string::npos) return std::string();
  if (pos == 0) return path.substr(0, 1);
  if (pos == 2 && path.size() >= 3 && path[1] == ':') return path.substr(0, 3);
  return path.substr(0, pos);
}

std::string JoinPath(const std::string& lhs, const std::string& rhs) {
  if (lhs.empty()) return rhs;
  if (rhs.empty()) return lhs;
  const char last = lhs[lhs.size() - 1];
  if (last == '\\' || last == '/') return lhs + rhs;
  return lhs + "\\" + rhs;
}

bool IsAbsolutePath(const std::string& path) {
  if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) &&
      path[1] == ':') {
    return true;
  }
  return path.size() >= 2 &&
         ((path[0] == '\\' && path[1] == '\\') ||
          (path[0] == '/' && path[1] == '/'));
}

std::string NormalizeSeparators(std::string path) {
  std::replace(path.begin(), path.end(), '/', '\\');
  return path;
}

std::string ToLowerAscii(std::string value) {
  for (std::size_t i = 0; i < value.size(); ++i) {
    value[i] = static_cast<char>(
        std::tolower(static_cast<unsigned char>(value[i])));
  }
  return value;
}

bool EnsureDirectoryRecursive(const std::string& path) {
  if (path.empty()) return false;

  std::string normalized = NormalizeSeparators(path);
  std::size_t cursor = 0;
  if (normalized.size() >= 2 && normalized[1] == ':') {
    cursor = 3;
  }

  while (cursor <= normalized.size()) {
    const std::size_t next = normalized.find('\\', cursor);
    const std::string current =
        next == std::string::npos ? normalized : normalized.substr(0, next);
    if (!current.empty()) {
      const std::wstring wideCurrent = ToWide(current);
      const DWORD attrs = GetFileAttributesW(wideCurrent.c_str());
      if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryW(wideCurrent.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
          return false;
        }
      } else if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return false;
      }
    }
    if (next == std::string::npos) break;
    cursor = next + 1;
  }

  return true;
}

std::string GetFullPathString(const std::string& path) {
  if (path.empty()) return std::string();
  std::vector<wchar_t> buffer(32768, L'\0');
  const std::wstring widePath = ToWide(path);
  DWORD length = GetFullPathNameW(widePath.c_str(),
                                  static_cast<DWORD>(buffer.size()),
                                  &buffer[0], nullptr);
  if (length == 0 || length >= buffer.size()) return std::string();
  return NormalizeSeparators(ToUtf8(std::wstring(&buffer[0], length)));
}

std::string EnsureTrailingSeparator(std::string path) {
  if (path.empty()) return path;
  path = NormalizeSeparators(path);
  const char tail = path[path.size() - 1];
  if (tail != '\\' && tail != '/') path.push_back('\\');
  return path;
}

bool IsPathWithinWorkspace(const std::string& workspaceRoot,
                           const std::string& candidate) {
  if (workspaceRoot.empty() || candidate.empty()) return false;
  const std::string normalizedRoot =
      ToLowerAscii(EnsureTrailingSeparator(GetFullPathString(workspaceRoot)));
  const std::string normalizedCandidate =
      ToLowerAscii(GetFullPathString(candidate));
  if (normalizedRoot.empty() || normalizedCandidate.empty()) return false;
  if (normalizedCandidate == normalizedRoot.substr(0, normalizedRoot.size() - 1)) {
    return true;
  }
  return normalizedCandidate.size() >= normalizedRoot.size() &&
         normalizedCandidate.compare(0, normalizedRoot.size(), normalizedRoot) == 0;
}

std::string ResolveToolPath(const std::string& requestedPath,
                            const std::string& workspaceRoot,
                            bool requireInsideWorkspace,
                            std::string* error) {
  const std::string trimmed = Trim(requestedPath);
  if (trimmed.empty()) {
    if (error) *error = "path cannot be empty";
    return std::string();
  }

  const bool isAbsolute = IsAbsolutePath(trimmed);
  const std::string candidate =
      (!isAbsolute && !workspaceRoot.empty()) ? JoinPath(workspaceRoot, trimmed)
                                              : trimmed;
  const std::string resolved = GetFullPathString(candidate);
  if (resolved.empty()) {
    if (error) *error = "failed to resolve path: " + trimmed;
    return std::string();
  }

  if (!isAbsolute && !workspaceRoot.empty() &&
      !IsPathWithinWorkspace(workspaceRoot, resolved)) {
    if (error) {
      *error =
          "relative path escapes trusted workspace, use a path inside the "
          "workspace or an explicit absolute path for external references: " +
          trimmed;
    }
    return std::string();
  }

  if (requireInsideWorkspace && !workspaceRoot.empty() &&
      !IsPathWithinWorkspace(workspaceRoot, resolved)) {
    if (error) {
      *error =
          "writes and edits must stay inside the trusted workspace: " + resolved;
    }
    return std::string();
  }

  return resolved;
}

bool StartsWithNoCase(const std::string& value, const std::string& prefix) {
  if (value.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(value[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

std::string ReplaceAll(std::string value,
                       const std::string& from,
                       const std::string& to) {
  if (from.empty()) return value;
  std::size_t pos = 0;
  while ((pos = value.find(from, pos)) != std::string::npos) {
    value.replace(pos, from.size(), to);
    pos += to.size();
  }
  return value;
}

void EraseTagBlock(std::string* html,
                   const std::string& startToken,
                   const std::string& endToken) {
  if (html == nullptr) return;
  std::string lower = ToLowerAscii(*html);
  std::size_t pos = 0;
  while ((pos = lower.find(startToken, pos)) != std::string::npos) {
    const std::size_t end = lower.find(endToken, pos);
    const std::size_t eraseLen =
        end == std::string::npos ? html->size() - pos
                                 : end + endToken.size() - pos;
    html->erase(pos, eraseLen);
    lower.erase(pos, eraseLen);
  }
}

std::string StripTags(const std::string& html) {
  std::string cleaned = html;
  EraseTagBlock(&cleaned, "<script", "</script>");
  EraseTagBlock(&cleaned, "<style", "</style>");

  std::string text;
  text.reserve(cleaned.size());
  bool insideTag = false;
  std::string tag;
  for (std::size_t i = 0; i < cleaned.size(); ++i) {
    const char ch = cleaned[i];
    if (ch == '<') {
      insideTag = true;
      tag.clear();
      continue;
    }
    if (insideTag) {
      if (ch == '>') {
        insideTag = false;
        const std::string lowerTag = ToLowerAscii(Trim(tag));
        if (StartsWithNoCase(lowerTag, "br") ||
            StartsWithNoCase(lowerTag, "/p") ||
            StartsWithNoCase(lowerTag, "/div") ||
            StartsWithNoCase(lowerTag, "/li") ||
            StartsWithNoCase(lowerTag, "/tr") ||
            StartsWithNoCase(lowerTag, "/h")) {
          text.push_back('\n');
        } else if (StartsWithNoCase(lowerTag, "li")) {
          if (!text.empty() && text[text.size() - 1] != '\n') text.push_back('\n');
          text += "- ";
        }
        continue;
      }
      tag.push_back(ch);
      continue;
    }
    text.push_back(ch);
  }

  text = ReplaceAll(text, "&nbsp;", " ");
  text = ReplaceAll(text, "&amp;", "&");
  text = ReplaceAll(text, "&lt;", "<");
  text = ReplaceAll(text, "&gt;", ">");
  text = ReplaceAll(text, "&quot;", "\"");
  text = ReplaceAll(text, "&#39;", "'");
  while (text.find("  ") != std::string::npos) {
    text = ReplaceAll(text, "  ", " ");
  }
  while (text.find("\n\n\n") != std::string::npos) {
    text = ReplaceAll(text, "\n\n\n", "\n\n");
  }
  return Trim(text);
}

std::string UrlEncode(const std::string& value) {
  std::ostringstream encoded;
  encoded << std::uppercase << std::hex;
  for (unsigned char ch : value) {
    if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      encoded << static_cast<char>(ch);
    } else if (ch == ' ') {
      encoded << '+';
    } else {
      encoded << '%' << static_cast<int>(ch / 16) << static_cast<int>(ch % 16);
    }
  }
  return encoded.str();
}

int HexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
  if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
  return -1;
}

std::string UrlDecode(const std::string& value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      int hi = HexValue(value[i + 1]);
      int lo = HexValue(value[i + 2]);
      if (hi >= 0 && lo >= 0) {
        decoded.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    if (value[i] == '+') {
      decoded.push_back(' ');
    } else {
      decoded.push_back(value[i]);
    }
  }
  return decoded;
}

bool ParseUrl(const std::string& url, ParsedUrl* parsed) {
  if (parsed == nullptr) return false;
  parsed->secure = false;
  parsed->port = 0;
  parsed->host.clear();
  parsed->path.clear();

  URL_COMPONENTS components;
  std::memset(&components, 0, sizeof(components));
  components.dwStructSize = sizeof(components);
  wchar_t hostName[256] = {0};
  wchar_t urlPath[2048] = {0};
  wchar_t extraInfo[2048] = {0};
  components.lpszHostName = hostName;
  components.dwHostNameLength =
      static_cast<DWORD>(sizeof(hostName) / sizeof(hostName[0]));
  components.lpszUrlPath = urlPath;
  components.dwUrlPathLength =
      static_cast<DWORD>(sizeof(urlPath) / sizeof(urlPath[0]));
  components.lpszExtraInfo = extraInfo;
  components.dwExtraInfoLength =
      static_cast<DWORD>(sizeof(extraInfo) / sizeof(extraInfo[0]));
  if (!WinHttpCrackUrl(ToWide(url).c_str(), 0, 0, &components)) {
    return false;
  }

  parsed->secure = components.nScheme == INTERNET_SCHEME_HTTPS;
  parsed->port = components.nPort;
  parsed->host = ToUtf8(
      std::wstring(components.lpszHostName, components.dwHostNameLength));
  parsed->path = ToUtf8(
      std::wstring(components.lpszUrlPath, components.dwUrlPathLength));
  parsed->path += ToUtf8(
      std::wstring(components.lpszExtraInfo, components.dwExtraInfoLength));
  if (parsed->path.empty()) parsed->path = "/";
  return !parsed->host.empty();
}

bool QueryHeaderString(HINTERNET request,
                       DWORD infoLevel,
                       std::string* value) {
  if (value == nullptr) return false;
  DWORD sizeBytes = 0;
  WinHttpQueryHeaders(request, infoLevel, WINHTTP_HEADER_NAME_BY_INDEX,
                      WINHTTP_NO_OUTPUT_BUFFER, &sizeBytes,
                      WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || sizeBytes == 0) {
    return false;
  }

  std::wstring buffer(static_cast<std::size_t>(sizeBytes / sizeof(wchar_t)), L'\0');
  if (!WinHttpQueryHeaders(request, infoLevel, WINHTTP_HEADER_NAME_BY_INDEX,
                           &buffer[0], &sizeBytes,
                           WINHTTP_NO_HEADER_INDEX)) {
    return false;
  }
  buffer.resize(sizeBytes / sizeof(wchar_t));
  while (!buffer.empty() && buffer.back() == L'\0') buffer.pop_back();
  *value = ToUtf8(buffer);
  return true;
}

bool HttpGet(const std::string& url,
             HttpResponse* response,
             std::string* error,
             int redirectDepth = 0) {
  if (response == nullptr) return false;
  if (redirectDepth > 5) {
    if (error) *error = "too many redirects";
    return false;
  }

  ParsedUrl parsed;
  if (!ParseUrl(url, &parsed)) {
    if (error) *error = "invalid url: " + url;
    return false;
  }

  HINTERNET session = WinHttpOpen(
      kWebUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    if (error) *error = "WinHttpOpen failed";
    return false;
  }

  HINTERNET connection = WinHttpConnect(
      session, ToWide(parsed.host).c_str(), parsed.port, 0);
  if (!connection) {
    if (error) *error = "WinHttpConnect failed";
    WinHttpCloseHandle(session);
    return false;
  }

  DWORD flags = parsed.secure ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET request = WinHttpOpenRequest(
      connection, L"GET", ToWide(parsed.path).c_str(), nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!request) {
    if (error) *error = "WinHttpOpenRequest failed";
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  std::wstring headers = L"Accept: text/html, text/plain, */*\r\n";
  if (!WinHttpSendRequest(request, headers.c_str(),
                          static_cast<DWORD>(headers.size()),
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr)) {
    if (error) *error = "WinHTTP request failed";
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  DWORD statusCode = 0;
  DWORD statusSize = sizeof(statusCode);
  if (!WinHttpQueryHeaders(request,
                           WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX,
                           &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
    if (error) *error = "failed to query status code";
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }
  response->statusCode = static_cast<int>(statusCode);
  QueryHeaderString(request, WINHTTP_QUERY_CONTENT_TYPE, &response->contentType);
  QueryHeaderString(request, WINHTTP_QUERY_LOCATION, &response->location);

  std::string body;
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available)) {
      if (error) *error = "failed to query response body";
      break;
    }
    if (available == 0) break;
    std::vector<char> buffer(available);
    DWORD bytesRead = 0;
    if (!WinHttpReadData(request, &buffer[0], available, &bytesRead)) {
      if (error) *error = "failed to read response body";
      break;
    }
    body.append(&buffer[0], &buffer[0] + bytesRead);
  }
  response->body = body;

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);

  if (!response->location.empty() &&
      (statusCode == 301 || statusCode == 302 || statusCode == 303 ||
       statusCode == 307 || statusCode == 308)) {
    std::string nextUrl = response->location;
    if (StartsWithNoCase(nextUrl, "/")) {
      nextUrl = std::string(parsed.secure ? "https://" : "http://") +
                parsed.host + nextUrl;
    }
    return HttpGet(nextUrl, response, error, redirectDepth + 1);
  }

  return error == nullptr || error->empty();
}

std::string ExtractHtmlTitle(const std::string& html) {
  const std::string lower = ToLowerAscii(html);
  const std::size_t startTag = lower.find("<title");
  if (startTag == std::string::npos) return std::string();
  const std::size_t start = lower.find('>', startTag);
  if (start == std::string::npos) return std::string();
  const std::size_t end = lower.find("</title>", start + 1);
  if (end == std::string::npos || end <= start) return std::string();
  return Trim(StripTags(html.substr(start + 1, end - start - 1)));
}

std::string ExtractHref(const std::string& tag) {
  const std::string lower = ToLowerAscii(tag);
  std::size_t hrefPos = lower.find("href=");
  if (hrefPos == std::string::npos) return std::string();
  hrefPos += 5;
  while (hrefPos < tag.size() &&
         std::isspace(static_cast<unsigned char>(tag[hrefPos]))) {
    ++hrefPos;
  }
  if (hrefPos >= tag.size()) return std::string();
  const char quote = tag[hrefPos];
  if (quote == '"' || quote == '\'') {
    const std::size_t end = tag.find(quote, hrefPos + 1);
    if (end == std::string::npos) return std::string();
    return tag.substr(hrefPos + 1, end - hrefPos - 1);
  }
  std::size_t end = hrefPos;
  while (end < tag.size() && !std::isspace(static_cast<unsigned char>(tag[end])) &&
         tag[end] != '>') {
    ++end;
  }
  return tag.substr(hrefPos, end - hrefPos);
}

std::string HtmlToText(const std::string& html) {
  return StripTags(html);
}

std::vector<std::pair<std::string, std::string> > ParseSearchResults(
    const std::string& html,
    int maxResults) {
  std::vector<std::pair<std::string, std::string> > results;
  std::string lower = ToLowerAscii(html);
  std::size_t pos = 0;
  while (results.size() < static_cast<std::size_t>(maxResults)) {
    const std::size_t anchorStart = lower.find("<a", pos);
    if (anchorStart == std::string::npos) break;
    const std::size_t tagEnd = lower.find('>', anchorStart);
    if (tagEnd == std::string::npos) break;
    const std::size_t close = lower.find("</a>", tagEnd + 1);
    if (close == std::string::npos) break;

    const std::string tag = html.substr(anchorStart, tagEnd - anchorStart + 1);
    std::string href = ExtractHref(tag);
    std::string text = Trim(HtmlToText(html.substr(tagEnd + 1, close - tagEnd - 1)));
    pos = close + 4;

    if (text.empty()) continue;
    if (href.find("uddg=") != std::string::npos) {
      const std::size_t uddg = href.find("uddg=");
      href = UrlDecode(href.substr(uddg + 5));
    }
    if (!(StartsWithNoCase(href, "http://") || StartsWithNoCase(href, "https://"))) {
      continue;
    }
    bool duplicate = false;
    for (std::size_t i = 0; i < results.size(); ++i) {
      if (results[i].second == href) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      results.push_back(std::make_pair(text, href));
    }
  }
  return results;
}

std::string BuildMarkdownFromHtml(const std::string& url,
                                  const std::string& html) {
  std::ostringstream markdown;
  const std::string title = ExtractHtmlTitle(html);
  if (!title.empty()) {
    markdown << "# " << title << "\n\n";
  }
  markdown << "Source: " << url << "\n\n";
  markdown << StripTags(html);
  return markdown.str();
}

std::string JsonGetString(const std::string& jsonStr,
                          const std::string& key,
                          const std::string& fallback = std::string()) {
  try {
    auto j = json::parse(jsonStr);
    if (j.contains(key) && j[key].is_string()) {
      return j[key].get<std::string>();
    }
  } catch (...) {
  }
  return fallback;
}

std::string JsonGetStringMultiKey(const std::string& jsonStr,
                                  const std::vector<std::string>& keys,
                                  const std::string& fallback = std::string()) {
  try {
    auto j = json::parse(jsonStr);
    for (const auto& key : keys) {
      if (j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
      }
    }
  } catch (...) {
  }
  return fallback;
}

bool JsonGetBool(const std::string& jsonStr,
                 const std::string& key,
                 bool fallback = false) {
  try {
    auto j = json::parse(jsonStr);
    if (j.contains(key) && j[key].is_boolean()) {
      return j[key].get<bool>();
    }
    if (j.contains(key) && j[key].is_string()) {
      const std::string val = j[key].get<std::string>();
      return val == "true" || val == "1";
    }
  } catch (...) {
  }
  return fallback;
}

int JsonGetInt(const std::string& jsonStr,
               const std::string& key,
               int fallback = 0) {
  try {
    json parsed = json::parse(jsonStr);
    if (parsed.contains(key) && parsed[key].is_number_integer()) {
      return parsed[key].get<int>();
    }
  } catch (...) {
  }
  return fallback;
}

std::string GetStateRootForTools(const std::string& workspaceRoot) {
  if (!workspaceRoot.empty()) {
    return JoinPath(workspaceRoot, ".cpp-agent");
  }
  std::vector<wchar_t> cwd(32768, L'\0');
  DWORD length = GetCurrentDirectoryW(static_cast<DWORD>(cwd.size()), &cwd[0]);
  if (length == 0 || length >= cwd.size()) {
    return ".cpp-agent";
  }
  return JoinPath(ToUtf8(std::wstring(&cwd[0], length)), ".cpp-agent");
}

std::string GetTaskStorePath(const std::string& workspaceRoot) {
  const std::string stateRoot = GetStateRootForTools(workspaceRoot);
  EnsureDirectoryRecursive(stateRoot);
  return JoinPath(stateRoot, "tasks.json");
}

json LoadTaskStore(const std::string& workspaceRoot) {
  const std::string path = GetTaskStorePath(workspaceRoot);
  std::string error;
  const std::string raw = ReadFileContent(path, &error);
  if (raw.empty()) return json::array();
  try {
    json parsed = json::parse(raw);
    if (parsed.is_array()) return parsed;
  } catch (...) {
  }
  return json::array();
}

bool SaveTaskStore(const std::string& workspaceRoot,
                   const json& tasks,
                   std::string* error) {
  return WriteFileContent(GetTaskStorePath(workspaceRoot), tasks.dump(2), error);
}

int FindTaskIndex(const json& tasks, const std::string& taskId) {
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    if (tasks[i].is_object() && tasks[i].value("id", std::string()) == taskId) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

std::string NextTaskId(const json& tasks) {
  int maxId = 0;
  for (const auto& task : tasks) {
    if (!task.is_object()) continue;
    const std::string id = task.value("id", std::string());
    if (id.empty()) continue;
    maxId = std::max(maxId, std::atoi(id.c_str()));
  }
  return std::to_string(maxId + 1);
}

std::string RenderTaskSummary(const json& tasks) {
  if (!tasks.is_array() || tasks.empty()) {
    return "No tasks found";
  }
  std::ostringstream out;
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    const json& task = tasks[i];
    const std::string id = task.value("id", std::string("?"));
    const std::string status = task.value("status", std::string("pending"));
    const std::string content = task.value("content",
        task.value("subject", std::string("(untitled)")));
    const std::string activeForm = task.value("activeForm", std::string());

    // Status indicator
    std::string statusIcon;
    if (status == "completed") statusIcon = "[x]";
    else if (status == "in_progress") statusIcon = "[~]";
    else if (status == "failed") statusIcon = "[!]";
    else if (status == "retrying") statusIcon = "[↻]";
    else statusIcon = "[ ]";

    out << "#" << id << " " << statusIcon << " " << content;
    if (!activeForm.empty() && (status == "in_progress" || status == "retrying")) {
      out << " (doing: " << activeForm << ")";
    }
    if (status == "failed") {
      const std::string errMsg = task.value("error_message", std::string());
      if (!errMsg.empty()) {
        out << "\n    error: " << errMsg;
      }
      int retryCount = task.value("retry_count", 0);
      if (retryCount > 0) {
        out << " (retried " << retryCount << " time(s))";
      }
    }
    if (status == "in_progress" || status == "pending" || status == "retrying") {
      const std::string criteria = task.value("acceptance_criteria", std::string());
      if (!criteria.empty()) {
        out << "\n    acceptance: " << criteria;
      }
    }
    if (task.contains("owner") && task["owner"].is_string() &&
        !task["owner"].get<std::string>().empty()) {
      out << " (" << task["owner"].get<std::string>() << ")";
    }
    if (i + 1 < tasks.size()) out << "\n";
  }
  return out.str();
}


std::string ReadFileContent(const std::string& path, std::string* error) {
  HANDLE handle = CreateFileW(ToWide(path).c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (error) *error = "failed to open file: " + path;
    return std::string();
  }

  LARGE_INTEGER size;
  if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0) {
    if (error) *error = "failed to get file size: " + path;
    CloseHandle(handle);
    return std::string();
  }

  std::string content(static_cast<std::size_t>(size.QuadPart), '\0');
  DWORD totalRead = 0;
  while (totalRead < static_cast<DWORD>(content.size())) {
    DWORD chunkRead = 0;
    const DWORD remaining = static_cast<DWORD>(content.size()) - totalRead;
    if (!ReadFile(handle, &content[totalRead], remaining, &chunkRead, nullptr)) {
      if (error) *error = "failed to read file: " + path;
      CloseHandle(handle);
      return std::string();
    }
    if (chunkRead == 0) break;
    totalRead += chunkRead;
  }
  content.resize(totalRead);
  CloseHandle(handle);
  return content;
}

bool WriteFileContent(const std::string& path,
                      const std::string& content,
                      std::string* error) {
  const std::string parent = ParentPath(path);
  if (!parent.empty() && !EnsureDirectoryRecursive(parent)) {
    if (error) *error = "failed to create parent directory: " + parent;
    return false;
  }
  HANDLE handle = CreateFileW(ToWide(path).c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (error) *error = "failed to write file: " + path;
    return false;
  }

  DWORD totalWritten = 0;
  while (totalWritten < static_cast<DWORD>(content.size())) {
    DWORD chunkWritten = 0;
    const DWORD remaining = static_cast<DWORD>(content.size()) - totalWritten;
    if (!WriteFile(handle, content.data() + totalWritten, remaining,
                   &chunkWritten, nullptr)) {
      if (error) *error = "failed to flush file: " + path;
      CloseHandle(handle);
      return false;
    }
    if (chunkWritten == 0) break;
    totalWritten += chunkWritten;
  }
  CloseHandle(handle);
  if (totalWritten != content.size()) {
    if (error) *error = "failed to flush file: " + path;
    return false;
  }
  return true;
}

std::string NormalizeLineEndings(const std::string& input) {
  std::string result;
  result.reserve(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '\r') {
      if (i + 1 < input.size() && input[i + 1] == '\n') {
        result.push_back('\n');
        ++i;
      } else {
        result.push_back('\n');
      }
    } else {
      result.push_back(input[i]);
    }
  }
  return result;
}

struct FileEntry {
  std::string name;
  bool isDirectory = false;
  long long size = 0;
};

bool WildcardMatch(const std::string& text, const std::string& pattern);

bool GlobSegmentHasWildcard(const std::string& segment) {
  return segment == "**" ||
         segment.find('*') != std::string::npos ||
         segment.find('?') != std::string::npos;
}

std::string NormalizeGlobPattern(std::string pattern) {
  for (std::size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] == '\\') pattern[i] = '/';
  }
  std::string normalized;
  normalized.reserve(pattern.size());
  bool previousSlash = false;
  for (char ch : pattern) {
    if (ch == '/') {
      if (!previousSlash) normalized.push_back(ch);
      previousSlash = true;
    } else {
      normalized.push_back(ch);
      previousSlash = false;
    }
  }
  return normalized;
}

std::vector<std::string> SplitGlobSegments(const std::string& path) {
  std::vector<std::string> segments;
  std::size_t start = 0;
  while (start < path.size()) {
    std::size_t slash = path.find('/', start);
    std::string segment =
        slash == std::string::npos ? path.substr(start)
                                   : path.substr(start, slash - start);
    if (!segment.empty()) segments.push_back(segment);
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return segments;
}

bool MatchGlobSegments(const std::vector<std::string>& pathSegments,
                       std::size_t pathIndex,
                       const std::vector<std::string>& patternSegments,
                       std::size_t patternIndex) {
  if (patternIndex == patternSegments.size()) {
    return pathIndex == pathSegments.size();
  }
  if (patternSegments[patternIndex] == "**") {
    if (MatchGlobSegments(
            pathSegments, pathIndex, patternSegments, patternIndex + 1)) {
      return true;
    }
    return pathIndex < pathSegments.size() &&
           MatchGlobSegments(
               pathSegments, pathIndex + 1, patternSegments, patternIndex);
  }
  if (pathIndex >= pathSegments.size()) return false;
  if (!WildcardMatch(pathSegments[pathIndex], patternSegments[patternIndex])) {
    return false;
  }
  return MatchGlobSegments(
      pathSegments, pathIndex + 1, patternSegments, patternIndex + 1);
}

void CollectGlobEntriesRecursive(const std::string& rootDirectory,
                                 const std::string& relativeDirectory,
                                 std::vector<FileEntry>* entries) {
  if (entries == nullptr) return;
  const std::string searchDirectory = relativeDirectory.empty()
      ? rootDirectory
      : JoinPath(rootDirectory, NormalizeSeparators(relativeDirectory));
  std::string searchPath = EnsureTrailingSeparator(searchDirectory) + "*";

  WIN32_FIND_DATAW findData;
  HANDLE findHandle = FindFirstFileW(ToWide(searchPath).c_str(), &findData);
  if (findHandle == INVALID_HANDLE_VALUE) {
    return;
  }

  do {
    const std::string name = ToUtf8(findData.cFileName);
    if (name == "." || name == "..") {
      continue;
    }
    FileEntry entry;
    entry.name = relativeDirectory.empty() ? name : relativeDirectory + "/" + name;
    entry.isDirectory =
        (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    LARGE_INTEGER size;
    size.LowPart = findData.nFileSizeLow;
    size.HighPart = findData.nFileSizeHigh;
    entry.size = size.QuadPart;
    entries->push_back(entry);
    if (entry.isDirectory) {
      CollectGlobEntriesRecursive(rootDirectory, entry.name, entries);
    }
  } while (FindNextFileW(findHandle, &findData));

  FindClose(findHandle);
}

std::vector<FileEntry> GlobFiles(const std::string& directory,
                                 const std::string& pattern) {
  std::vector<FileEntry> entries;
  const std::string normalizedPattern = NormalizeGlobPattern(pattern);
  const std::vector<std::string> allPatternSegments =
      SplitGlobSegments(normalizedPattern);
  if (allPatternSegments.empty()) return entries;

  std::size_t prefixCount = 0;
  while (prefixCount < allPatternSegments.size() &&
         !GlobSegmentHasWildcard(allPatternSegments[prefixCount])) {
    ++prefixCount;
  }

  std::string searchRoot = directory;
  for (std::size_t i = 0; i < prefixCount; ++i) {
    searchRoot = JoinPath(searchRoot, allPatternSegments[i]);
  }
  const DWORD attrs = GetFileAttributesW(ToWide(searchRoot).c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    return entries;
  }
  if (prefixCount == allPatternSegments.size()) {
    FileEntry entry;
    entry.name = allPatternSegments.back();
    entry.isDirectory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    LARGE_INTEGER size;
    WIN32_FILE_ATTRIBUTE_DATA data;
    ZeroMemory(&data, sizeof(data));
    if (GetFileAttributesExW(
            ToWide(searchRoot).c_str(), GetFileExInfoStandard, &data)) {
      size.LowPart = data.nFileSizeLow;
      size.HighPart = data.nFileSizeHigh;
    } else {
      size.LowPart = 0;
      size.HighPart = 0;
    }
    entry.size = size.QuadPart;
    entries.push_back(entry);
    return entries;
  }

  std::vector<std::string> remainingPatternSegments(
      allPatternSegments.begin() + static_cast<std::ptrdiff_t>(prefixCount),
      allPatternSegments.end());
  std::vector<FileEntry> candidates;
  CollectGlobEntriesRecursive(searchRoot, std::string(), &candidates);
  for (const auto& candidate : candidates) {
    const std::vector<std::string> candidateSegments =
        SplitGlobSegments(candidate.name);
    if (MatchGlobSegments(
            candidateSegments, 0, remainingPatternSegments, 0)) {
      entries.push_back(candidate);
    }
  }
  return entries;
}

std::string GrepFile(const std::string& filePath,
                     const std::string& pattern,
                     int maxMatches) {
  std::string readError;
  const std::string content = NormalizeLineEndings(
      ReadFileContent(filePath, &readError));
  if (!readError.empty()) return std::string();

  std::ostringstream result;
  int lineNumber = 0;
  int matches = 0;
  const bool caseInsensitive = true;

  auto matchLine = [&](const std::string& haystack, const std::string& needle) {
    if (caseInsensitive) {
      auto it = std::search(
          haystack.begin(), haystack.end(), needle.begin(), needle.end(),
          [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
          });
      return it != haystack.end();
    }
    return haystack.find(needle) != std::string::npos;
  };

  std::istringstream input(content);
  std::string line;
  while (std::getline(input, line) && matches < maxMatches) {
    ++lineNumber;
    if (matchLine(line, pattern)) {
      result << filePath << ":" << lineNumber << ": " << line << "\n";
      ++matches;
    }
  }
  return result.str();
}

bool WildcardMatch(const std::string& text, const std::string& pattern) {
  std::size_t t = 0;
  std::size_t p = 0;
  std::size_t starIdx = std::string::npos;
  std::size_t matchIdx = 0;

  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' ||
        std::tolower(static_cast<unsigned char>(pattern[p])) ==
            std::tolower(static_cast<unsigned char>(text[t])))) {
      ++t;
      ++p;
    } else if (p < pattern.size() && pattern[p] == '*') {
      starIdx = p;
      matchIdx = t;
      ++p;
    } else if (starIdx != std::string::npos) {
      p = starIdx + 1;
      matchIdx++;
      t = matchIdx;
    } else {
      return false;
    }
  }

  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

void GrepDirectory(const std::string& dirPath,
                   const std::string& pattern,
                   int maxMatches,
                   int* matchCount,
                   std::ostringstream* output) {
  std::string searchPath = dirPath;
  if (!searchPath.empty() && searchPath.back() != '\\') {
    searchPath.push_back('\\');
  }
  searchPath += "*";

  WIN32_FIND_DATAW findData;
  HANDLE findHandle = FindFirstFileW(ToWide(searchPath).c_str(), &findData);
  if (findHandle == INVALID_HANDLE_VALUE) return;

  do {
    if (*matchCount >= maxMatches) break;
    const std::string fileName = ToUtf8(findData.cFileName);
    if (fileName == "." || fileName == "..") {
      continue;
    }
    std::string fullPath = dirPath;
    if (!fullPath.empty() && fullPath.back() != '\\') fullPath.push_back('\\');
    fullPath += fileName;

    if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      GrepDirectory(fullPath, pattern, maxMatches, matchCount, output);
    } else {
      std::string result = GrepFile(fullPath, pattern, maxMatches - *matchCount);
      if (!result.empty()) {
        *output << result;
        for (char ch : result) {
          if (ch == '\n') ++(*matchCount);
        }
      }
    }
  } while (FindNextFileW(findHandle, &findData));

  FindClose(findHandle);
}

// ---- Fuzzy matching helpers (aligned with local-ace FileEditTool/utils.ts) ----

// Flags for MapNormalizedMatchToOriginal to indicate which transformations
// were applied to the normalized string.
enum NormalizeFlags {
  NORM_CRLF   = 1 << 0,  // \r\n → \n applied
  NORM_QUOTES = 1 << 1,  // curly → straight quotes applied
  NORM_WS     = 1 << 2,  // trailing whitespace stripped
};

// Reverse XML entity escaping that weaker LLMs (e.g. Qwen) may produce
// in tool_call arguments. Aligned with local-ace de-sanitize pass.
void DesanitizeXmlEntities(std::string& s) {
  // Order matters: &amp; must be last to avoid double-decode.
  static const struct { const char* from; const char* to; } kEntities[] = {
    {"&lt;",   "<"},
    {"&gt;",   ">"},
    {"&quot;", "\""},
    {"&apos;", "'"},
    {"&amp;",  "&"},
  };
  for (const auto& ent : kEntities) {
    std::string from(ent.from);
    std::string to(ent.to);
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
      s.replace(pos, from.size(), to);
      pos += to.size();
    }
  }
}

// Check if a file path is a Markdown file (skip stripTrailingWhitespace for .md/.mdx).
bool IsMarkdownFile(const std::string& path) {
  std::size_t len = path.size();
  if (len >= 4 &&
      (path[len - 4] == '.') &&
      (path[len - 3] == 'm' || path[len - 3] == 'M') &&
      (path[len - 2] == 'd' || path[len - 2] == 'D') &&
      (path[len - 1] == 'x' || path[len - 1] == 'X')) {
    return true;  // .mdx
  }
  if (len >= 3 &&
      path[len - 3] == '.' &&
      (path[len - 2] == 'm' || path[len - 2] == 'M') &&
      (path[len - 1] == 'd' || path[len - 1] == 'D')) {
    return true;  // .md
  }
  return false;
}

// Replace all \r\n with \n in-place.
void NormalizeCRLF(std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') {
      out += '\n';
      ++i;  // skip the \n of \r\n
    } else if (s[i] == '\r') {
      // Bare CR → LF
      out += '\n';
    } else {
      out += s[i];
    }
  }
  s = std::move(out);
}

// Replace curly quotes with straight quotes (aligned with local-ace normalizeQuotes).
void NormalizeQuotes(std::string& s) {
  // UTF-8 byte sequences for curly quotes:
  //   '\u2018' (') = E2 80 98    '\u2019' (') = E2 80 99
  //   '\u201C' (") = E2 80 9C    '\u201D' (") = E2 80 9D
  static const struct { const char* utf8; int len; char replacement; } kQuotes[] = {
    {"\xE2\x80\x98", 3, '\''},  // left single curly '
    {"\xE2\x80\x99", 3, '\''},  // right single curly '
    {"\xE2\x80\x9C", 3, '"'},   // left double curly "
    {"\xE2\x80\x9D", 3, '"'},   // right double curly "
  };
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    bool replaced = false;
    for (const auto& q : kQuotes) {
      if (i + q.len <= s.size() &&
          std::memcmp(s.data() + i, q.utf8, q.len) == 0) {
        out += q.replacement;
        i += q.len;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      out += s[i];
      ++i;
    }
  }
  s = std::move(out);
}

// Strip trailing whitespace from each line (aligned with local-ace stripTrailingWhitespace).
std::string StripTrailingWhitespace(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  while (i < s.size()) {
    // Collect line content (up to line ending)
    std::size_t lineStart = i;
    while (i < s.size() && s[i] != '\r' && s[i] != '\n') ++i;
    // Trim trailing whitespace from line content
    std::size_t lineEnd = i;
    while (lineEnd > lineStart &&
           (s[lineEnd - 1] == ' ' || s[lineEnd - 1] == '\t')) {
      --lineEnd;
    }
    out.append(s, lineStart, lineEnd - lineStart);
    // Preserve line ending(s)
    if (i < s.size()) {
      if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') {
        out += "\r\n";
        i += 2;
      } else {
        out += s[i];
        ++i;
      }
    }
  }
  return out;
}

// Map a match found in normalized content back to the original content.
// Returns the substring of `original` that corresponds to the match at
// [normPos, normPos+normLen) in `normalized`.
std::string MapNormalizedMatchToOriginal(
    const std::string& original,
    const std::string& normalized,
    std::size_t normPos,
    std::size_t normLen,
    int flags = NORM_CRLF) {
  // Build a character-level mapping from normalized → original positions.
  // The mapping accounts for the transformations indicated by |flags|.
  std::vector<std::size_t> origMap;  // origMap[i] = position in original for normalized[i]
  origMap.reserve(normalized.size() + 1);

  std::size_t oi = 0, ni = 0;
  while (ni <= normalized.size() && oi <= original.size()) {
    if (ni == origMap.size()) {
      origMap.push_back(oi);
    }
    if (ni >= normalized.size()) break;
    if (oi >= original.size()) break;

    // CRLF in original → single LF in normalized
    if ((flags & NORM_CRLF) &&
        original[oi] == '\r' && oi + 1 < original.size() && original[oi + 1] == '\n') {
      oi += 2;
      ni += 1;
    }
    // Curly quote in original (3 bytes) → straight quote in normalized (1 byte)
    else if ((flags & NORM_QUOTES) &&
             oi + 2 < original.size() &&
             (unsigned char)original[oi] == 0xE2 &&
             (unsigned char)original[oi + 1] == 0x80) {
      unsigned char b3 = (unsigned char)original[oi + 2];
      if (b3 == 0x98 || b3 == 0x99 || b3 == 0x9C || b3 == 0x9D) {
        oi += 3;
        ni += 1;
      } else {
        oi += 1;
        ni += 1;
      }
    }
    // Trailing whitespace in original → stripped in normalized
    else if ((flags & NORM_WS) &&
             (original[oi] == ' ' || original[oi] == '\t')) {
      // Peek ahead to see if this whitespace is followed by a newline (trailing ws)
      std::size_t peek = oi + 1;
      while (peek < original.size() &&
             (original[peek] == ' ' || original[peek] == '\t')) {
        ++peek;
      }
      if (peek < original.size() &&
          (original[peek] == '\n' || original[peek] == '\r')) {
        // Trailing whitespace — skip it in original, don't advance normalized
        oi = peek;
        continue;
      }
      // Non-trailing whitespace — advance both normally
      oi += 1;
      ni += 1;
    }
    else {
      oi += 1;
      ni += 1;
    }
  }
  // Ensure origMap covers the full normalized length
  while (origMap.size() <= normalized.size()) {
    origMap.push_back(oi);
  }

  std::size_t origStart = origMap[normPos];
  std::size_t origEnd = origMap[std::min(normPos + normLen, normalized.size())];
  return original.substr(origStart, origEnd - origStart);
}

// Preserve curly quote style from the file when the model used straight quotes.
// Aligned with local-ace preserveQuoteStyle.
std::string PreserveQuoteStyle(
    const std::string& modelOldStr,
    const std::string& actualOldStr,
    const std::string& modelNewStr) {
  if (modelOldStr == actualOldStr) return modelNewStr;

  // Detect which curly quote types were in the file's actual string
  bool hasDoubleCurly = actualOldStr.find("\xE2\x80\x9C") != std::string::npos ||
                        actualOldStr.find("\xE2\x80\x9D") != std::string::npos;
  bool hasSingleCurly = actualOldStr.find("\xE2\x80\x98") != std::string::npos ||
                        actualOldStr.find("\xE2\x80\x99") != std::string::npos;

  if (!hasDoubleCurly && !hasSingleCurly) return modelNewStr;

  std::string result = modelNewStr;
  // Simple heuristic: replace straight quotes with curly quotes
  // using open/close context detection
  if (hasDoubleCurly) {
    std::string out;
    out.reserve(result.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
      if (result[i] == '"') {
        bool isOpening = (i == 0 || result[i - 1] == ' ' ||
                          result[i - 1] == '\t' || result[i - 1] == '\n' ||
                          result[i - 1] == '(' || result[i - 1] == '[' ||
                          result[i - 1] == '{');
        out += isOpening ? "\xE2\x80\x9C" : "\xE2\x80\x9D";
      } else {
        out += result[i];
      }
    }
    result = std::move(out);
  }
  if (hasSingleCurly) {
    std::string out;
    out.reserve(result.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
      if (result[i] == '\'') {
        // Check for contraction (apostrophe between letters)
        bool prevIsLetter = (i > 0 && std::isalpha(static_cast<unsigned char>(result[i - 1])));
        bool nextIsLetter = (i + 1 < result.size() && std::isalpha(static_cast<unsigned char>(result[i + 1])));
        if (prevIsLetter && nextIsLetter) {
          out += "\xE2\x80\x99";  // right single curly for apostrophe
        } else {
          bool isOpening = (i == 0 || result[i - 1] == ' ' ||
                            result[i - 1] == '\t' || result[i - 1] == '\n' ||
                            result[i - 1] == '(');
          out += isOpening ? "\xE2\x80\x98" : "\xE2\x80\x99";
        }
      } else {
        out += result[i];
      }
    }
    result = std::move(out);
  }
  return result;
}

}  // namespace

// P0-03: Case-insensitive comparison for tool name matching (moved to agent::tools scope)
static bool CaseInsensitiveCompare(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}


ToolOrchestrator::ToolOrchestrator() {
  // P0-03: Read Bash timeout from environment variable (aligned with local-ace).
  const char* envTimeout = std::getenv("AGENT_BASH_TIMEOUT_MS");
  if (envTimeout) {
    int val = std::atoi(envTimeout);
    if (val > 0) bashTimeoutMs_ = val;
  }
}

void ToolOrchestrator::SetBashTimeoutMs(int timeoutMs) {
  if (timeoutMs > 0) bashTimeoutMs_ = timeoutMs;
}

void ToolOrchestrator::SetToolRegistry(const ToolRegistry* registry) {
  toolRegistry_ = registry;
}

void ToolOrchestrator::SetSubAgentManager(
    agents::SubAgentManager* subAgentManager) {
  subAgentManager_ = subAgentManager;
}

void ToolOrchestrator::SetMcpClientManager(
    mcp::McpClientManager* mcpClientManager) {
  mcpClientManager_ = mcpClientManager;
}

void ToolOrchestrator::SetWorkspaceRoot(const std::string& workspaceRoot) {
  workspaceRoot_ = GetFullPathString(workspaceRoot);
}

void ToolOrchestrator::SetHookExecutor(hooks::HookExecutor* hookExecutor) {
  hookExecutor_ = hookExecutor;
}

void ToolOrchestrator::SetToolCompletionCallback(ToolCompletionCallback cb) {
  toolCompletionCallback_ = std::move(cb);
}

std::vector<ToolBatch> ToolOrchestrator::PartitionToolCalls(
    const std::vector<core::ContentBlock>& toolUseBlocks) const {
  std::vector<ToolBatch> batches;

  for (const auto& block : toolUseBlocks) {
    bool concurrentSafe = false;
    if (toolRegistry_) {
      concurrentSafe =
          toolRegistry_->IsConcurrencySafe(block.asToolUse.name);
    }

    if (!batches.empty() && concurrentSafe && batches.back().concurrentSafe) {
      batches.back().blocks.push_back(block);
      continue;
    }

    ToolBatch batch;
    batch.concurrentSafe = concurrentSafe;
    batch.blocks.push_back(block);
    batches.push_back(batch);
  }

  return batches;
}

std::string ToolOrchestrator::TruncateResult(const std::string& result,
                                             int maxSize) {
  if (maxSize <= 0 || static_cast<int>(result.size()) <= maxSize) {
    return result;
  }

  std::size_t cutAt = static_cast<std::size_t>(maxSize);
  std::size_t lastNewline = result.find_last_of('\n', cutAt);
  if (lastNewline != std::string::npos && lastNewline > cutAt / 2) {
    cutAt = lastNewline;
  }

  std::ostringstream truncated;
  truncated << result.substr(0, cutAt);
  truncated << "\n... [truncated " << (result.size() - cutAt)
            << " bytes]";
  return truncated.str();
}

std::string ToolOrchestrator::ExecuteToolBlock(
    const core::ContentBlock& block,
    int maxResultSize,
    std::string* error) const {
  const std::string& name = block.asToolUse.name;
  const std::string& inputJson = block.asToolUse.inputJson;

  const auto CanonicalizeToolAlias = [](const std::string& toolName) {
    if (CaseInsensitiveCompare(toolName, "write_file") ||
        CaseInsensitiveCompare(toolName, "file_write")) {
      return std::string("Write");
    }
    if (CaseInsensitiveCompare(toolName, "read_file") ||
        CaseInsensitiveCompare(toolName, "file_read")) {
      return std::string("Read");
    }
    return toolName;
  };

  const std::string resolvedName = CanonicalizeToolAlias(name);

  if (CaseInsensitiveCompare(resolvedName, "Bash")) {
    return ExecuteBash(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "FileRead") ||
      CaseInsensitiveCompare(resolvedName, "Read")) {
    return ExecuteFileRead(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "FileWrite") ||
      CaseInsensitiveCompare(resolvedName, "Write")) {
    return ExecuteFileWrite(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "Grep")) {
    return ExecuteGrep(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "Glob")) {
    return ExecuteGlob(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "Agent")) {
    return ExecuteAgent(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "TodoWrite")) {
    return ExecuteTodoWrite(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "TaskCreate")) {
    return ExecuteTaskCreate(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "TaskGet")) {
    return ExecuteTaskGet(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "TaskUpdate")) {
    return ExecuteTaskUpdate(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "TaskList")) {
    return ExecuteTaskList(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "TaskStop")) {
    return ExecuteTaskStop(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "AskUserQuestion")) {
    return ExecuteAskUserQuestion(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "FileEdit")) {
    return ExecuteFileEdit(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "NotebookEdit")) {
    return ExecuteNotebookEdit(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "Skill")) {
    return ExecuteSkill(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "ListMcpResources")) {
    return ExecuteListMcpResources(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "ReadMcpResource")) {
    return ExecuteReadMcpResource(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "WebFetch")) {
    return ExecuteWebFetch(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "WebSearch")) {
    return ExecuteWebSearch(inputJson, maxResultSize, error);
  }

  if (error) {
    *error = "unknown tool: " + resolvedName;
  }
  return std::string();
}

std::string ToolOrchestrator::ExecuteBash(const std::string& inputJson,
                                          int maxResultSize,
                                          std::string* error) const {
  std::string command = JsonGetStringMultiKey(inputJson, {"command", "cmd"});
  if (command.empty()) {
    if (error) *error = "Bash tool requires 'command' parameter";
    return std::string();
  }

  // Parse optional per-call timeout (ms) from LLM input, aligned with local-ace.
  // Falls back to the global bashTimeoutMs_ if not specified.
  int callTimeoutMs = JsonGetInt(inputJson, "timeout", 0);
  if (callTimeoutMs <= 0) {
    // Also try "is_background" boolean: background commands get a longer default timeout
    callTimeoutMs = bashTimeoutMs_;
  } else {
    // Clamp to [5s, 600s] to prevent abuse
    if (callTimeoutMs < 5000) callTimeoutMs = 5000;
    if (callTimeoutMs > 600000) callTimeoutMs = 600000;
  }

  const std::string normalizedCommand = NormalizeWindowsShellCommand(command);

  infra::ProcessRunOptions options;
  options.executable = "powershell.exe";
  options.arguments = {"-NoProfile", "-Command", normalizedCommand};
  if (!workspaceRoot_.empty()) {
    options.workingDirectory = workspaceRoot_;
  }
  options.timeoutMs = static_cast<unsigned long>(callTimeoutMs);

  infra::ProcessRunResult result = processRunner_.Run(options);

  std::ostringstream output;
  if (normalizedCommand != command) {
    output << "[normalized command] " << normalizedCommand << "\n";
  }
  if (result.spawnFailed) {
    if (error) *error = result.errorMessage;
    output << "Error: " << result.errorMessage;
  } else if (result.timedOut) {
    if (error) *error = "command timed out after " + std::to_string(callTimeoutMs / 1000) + "s";
    output << "Error: command timed out after " << (callTimeoutMs / 1000) << "s\n";
    output << result.stdoutText;
  } else {
    output << result.stdoutText;
    if (!result.stderrText.empty()) {
      if (!result.stdoutText.empty() && result.stdoutText.back() != '\n') {
        output << "\n";
      }
      output << result.stderrText;
    }
    if (result.exitCode != 0) {
      if (error) {
        *error = "command exited with code " + std::to_string(result.exitCode);
      }
      output << "\n[exit code: " << result.exitCode << "]";
    }
  }

  std::string finalOutput = output.str();

  // GEMMA-ENHANCE: Auto-append API discovery hints when Python library errors
  // are detected in Bash output. This gives the model immediate guidance to
  // check the library version and discover actual API signatures, instead of
  // blindly guessing parameter names (a major defect observed in Gemma-4-31B
  // logs with pyecharts).
  if (result.exitCode != 0 || !result.stderrText.empty()) {
    std::string lowerOutput = finalOutput;
    std::transform(lowerOutput.begin(), lowerOutput.end(),
                   lowerOutput.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    bool isPythonApiError = false;
    std::string libraryHint;
    if (lowerOutput.find("typeerror") != std::string::npos &&
        (lowerOutput.find("unexpected keyword argument") != std::string::npos ||
         lowerOutput.find("got an unexpected") != std::string::npos ||
         lowerOutput.find("takes") != std::string::npos)) {
      isPythonApiError = true;
      libraryHint = "TypeError with unexpected keyword argument";
    } else if (lowerOutput.find("attributeerror") != std::string::npos &&
               (lowerOutput.find("has no attribute") != std::string::npos ||
                lowerOutput.find("module") != std::string::npos)) {
      isPythonApiError = true;
      libraryHint = "AttributeError (missing method/attribute)";
    }
    if (isPythonApiError) {
      finalOutput +=
          "\n\n[API Discovery Hint] The error (" + libraryHint +
          ") suggests a library API version mismatch. Before fixing:\n"
          "1. Run: pip show <library_name>   (check installed version)\n"
          "2. Run: python -c \"import <lib>; help(<lib>.<class>)\" "
          "(discover actual API)\n"
          "3. READ the library source file to find correct parameter names\n"
          "Do NOT guess parameter names from memory.";
    }
  }

  return TruncateResult(finalOutput, maxResultSize > 0 ? maxResultSize
                                                        : kMaxToolResultTruncation);
}

std::string ToolOrchestrator::ExecuteFileRead(const std::string& inputJson,
                                              int maxResultSize,
                                              std::string* error) const {
  const std::string rawPath =
      JsonGetStringMultiKey(inputJson, {"file_path", "path"});
  if (rawPath.empty()) {
    if (error) *error = "FileRead tool requires 'file_path' parameter";
    return std::string();
  }

  std::string resolveError;
  const std::string filePath =
      ResolveToolPath(rawPath, workspaceRoot_, false, &resolveError);
  if (filePath.empty()) {
    if (error) *error = resolveError;
    return "Error: " + resolveError;
  }

  DWORD attrs = GetFileAttributesW(ToWide(filePath).c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    if (error) *error = "file not found: " + filePath;
    return "Error: file not found: " + filePath;
  }

  if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
    std::ostringstream listing;
    auto entries = GlobFiles(filePath, "*");
    std::sort(entries.begin(), entries.end(),
              [](const FileEntry& a, const FileEntry& b) {
                if (a.isDirectory != b.isDirectory)
                  return a.isDirectory;
                return a.name < b.name;
              });
    listing << "Directory listing for " << filePath << ":\n";
    for (const auto& entry : entries) {
      listing << (entry.isDirectory ? "[DIR]  " : "[FILE] ")
              << entry.name;
      if (!entry.isDirectory) {
        listing << " (" << entry.size << " bytes)";
      }
      listing << "\n";
    }
    return TruncateResult(listing.str(), maxResultSize);
  }

  const int offset = JsonGetInt(inputJson, "offset", 0);
  const int limit = JsonGetInt(inputJson, "limit", 0);
  if (offset < 0) {
    if (error) *error = "Read tool offset must be >= 0";
    return "Error: Read tool offset must be >= 0";
  }
  if (limit < 0) {
    if (error) *error = "Read tool limit must be >= 0";
    return "Error: Read tool limit must be >= 0";
  }
  if (offset == 0 && limit == 0 && attrs != INVALID_FILE_ATTRIBUTES) {
    LARGE_INTEGER size;
    size.QuadPart = 0;
    HANDLE sizeHandle = CreateFileW(ToWide(filePath).c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE |
                                        FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (sizeHandle != INVALID_HANDLE_VALUE) {
      if (GetFileSizeEx(sizeHandle, &size) && size.QuadPart > kMaxFullReadBytes) {
        CloseHandle(sizeHandle);
        std::ostringstream oversized;
        oversized << "Error: file too large to read in full (" << size.QuadPart
                  << " bytes): " << filePath
                  << ". Use Read with offset/limit to inspect a targeted line "
                     "range, or use Grep to search first.";
        if (error) *error = oversized.str();
        return oversized.str();
      }
      CloseHandle(sizeHandle);
    }
  }

  std::string readErr;
  std::string content = ReadFileContent(filePath, &readErr);
  if (content.empty() && !readErr.empty()) {
    if (error) *error = readErr;
    return "Error: " + readErr;
  }

  const std::string normalized = NormalizeLineEndings(content);
  if (offset > 0 || limit > 0) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= normalized.size()) {
      const std::size_t next = normalized.find('\n', start);
      if (next == std::string::npos) {
        lines.push_back(normalized.substr(start));
        break;
      }
      lines.push_back(normalized.substr(start, next - start));
      start = next + 1;
      if (start == normalized.size()) {
        lines.push_back(std::string());
        break;
      }
    }

    const int startLine = std::max(1, offset == 0 ? 1 : offset);
    const int lastLine = limit > 0
                             ? std::min(static_cast<int>(lines.size()),
                                        startLine + limit - 1)
                             : static_cast<int>(lines.size());

    std::ostringstream output;
    output << "<file path=\"" << filePath << "\"";
    output << " start_line=\"" << startLine << "\"";
    if (limit > 0) {
      output << " line_count=\"" << std::max(0, lastLine - startLine + 1)
             << "\"";
    }
    output << ">\n";
    if (startLine > static_cast<int>(lines.size())) {
      output << "(requested range is beyond end of file)\n";
    } else {
      for (int line = startLine; line <= lastLine; ++line) {
        output << line << "->" << lines[static_cast<std::size_t>(line - 1)]
               << "\n";
      }
    }
    output << "</file>";
    return TruncateResult(output.str(), maxResultSize);
  }

  std::ostringstream output;
  output << "<file path=\"" << filePath << "\">\n";
  output << normalized;
  if (!content.empty() && content.back() != '\n') {
    output << "\n";
  }
  output << "</file>";

  return TruncateResult(output.str(), maxResultSize);
}

std::string ToolOrchestrator::ExecuteFileWrite(const std::string& inputJson,
                                               int maxResultSize,
                                               std::string* error) const {
  const std::string rawPath =
      JsonGetStringMultiKey(inputJson, {"file_path", "path"});
  std::string content = JsonGetString(inputJson, "content");

  if (rawPath.empty()) {
    if (error) *error = "FileWrite tool requires 'file_path' parameter";
    return std::string();
  }
  if (content.empty()) {
    if (error) *error = "FileWrite tool requires 'content' parameter";
    return std::string();
  }

  std::string resolveError;
  const std::string filePath =
      ResolveToolPath(rawPath, workspaceRoot_, true, &resolveError);
  if (filePath.empty()) {
    if (error) *error = resolveError;
    return "Error: " + resolveError;
  }

  DWORD attrs = GetFileAttributesW(ToWide(filePath).c_str());
  bool existed = (attrs != INVALID_FILE_ATTRIBUTES);

  std::string writeErr;
  if (!WriteFileContent(filePath, content, &writeErr)) {
    if (error) *error = writeErr;
    return "Error: " + writeErr;
  }

  std::ostringstream output;
  output << (existed ? "Updated" : "Created") << " file: " << filePath
         << "\nWrote " << content.size() << " bytes.";
  return TruncateResult(output.str(), maxResultSize);
}

std::string ToolOrchestrator::ExecuteGrep(const std::string& inputJson,
                                          int maxResultSize,
                                          std::string* error) const {
  std::string pattern = JsonGetStringMultiKey(inputJson, {"pattern", "query"});
  std::string searchPath = JsonGetStringMultiKey(inputJson, {"path", "directory"});
  if (searchPath.empty()) {
    searchPath = workspaceRoot_.empty() ? "." : workspaceRoot_;
  } else {
    std::string resolveError;
    const std::string resolved =
        ResolveToolPath(searchPath, workspaceRoot_, false, &resolveError);
    if (resolved.empty()) {
      if (error) *error = resolveError;
      return "Error: " + resolveError;
    }
    searchPath = resolved;
  }

  if (pattern.empty()) {
    if (error) *error = "Grep tool requires 'pattern' parameter";
    return std::string();
  }

  const int kMaxGrepMatches = 100;
  std::ostringstream output;
  int matchCount = 0;

  DWORD attrs = GetFileAttributesW(ToWide(searchPath).c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    if (error) *error = "path not found: " + searchPath;
    return "Error: path not found: " + searchPath;
  }

  if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
    GrepDirectory(searchPath, pattern, kMaxGrepMatches, &matchCount, &output);
  } else {
    output << GrepFile(searchPath, pattern, kMaxGrepMatches);
  }

  std::string result = output.str();
  if (result.empty()) {
    result = "No matches found for pattern: " + pattern;
  }
  return TruncateResult(result, maxResultSize);
}

std::string ToolOrchestrator::ExecuteGlob(const std::string& inputJson,
                                          int maxResultSize,
                                          std::string* error) const {
  std::string pattern = JsonGetStringMultiKey(inputJson, {"pattern", "glob"});
  std::string directory = JsonGetString(inputJson, "path");
  if (directory.empty()) {
    directory = workspaceRoot_.empty() ? "." : workspaceRoot_;
  } else {
    std::string resolveError;
    const std::string resolved =
        ResolveToolPath(directory, workspaceRoot_, false, &resolveError);
    if (resolved.empty()) {
      if (error) *error = resolveError;
      return "Error: " + resolveError;
    }
    directory = resolved;
  }

  if (pattern.empty()) {
    if (error) *error = "Glob tool requires 'pattern' parameter";
    return std::string();
  }

  auto entries = GlobFiles(directory, pattern);
  std::sort(entries.begin(), entries.end(),
            [](const FileEntry& a, const FileEntry& b) {
              if (a.isDirectory != b.isDirectory)
                return a.isDirectory;
              return a.name < b.name;
            });

  std::ostringstream output;
  output << "Found " << entries.size() << " files matching '"
         << pattern << "' in " << directory << ":\n";
  for (const auto& entry : entries) {
    output << (entry.isDirectory ? "[DIR]  " : "[FILE] ")
           << entry.name;
    if (!entry.isDirectory) {
      output << " (" << entry.size << " bytes)";
    }
    output << "\n";
  }

  if (entries.empty()) {
    output << "(no matches)";
  }
  return TruncateResult(output.str(), maxResultSize);
}

std::string ToolOrchestrator::ExecuteAgent(const std::string& inputJson,
                                           int maxResultSize,
                                           std::string* error) const {
  if (!subAgentManager_) {
    if (error) *error = "SubAgentManager not set on ToolOrchestrator";
    return "Error: Agent tool requires SubAgentManager configuration";
  }

  std::string prompt = JsonGetStringMultiKey(inputJson, {"prompt", "description"});
  std::string subagentType = JsonGetString(inputJson, "subagent_type");
  std::string isolation = JsonGetString(inputJson, "isolation");

  bool runInBackground = JsonGetBool(inputJson, "run_in_background");

  if (prompt.empty()) {
    if (error) *error = "Agent tool requires 'prompt' parameter";
    return "Error: missing prompt";
  }

  std::string result =
      subAgentManager_->RunAsyncAgentLifecycle(
          prompt, inputJson, subagentType, runInBackground,
          isolation, {});

  return TruncateResult(result, maxResultSize > 0 ? maxResultSize : 400000);
}

ToolOrchestrator::ExecuteResult ToolOrchestrator::Execute(
    const std::vector<core::ContentBlock>& toolUseBlocks,
    core::CanUseTool canUseTool,
    const std::vector<core::Message>& messages) const {
  ExecuteResult result;
  const std::vector<ToolBatch> batches = PartitionToolCalls(toolUseBlocks);

  std::vector<core::Message> accumulatedMessages = messages;

  for (const auto& batch : batches) {
    for (const auto& block : batch.blocks) {
      const std::vector<core::Message>& decisionMessages =
          batch.concurrentSafe ? messages : accumulatedMessages;

      core::PermissionDecision decision =
          canUseTool(block, decisionMessages);

      if (decision.behavior == core::PermissionBehavior::Deny) {
        core::Message deniedMsg;
        deniedMsg.role = core::MessageRole::User;
        deniedMsg.content.push_back(core::ContentBlock::MakeToolResult(
            block.asToolUse.id,
            "Tool denied: " + decision.reason,
            true));
        result.userMessages.push_back(deniedMsg);
        result.deniedCount++;
        accumulatedMessages.push_back(deniedMsg);
        continue;
      }

      if (decision.behavior == core::PermissionBehavior::Ask) {
        core::Message askMsg;
        askMsg.role = core::MessageRole::User;
        askMsg.content.push_back(core::ContentBlock::MakeToolResult(
            block.asToolUse.id,
            "Tool requires confirmation in non-interactive skeleton mode: " +
                decision.reason,
            true));
        result.userMessages.push_back(askMsg);
        result.deniedCount++;
        result.errorCount++;
        accumulatedMessages.push_back(askMsg);
        continue;
      }

      int maxSize = kDefaultMaxResultChars;
      if (toolRegistry_) {
        maxSize = toolRegistry_->MaxResultSizeChars(block.asToolUse.name);
      }

      std::string execError;
      std::string output =
          ExecuteToolBlock(block, maxSize, &execError);

      bool isError = !execError.empty();

      core::Message toolMsg;
      toolMsg.role = core::MessageRole::User;
      toolMsg.content.push_back(core::ContentBlock::MakeToolResult(
          block.asToolUse.id, output, isError));
      result.userMessages.push_back(toolMsg);
      if (isError) {
        result.errorCount++;
        // P0-03: Post-tool-use failure hooks (aligned with local-ace)
        // Run hooks when a tool fails so that corrective actions can be taken.
        if (hookExecutor_) {
          hookExecutor_->RunPostToolUseFailureHooks(
              block.asToolUse.name,
              block.asToolUse.inputJson,
              block.asToolUse.id,
              execError,
              1,
              10000);  // 10s timeout for hooks
        }
      }
      accumulatedMessages.push_back(toolMsg);
    }
  }

  return result;
}

std::string ToolOrchestrator::ExecuteTodoWrite(
    const std::string& inputJson,
    int maxResultSize,
    std::string* error) const {
  if (error) *error = "";
  json payload;
  try {
    payload = json::parse(inputJson.empty() ? "{}" : inputJson);
  } catch (...) {
    if (error) *error = "TodoWrite input must be valid JSON";
    return "Error: TodoWrite input must be valid JSON";
  }

  json incoming = payload.value("todos", json::array());
  if (!incoming.is_array()) incoming = json::array();

  json tasks = LoadTaskStore(workspaceRoot_);
  if (!payload.value("merge", false)) {
    tasks = incoming;
  } else {
    for (const auto& item : incoming) {
      const std::string id = item.value("id", std::string());
      if (id.empty()) continue;
      const int index = FindTaskIndex(tasks, id);
      if (index >= 0) {
        tasks[static_cast<std::size_t>(index)] = item;
      } else {
        tasks.push_back(item);
      }
    }
  }

  std::string writeError;
  if (!SaveTaskStore(workspaceRoot_, tasks, &writeError)) {
    if (error) *error = writeError;
    return "Error: " + writeError;
  }

  const std::string summary = payload.value("summary", std::string());

  // P0-1: Check for tasks being marked completed without verification.
  // If a task is being marked as completed and has acceptance_criteria,
  // append a reminder about the criteria to the tool result.
  std::string criteriaReminder;
  if (!payload.value("merge", false)) {
    // Full replace mode: check all tasks being set to completed
    for (const auto& item : tasks.is_array() ? tasks : json::array()) {
      if (item.value("status", std::string()) == "completed") {
        const std::string criteria = item.value("acceptance_criteria", std::string());
        const std::string content = item.value("content",
            item.value("subject", std::string()));
        if (!criteria.empty()) {
          criteriaReminder += "\n  - Task '" + content + "': " + criteria;
        }
      }
    }
  } else {
    // Merge mode: check only tasks being updated to completed
    for (const auto& item : incoming.is_array() ? incoming : json::array()) {
      if (item.value("status", std::string()) == "completed") {
        const std::string criteria = item.value("acceptance_criteria", std::string());
        const std::string content = item.value("content",
            item.value("subject", std::string()));
        if (!criteria.empty()) {
          criteriaReminder += "\n  - Task '" + content + "': " + criteria;
        }
      }
    }
  }

  // P1-3: Verification nudge - when all tasks are completed but none was
  // a verification step, remind the model to verify before finishing.
  // Aligned with local-ace's verificationNudgeNeeded mechanism.
  bool allDone = true;
  bool hasVerificationStep = false;
  int completedCount = 0;
  if (tasks.is_array()) {
    for (const auto& item : tasks) {
      const std::string status = item.value("status", std::string());
      const std::string content = item.value("content",
          item.value("subject", std::string()));
      if (status != "completed") {
        allDone = false;
      } else {
        ++completedCount;
      }
      // Check if any task looks like a verification step
      std::string lowerContent = content;
      std::transform(lowerContent.begin(), lowerContent.end(),
                     lowerContent.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (lowerContent.find("verif") != std::string::npos ||
          lowerContent.find("test") != std::string::npos ||
          lowerContent.find("run") != std::string::npos ||
          lowerContent.find("check") != std::string::npos ||
          lowerContent.find("build") != std::string::npos) {
        hasVerificationStep = true;
      }
    }
  }

  std::string verificationNudge;
  if (allDone && completedCount >= 3 && !hasVerificationStep) {
    verificationNudge =
        "\n\nNOTE: You just closed out " + std::to_string(completedCount) +
        "+ tasks and none of them was a verification step (run/test/check/build). "
        "Before writing your final summary, you should verify the implementation "
        "by running the code, executing tests, or checking the output. "
        "Do NOT self-assign completion without evidence - run a verification command.";
  }

  std::string result = summary.empty()
      ? "Todo list updated.\n" + RenderTaskSummary(tasks)
      : summary + "\n" + RenderTaskSummary(tasks);

  if (!criteriaReminder.empty()) {
    result += "\n\nREMINDER: You marked task(s) as completed. Please verify the following acceptance criteria were met:"
        + criteriaReminder
        + "\nIf any criteria were NOT verified (e.g., code not run, tests not executed),"
        " run the appropriate verification commands before considering the task done.";
  }

  result += verificationNudge;

  return TruncateResult(result, maxResultSize);
}

std::string ToolOrchestrator::ExecuteTaskCreate(const std::string& inputJson,
                                                int maxResultSize,
                                                std::string* error) const {
  json payload;
  try {
    payload = json::parse(inputJson.empty() ? "{}" : inputJson);
  } catch (...) {
    if (error) *error = "TaskCreate input must be valid JSON";
    return "Error: TaskCreate input must be valid JSON";
  }
  const std::string subject = payload.value("subject", std::string());
  const std::string description = payload.value("description", std::string());
  if (subject.empty() || description.empty()) {
    if (error) *error = "TaskCreate requires subject and description";
    return "Error: missing subject or description";
  }

  json tasks = LoadTaskStore(workspaceRoot_);
  json task = json::object();
  task["id"] = NextTaskId(tasks);
  task["subject"] = subject;
  task["description"] = description;
  task["activeForm"] = payload.value("activeForm", std::string());
  task["status"] = "pending";
  task["blockedBy"] = json::array();
  task["owner"] = "";
  task["metadata"] = payload.value("metadata", json::object());
  tasks.push_back(task);

  std::string writeError;
  if (!SaveTaskStore(workspaceRoot_, tasks, &writeError)) {
    if (error) *error = writeError;
    return "Error: " + writeError;
  }
  return TruncateResult("Task #" + task["id"].get<std::string>() +
                            " created successfully: " + subject,
                        maxResultSize);
}

std::string ToolOrchestrator::ExecuteTaskGet(const std::string& inputJson,
                                             int maxResultSize,
                                             std::string* error) const {
  const std::string taskId = JsonGetString(inputJson, "id");
  if (taskId.empty()) {
    if (error) *error = "TaskGet requires id";
    return "Error: missing id";
  }
  const json tasks = LoadTaskStore(workspaceRoot_);
  const int index = FindTaskIndex(tasks, taskId);
  if (index < 0) {
    if (error) *error = "task not found";
    return "Error: task not found";
  }
  return TruncateResult(tasks[static_cast<std::size_t>(index)].dump(2),
                        maxResultSize);
}

std::string ToolOrchestrator::ExecuteTaskUpdate(const std::string& inputJson,
                                                int maxResultSize,
                                                std::string* error) const {
  json payload;
  try {
    payload = json::parse(inputJson.empty() ? "{}" : inputJson);
  } catch (...) {
    if (error) *error = "TaskUpdate input must be valid JSON";
    return "Error: TaskUpdate input must be valid JSON";
  }
  const std::string taskId = payload.value("id", std::string());
  if (taskId.empty()) {
    if (error) *error = "TaskUpdate requires id";
    return "Error: missing id";
  }

  json tasks = LoadTaskStore(workspaceRoot_);
  const int index = FindTaskIndex(tasks, taskId);
  if (index < 0) {
    if (error) *error = "task not found";
    return "Error: task not found";
  }

  json& task = tasks[static_cast<std::size_t>(index)];
  const std::string oldStatus = task.value("status", std::string("pending"));
  const char* keys[] = {"subject", "description", "activeForm", "status",
                        "owner"};
  for (const char* key : keys) {
    if (payload.contains(key) && payload[key].is_string()) task[key] = payload[key];
  }
  if (payload.contains("blockedBy") && payload["blockedBy"].is_array()) {
    task["blockedBy"] = payload["blockedBy"];
  }
  if (payload.contains("metadata") && payload["metadata"].is_object()) {
    task["metadata"] = payload["metadata"];
  }
  // P1-1: Support acceptance_criteria and error_message fields
  if (payload.contains("acceptance_criteria") && payload["acceptance_criteria"].is_string()) {
    task["acceptance_criteria"] = payload["acceptance_criteria"];
  }
  if (payload.contains("error_message") && payload["error_message"].is_string()) {
    task["error_message"] = payload["error_message"];
  }

  // P1-1: State transition validation
  const std::string newStatus = task.value("status", oldStatus);
  if (newStatus != oldStatus) {
    // Valid transitions:
    // pending -> in_progress (start work)
    // in_progress -> completed (finish successfully)
    // in_progress -> failed (encounter error)
    // failed -> retrying (retry after error)
    // retrying -> in_progress (retry accepted)
    // retrying -> failed (retry failed)
    // failed -> completed (manual override after verification)
    // Any status -> pending (reset)
    static const std::map<std::string, std::set<std::string>> validTransitions = {
      {"pending", {"in_progress"}},
      {"in_progress", {"completed", "failed", "pending"}},
      {"failed", {"retrying", "completed", "pending"}},
      {"retrying", {"in_progress", "failed", "pending"}},
      {"completed", {"pending"}},  // Allow reopening completed tasks
    };
    auto it = validTransitions.find(oldStatus);
    if (it != validTransitions.end() &&
        it->second.find(newStatus) == it->second.end()) {
      // Invalid transition - still allow it but log a warning
      task["status_warning"] = "Transition from " + oldStatus + " to " +
          newStatus + " is unusual";
    }

    // Track retry count for failed -> retrying transitions
    if (newStatus == "retrying") {
      int retryCount = task.value("retry_count", 0);
      task["retry_count"] = retryCount + 1;
      task["last_retry_time"] = std::to_string(
          static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count()));
    }

    // Clear error_message when transitioning away from failed
    if (oldStatus == "failed" && newStatus != "failed") {
      task.erase("error_message");
    }
  }

  std::string writeError;
  if (!SaveTaskStore(workspaceRoot_, tasks, &writeError)) {
    if (error) *error = writeError;
    return "Error: " + writeError;
  }
  return TruncateResult("Task #" + taskId + " updated: " +
                            task.value("subject", std::string()),
                        maxResultSize);
}

std::string ToolOrchestrator::ExecuteTaskList(const std::string&,
                                              int maxResultSize,
                                              std::string* error) const {
  if (error) *error = "";
  const json tasks = LoadTaskStore(workspaceRoot_);
  return TruncateResult(RenderTaskSummary(tasks), maxResultSize);
}

std::string ToolOrchestrator::ExecuteTaskStop(const std::string& inputJson,
                                              int maxResultSize,
                                              std::string* error) const {
  json payload;
  try {
    payload = json::parse(inputJson.empty() ? "{}" : inputJson);
  } catch (...) {
    if (error) *error = "TaskStop input must be valid JSON";
    return "Error: TaskStop input must be valid JSON";
  }
  const std::string taskId = payload.value("id", std::string());
  if (taskId.empty()) {
    if (error) *error = "TaskStop requires id";
    return "Error: missing id";
  }

  json tasks = LoadTaskStore(workspaceRoot_);
  const int index = FindTaskIndex(tasks, taskId);
  if (index < 0) {
    if (error) *error = "task not found";
    return "Error: task not found";
  }
  tasks[static_cast<std::size_t>(index)]["status"] = "cancelled";
  tasks[static_cast<std::size_t>(index)]["stopReason"] =
      payload.value("reason", std::string("stopped by user"));

  std::string writeError;
  if (!SaveTaskStore(workspaceRoot_, tasks, &writeError)) {
    if (error) *error = writeError;
    return "Error: " + writeError;
  }
  return TruncateResult("Task #" + taskId + " stopped", maxResultSize);
}

std::string ToolOrchestrator::ExecuteAskUserQuestion(
    const std::string& inputJson,
    int maxResultSize,
    std::string* error) const {
  if (error) *error = "";
  std::string questions = JsonGetString(inputJson, "questions");
  if (questions.empty()) {
    return TruncateResult(
        "AskUserQuestion tool received. In interactive mode the user "
        "will be prompted; in non-interactive mode, proceed with best "
        "available information.",
        maxResultSize);
  }
  return TruncateResult(
      "Questions queued for user response: " + questions,
      maxResultSize);
}

std::string ToolOrchestrator::ExecuteFileEdit(
    const std::string& inputJson,
    int maxResultSize,
    std::string* error) const {
  const std::string rawPath =
      JsonGetStringMultiKey(inputJson, {"file_path", "path"});
  std::string oldStr = JsonGetString(inputJson, "old_string");
  std::string newStr = JsonGetString(inputJson, "new_string");
  bool replaceAll = JsonGetBool(inputJson, "replace_all");

  if (rawPath.empty() || oldStr.empty()) {
    if (error) *error = "FileEdit requires file_path and old_string";
    return "Error: missing required parameters";
  }

  std::string resolveError;
  const std::string filePath =
      ResolveToolPath(rawPath, workspaceRoot_, true, &resolveError);
  if (filePath.empty()) {
    if (error) *error = resolveError;
    return "Error: " + resolveError;
  }

  std::ifstream input(filePath, std::ios::binary);
  if (!input) {
    if (error) *error = "Cannot read file: " + filePath;
    return "Error: cannot read file " + filePath;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  std::string content = buffer.str();

  // Markdown files use two trailing spaces as hard line breaks — stripping
  // would silently change semantics. Skip stripTrailingWhitespace for .md/.mdx.
  const bool isMarkdown = IsMarkdownFile(filePath);

  // ---- Pre-processing: XML de-sanitization (Task 2) ----
  // Weaker LLMs (Qwen 3.6 35b) sometimes emit XML-escaped entities in
  // tool_call arguments. Reverse them before any matching.
  DesanitizeXmlEntities(oldStr);
  DesanitizeXmlEntities(newStr);

  // ---- Fuzzy matching engine (aligned with local-ace findActualString) ----
  // Multi-level fallback: exact → CRLF → quote+CRLF → whitespace+CRLF
  std::string actualOldStr = oldStr;
  std::string actualNewStr = newStr;
  bool fuzzyUsed = false;
  std::string fuzzyMethod;

  // ---- Diagnostic log: record each step's result ----
  std::ostringstream diagLog;
  diagLog << "[FileEdit] file=" << filePath
          << " old_string_len=" << oldStr.size()
          << " new_string_len=" << newStr.size()
          << " file_len=" << content.size()
          << " replace_all=" << (replaceAll ? "true" : "false")
          << " is_markdown=" << (isMarkdown ? "true" : "false") << "\n";

  // Step 1: Try exact byte-level match (fast path)
  {
    std::size_t pos = content.find(oldStr);
    if (pos != std::string::npos) {
      diagLog << "[FileEdit] Step 1 (exact match): SUCCESS at offset " << pos << "\n";
      // actualOldStr is already oldStr
    } else {
      diagLog << "[FileEdit] Step 1 (exact match): FAILED - old_string not found byte-for-byte in file\n";

      // Step 2: Normalize CRLF → LF
      if (!fuzzyUsed) {
        std::string normContent = content;
        std::string normOld = oldStr;
        NormalizeCRLF(normContent);
        NormalizeCRLF(normOld);
        int fileCrCount = 0, oldCrCount = 0;
        for (char c : content) if (c == '\r') ++fileCrCount;
        for (char c : oldStr) if (c == '\r') ++oldCrCount;
        diagLog << "[FileEdit] Step 2 (CRLF normalization): file_cr_count=" << fileCrCount
                << " old_string_cr_count=" << oldCrCount;

        std::size_t pos2 = normContent.find(normOld);
        if (pos2 != std::string::npos) {
          actualOldStr = MapNormalizedMatchToOriginal(
              content, normContent, pos2, normOld.size(), NORM_CRLF);
          actualNewStr = newStr;
          NormalizeCRLF(actualNewStr);
          fuzzyUsed = true;
          fuzzyMethod = "CRLF-normalization";
          diagLog << " -> SUCCESS at norm_offset=" << pos2 << "\n";
        } else {
          diagLog << " -> FAILED (normalized strings still don't match)\n";
        }
      }

      // Step 3: Normalize quotes (curly → straight) + CRLF
      if (!fuzzyUsed) {
        std::string normContent = content;
        std::string normOld = oldStr;
        NormalizeQuotes(normContent);
        NormalizeQuotes(normOld);
        NormalizeCRLF(normContent);
        NormalizeCRLF(normOld);

        bool fileHasCurly = content.find("\xE2\x80\x9C") != std::string::npos ||
                            content.find("\xE2\x80\x9D") != std::string::npos ||
                            content.find("\xE2\x80\x98") != std::string::npos ||
                            content.find("\xE2\x80\x99") != std::string::npos;
        bool oldHasStraight = oldStr.find('"') != std::string::npos ||
                              oldStr.find('\'') != std::string::npos;
        diagLog << "[FileEdit] Step 3 (quote+CRLF): file_has_curly="
                << (fileHasCurly ? "true" : "false")
                << " old_string_has_straight="
                << (oldHasStraight ? "true" : "false");

        std::size_t pos3 = normContent.find(normOld);
        if (pos3 != std::string::npos) {
          actualOldStr = MapNormalizedMatchToOriginal(
              content, normContent, pos3, normOld.size(),
              NORM_CRLF | NORM_QUOTES);
          actualNewStr = PreserveQuoteStyle(oldStr, actualOldStr, newStr);
          fuzzyUsed = true;
          fuzzyMethod = "quote+CRLF-normalization";
          diagLog << " -> SUCCESS\n";
        } else {
          diagLog << " -> FAILED\n";
        }
      }

      // Step 4: Strip trailing whitespace + CRLF (skip for Markdown files)
      if (!fuzzyUsed && !isMarkdown) {
        std::string strippedOld = StripTrailingWhitespace(oldStr);
        std::string strippedContent = StripTrailingWhitespace(content);
        NormalizeCRLF(strippedContent);
        NormalizeCRLF(strippedOld);

        bool oldHadTrailing = (strippedOld.size() != oldStr.size());
        diagLog << "[FileEdit] Step 4 (whitespace+CRLF): old_string_had_trailing_ws="
                << (oldHadTrailing ? "true" : "false")
                << " size_before=" << oldStr.size()
                << " size_after=" << strippedOld.size();

        std::size_t pos4 = strippedContent.find(strippedOld);
        if (pos4 != std::string::npos) {
          actualOldStr = MapNormalizedMatchToOriginal(
              content, strippedContent, pos4, strippedOld.size(),
              NORM_CRLF | NORM_WS);
          actualNewStr = newStr;
          NormalizeCRLF(actualNewStr);
          fuzzyUsed = true;
          fuzzyMethod = "whitespace+CRLF-normalization";
          diagLog << " -> SUCCESS\n";
        } else {
          diagLog << " -> FAILED\n";
        }
      } else if (!fuzzyUsed && isMarkdown) {
        diagLog << "[FileEdit] Step 4 (whitespace+CRLF): SKIPPED (.md/.mdx file)\n";
      }

      // Step 5: Full combination — quote + CRLF + whitespace
      if (!fuzzyUsed) {
        std::string normOld = oldStr;
        std::string normContent = content;

        NormalizeQuotes(normContent);
        NormalizeQuotes(normOld);

        if (!isMarkdown) {
          normOld = StripTrailingWhitespace(normOld);
          normContent = StripTrailingWhitespace(normContent);
        }

        NormalizeCRLF(normContent);
        NormalizeCRLF(normOld);

        diagLog << "[FileEdit] Step 5 (full normalization): norm_old_len="
                << normOld.size()
                << " norm_content_len=" << normContent.size();

        int fullFlags = NORM_CRLF | NORM_QUOTES;
        if (!isMarkdown) fullFlags |= NORM_WS;

        std::size_t pos5 = normContent.find(normOld);
        if (pos5 != std::string::npos) {
          actualOldStr = MapNormalizedMatchToOriginal(
              content, normContent, pos5, normOld.size(), fullFlags);
          actualNewStr = PreserveQuoteStyle(oldStr, actualOldStr, newStr);
          NormalizeCRLF(actualNewStr);
          fuzzyUsed = true;
          fuzzyMethod = "full-normalization";
          diagLog << " -> SUCCESS\n";
        } else {
          diagLog << " -> FAILED\n";
        }
      }

      // Step 6: Line-anchor-based fallback (Task 1)
      // When all fuzzy strategies fail, use the first and last non-empty
      // lines of old_string as anchors to locate the block in the file.
      // This handles cases where the LLM gets the structure right but has
      // minor whitespace/indentation discrepancies within the block.
      if (!fuzzyUsed) {
        auto splitLines = [](const std::string& s) -> std::vector<std::string> {
          std::vector<std::string> lines;
          std::istringstream iss(s);
          std::string line;
          while (std::getline(iss, line)) {
            // Normalize CRLF
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
          }
          return lines;
        };
        auto trimLine = [](const std::string& s) -> std::string {
          std::size_t start = 0;
          while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) ++start;
          std::size_t end = s.size();
          while (end > start && (s[end-1] == ' ' || s[end-1] == '\t')) --end;
          return s.substr(start, end - start);
        };

        std::vector<std::string> oldLines = splitLines(oldStr);
        std::vector<std::string> fileLines = splitLines(content);

        // Collect non-empty trimmed old lines for anchor matching
        std::vector<std::pair<int, std::string>> trimmedOldLines;
        for (int i = 0; i < static_cast<int>(oldLines.size()); ++i) {
          std::string trimmed = trimLine(oldLines[i]);
          if (!trimmed.empty()) {
            trimmedOldLines.push_back({i, trimmed});
          }
        }

        if (trimmedOldLines.size() >= 2 && fileLines.size() >= oldLines.size()) {
          // Use first and last non-empty lines as anchors
          const std::string& firstAnchor = trimmedOldLines.front().second;
          const std::string& lastAnchor = trimmedOldLines.back().second;
          int oldLineCount = static_cast<int>(oldLines.size());

          diagLog << "[FileEdit] Step 6 (line-anchor fallback): first_anchor_len="
                  << firstAnchor.size() << " last_anchor_len=" << lastAnchor.size()
                  << " old_line_count=" << oldLineCount;

          bool found = false;
          for (int fi = 0; fi <= static_cast<int>(fileLines.size()) - oldLineCount; ++fi) {
            std::string fileFirstTrimmed = trimLine(fileLines[fi]);
            if (fileFirstTrimmed != firstAnchor) continue;

            // Check last anchor
            int lastIdx = fi + oldLineCount - 1;
            if (lastIdx >= static_cast<int>(fileLines.size())) break;
            std::string fileLastTrimmed = trimLine(fileLines[lastIdx]);
            if (fileLastTrimmed != lastAnchor) continue;

            // Verify at least 50% of interior lines match (trimmed)
            int matchCount = 2; // first + last already matched
            for (std::size_t oi = 1; oi < trimmedOldLines.size() - 1; ++oi) {
              int mappedFileLine = fi + trimmedOldLines[oi].first;
              if (mappedFileLine < static_cast<int>(fileLines.size()) &&
                  trimLine(fileLines[mappedFileLine]) == trimmedOldLines[oi].second) {
                ++matchCount;
              }
            }
            int requiredMatches = std::max(2, static_cast<int>(trimmedOldLines.size()) / 2);
            if (matchCount >= requiredMatches) {
              // Found a match! Build actualOldStr from file content lines
              std::string matchedBlock;
              for (int li = fi; li <= lastIdx; ++li) {
                if (li > fi) matchedBlock += '\n';
                matchedBlock += fileLines[li];
              }
              actualOldStr = matchedBlock;
              actualNewStr = newStr;
              NormalizeCRLF(actualNewStr);
              fuzzyUsed = true;
              fuzzyMethod = "line-anchor-fallback";
              found = true;
              diagLog << " -> SUCCESS at file_line=" << fi
                      << " match_count=" << matchCount << "\n";
              break;
            }
          }
          if (!found) {
            diagLog << " -> FAILED (no anchor match found)\n";
          }
        } else {
          diagLog << "[FileEdit] Step 6 (line-anchor fallback): SKIPPED (too few lines)\n";
        }
      }
    }
  }

  // Perform the replacement using actualOldStr (which is an exact substring
  // of the original file content, found via fuzzy matching above).
  std::size_t pos = 0;
  int count = 0;
  while ((pos = content.find(actualOldStr, pos)) != std::string::npos) {
    content.replace(pos, actualOldStr.size(), actualNewStr);
    pos += actualNewStr.size();
    ++count;
    if (!replaceAll) break;
  }

  if (count == 0) {
    // ---- Task 3: Auto-recovery with file re-read ----
    // Re-read the file in case it was modified since the initial read.
    // Then retry exact matching with fresh content.
    std::ifstream reread(filePath, std::ios::binary);
    if (reread) {
      std::ostringstream freshBuf;
      freshBuf << reread.rdbuf();
      std::string freshContent = freshBuf.str();
      if (freshContent != content) {
        diagLog << "[FileEdit] Auto-recovery: file changed on disk, re-trying with fresh content\n";
        // Retry exact match with fresh content
        std::size_t retryPos = freshContent.find(oldStr);
        if (retryPos != std::string::npos) {
          // Found in fresh content! Apply replacement and write.
          if (replaceAll) {
            std::size_t rp = 0;
            int rc = 0;
            while ((rp = freshContent.find(oldStr, rp)) != std::string::npos) {
              freshContent.replace(rp, oldStr.size(), newStr);
              rp += newStr.size();
              ++rc;
            }
            std::ofstream retryOut(filePath, std::ios::binary | std::ios::trunc);
            if (retryOut) {
              retryOut << freshContent;
              std::string msg = "File edited (auto-recovery): " + filePath +
                  " (" + std::to_string(rc) + " occurrences replaced after re-read)";
              return TruncateResult(msg, maxResultSize);
            }
          } else {
            freshContent.replace(retryPos, oldStr.size(), newStr);
            std::ofstream retryOut(filePath, std::ios::binary | std::ios::trunc);
            if (retryOut) {
              retryOut << freshContent;
              std::string msg = "File edited (auto-recovery): " + filePath +
                  " (1 occurrence replaced after re-read)";
              return TruncateResult(msg, maxResultSize);
            }
          }
        }
        // Update content for error message snippet generation
        content = freshContent;
      }
    }

    // ---- Task 5: Improved error messages for LLM self-correction ----
    // Find the closest line-based match to give the LLM a useful snippet.
    std::string closestSnippet;
    {
      auto trimWs = [](const std::string& s) -> std::string {
        std::size_t a = 0;
        while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
        std::size_t b = s.size();
        while (b > a && (s[b-1] == ' ' || s[b-1] == '\t')) --b;
        return s.substr(a, b - a);
      };
      // Use the first non-empty line of old_string to find closest match
      std::istringstream oldIss(oldStr);
      std::string firstNonEmpty;
      {
        std::string line;
        while (std::getline(oldIss, line)) {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          std::string trimmed = trimWs(line);
          if (!trimmed.empty()) { firstNonEmpty = trimmed; break; }
        }
      }
      if (!firstNonEmpty.empty()) {
        std::istringstream fileIss(content);
        std::string fline;
        int lineNum = 0;
        int bestLine = -1;
        int bestDist = INT_MAX;
        while (std::getline(fileIss, fline)) {
          ++lineNum;
          if (!fline.empty() && fline.back() == '\r') fline.pop_back();
          std::string trimmed = trimWs(fline);
          // Simple distance: count of differing characters (bounded)
          int dist = 0;
          std::size_t minLen = std::min(trimmed.size(), firstNonEmpty.size());
          for (std::size_t ci = 0; ci < minLen; ++ci) {
            if (trimmed[ci] != firstNonEmpty[ci]) ++dist;
          }
          dist += static_cast<int>(std::abs(
              static_cast<int>(trimmed.size()) - static_cast<int>(firstNonEmpty.size())));
          if (dist < bestDist) {
            bestDist = dist;
            bestLine = lineNum;
          }
        }
        if (bestLine > 0) {
          // Extract a snippet around the best matching line
          std::istringstream fileIss2(content);
          std::string snippetLine;
          int curLine = 0;
          int contextBefore = 3;
          int contextAfter = 3;
          int startLine = std::max(1, bestLine - contextBefore);
          int endLine = bestLine + contextAfter;
          std::ostringstream snippetStream;
          while (std::getline(fileIss2, snippetLine)) {
            ++curLine;
            if (!snippetLine.empty() && snippetLine.back() == '\r') snippetLine.pop_back();
            if (curLine >= startLine && curLine <= endLine) {
              snippetStream << "  " << curLine << ": " << snippetLine << "\n";
            }
            if (curLine > endLine) break;
          }
          closestSnippet = snippetStream.str();
        }
      }
    }

    std::ostringstream hint;
    hint << "search string not found in " << filePath << "\n";
    hint << "\n--- Diagnostic Log ---\n";
    hint << diagLog.str();
    hint << "\n--- Attempted matching strategies ---\n";
    hint << "  1. Exact match (failed)\n";
    hint << "  2. CRLF normalization (failed)\n";
    hint << "  3. Quote + CRLF normalization (failed)\n";
    if (!isMarkdown) {
      hint << "  4. Whitespace + CRLF normalization (failed)\n";
    } else {
      hint << "  4. Whitespace + CRLF normalization (skipped: .md/.mdx file)\n";
    }
    hint << "  5. Full normalization (failed)\n";
    hint << "  6. Line-anchor fallback (failed)\n";
    hint << "\n--- Possible causes ---\n";
    hint << "  - old_string does not match any part of the file\n";
    hint << "  - Indentation mismatch (tabs vs spaces)\n";
    hint << "  - The file may have been modified since it was last read\n";
    hint << "  - old_string length: " << oldStr.size() << " bytes\n";
    hint << "  - file content length: " << content.size() << " bytes\n";
    if (!closestSnippet.empty()) {
      hint << "\n--- Closest matching region in file ---\n";
      hint << closestSnippet;
    }
    hint << "\n--- How to fix ---\n";
    hint << "  1. Use Read to get the CURRENT content of " << filePath << "\n";
    hint << "  2. Copy the EXACT text you want to replace from the Read output\n";
    hint << "  3. Retry Edit with the correct old_string matching the file exactly\n";
    hint << "  4. For large changes, consider using Write to overwrite the entire file\n";
    if (error) *error = hint.str();
    return "Error: " + hint.str();
  }

  std::ofstream output(filePath, std::ios::binary | std::ios::trunc);
  if (!output) {
    if (error) *error = "Cannot write file: " + filePath;
    return "Error: cannot write to " + filePath;
  }
  output << content;

  std::string resultMsg;
  if (replaceAll) {
    resultMsg = "File edited: " + filePath + " (" +
        std::to_string(count) + " occurrences replaced)";
  } else {
    resultMsg = "File edited: " + filePath + " (1 occurrence replaced)";
  }
  if (fuzzyUsed) {
    resultMsg += " [fuzzy-match: " + fuzzyMethod + "]";
    // Only include diagnostic log when fuzzy matching was used (avoid noise)
    resultMsg += "\n" + diagLog.str();
  }

  return TruncateResult(resultMsg, maxResultSize);
}

std::string ToolOrchestrator::ExecuteNotebookEdit(const std::string& inputJson,
                                                  int maxResultSize,
                                                  std::string* error) const {
  json payload;
  try {
    payload = json::parse(inputJson.empty() ? "{}" : inputJson);
  } catch (...) {
    if (error) *error = "NotebookEdit input must be valid JSON";
    return "Error: NotebookEdit input must be valid JSON";
  }

  const std::string rawPath = payload.value("notebook_path", std::string());
  if (rawPath.empty()) {
    if (error) *error = "NotebookEdit requires notebook_path";
    return "Error: missing notebook_path";
  }
  std::string resolveError;
  const std::string filePath =
      ResolveToolPath(rawPath, workspaceRoot_, true, &resolveError);
  if (filePath.empty()) {
    if (error) *error = resolveError;
    return "Error: " + resolveError;
  }

  std::string readError;
  const std::string raw = ReadFileContent(filePath, &readError);
  if (raw.empty() && !readError.empty()) {
    if (error) *error = readError;
    return "Error: " + readError;
  }

  json notebook;
  try {
    notebook = json::parse(raw);
  } catch (...) {
    if (error) *error = "Notebook is not valid JSON";
    return "Error: notebook is not valid JSON";
  }
  if (!notebook.contains("cells") || !notebook["cells"].is_array()) {
    if (error) *error = "Notebook does not contain a cells array";
    return "Error: notebook does not contain a cells array";
  }

  const std::string editMode = payload.value("edit_mode", std::string("replace"));
  const std::string cellId = payload.value("cell_id", std::string());
  const std::string newSource = payload.value("new_source", std::string());
  const std::string cellType = payload.value("cell_type", std::string("code"));

  json& cells = notebook["cells"];
  int cellIndex = -1;
  if (!cellId.empty()) {
    for (std::size_t i = 0; i < cells.size(); ++i) {
      if (cells[i].is_object() && cells[i].value("id", std::string()) == cellId) {
        cellIndex = static_cast<int>(i);
        break;
      }
    }
  }

  if (editMode == "replace") {
    if (cellIndex < 0) {
      if (error) *error = "replace requires an existing cell_id";
      return "Error: replace requires an existing cell_id";
    }
    cells[static_cast<std::size_t>(cellIndex)]["source"] = json::array({newSource});
    if (!cellType.empty()) {
      cells[static_cast<std::size_t>(cellIndex)]["cell_type"] = cellType;
    }
  } else if (editMode == "insert") {
    json newCell = json::object();
    newCell["id"] = cellId.empty()
        ? ("cell-" + std::to_string(cells.size() + 1))
        : ("cell-" + cellId + "-new");
    newCell["cell_type"] = cellType;
    newCell["metadata"] = json::object();
    newCell["source"] = json::array({newSource});
    if (cellType == "code") {
      newCell["execution_count"] = nullptr;
      newCell["outputs"] = json::array();
    }
    if (cellIndex < 0) {
      cells.insert(cells.begin(), newCell);
    } else {
      cells.insert(cells.begin() + cellIndex + 1, newCell);
    }
  } else if (editMode == "delete") {
    if (cellIndex < 0) {
      if (error) *error = "delete requires an existing cell_id";
      return "Error: delete requires an existing cell_id";
    }
    cells.erase(cells.begin() + cellIndex);
  } else {
    if (error) *error = "Unsupported notebook edit mode";
    return "Error: unsupported notebook edit mode";
  }

  std::string writeError;
  if (!WriteFileContent(filePath, notebook.dump(2), &writeError)) {
    if (error) *error = writeError;
    return "Error: " + writeError;
  }
  return TruncateResult("Notebook edited successfully: " + filePath,
                        maxResultSize);
}

std::string ToolOrchestrator::ExecuteSkill(const std::string& inputJson,
                                           int maxResultSize,
                                           std::string* error) const {
  const std::string command = JsonGetString(inputJson, "command");
  const std::string args = JsonGetString(inputJson, "args");
  if (command.empty()) {
    if (error) *error = "Skill requires command";
    return "Error: missing command";
  }
  json agentInput = json::object();
  agentInput["prompt"] = args.empty()
      ? ("Execute the skill `" + command + "` and complete the requested work.")
      : args;
  agentInput["description"] = "skill:" + command;
  const std::string lower = ToLowerAscii(command);
  if (lower == "plan" || lower == "explore" || lower == "verification" ||
      lower == "general-purpose" || lower == "claude-code-guide") {
    agentInput["subagent_type"] = lower;
  } else {
    agentInput["subagent_type"] = "general-purpose";
  }
  return ExecuteAgent(agentInput.dump(), maxResultSize, error);
}

std::string ToolOrchestrator::ExecuteListMcpResources(
    const std::string& inputJson,
    int maxResultSize,
    std::string* error) const {
  if (!mcpClientManager_) {
    if (error) *error = "McpClientManager not configured";
    return "Error: MCP resource tools require McpClientManager configuration";
  }
  const std::string targetServer = JsonGetString(inputJson, "server");
  std::vector<mcp::McpResourceSchema> resources;
  const std::vector<mcp::McpServerConnection> connections =
      mcpClientManager_->connections();
  for (const auto& connection : connections) {
    if (!targetServer.empty() && connection.name != targetServer) continue;
    mcpClientManager_->RefreshResourcesFromTransport(connection.name);
    std::vector<mcp::McpResourceSchema> current =
        mcpClientManager_->FetchResourcesForClient(connection.name);
    resources.insert(resources.end(), current.begin(), current.end());
  }
  if (resources.empty()) return "No resources found";
  std::ostringstream out;
  for (std::size_t i = 0; i < resources.size(); ++i) {
    const auto& resource = resources[i];
    out << resource.serverName << ": " << resource.name
        << " <" << resource.uri << ">";
    if (!resource.mimeType.empty()) out << " [" << resource.mimeType << "]";
    if (i + 1 < resources.size()) out << "\n";
  }
  return TruncateResult(out.str(), maxResultSize);
}

std::string ToolOrchestrator::ExecuteReadMcpResource(
    const std::string& inputJson,
    int maxResultSize,
    std::string* error) const {
  if (!mcpClientManager_) {
    if (error) *error = "McpClientManager not configured";
    return "Error: MCP resource tools require McpClientManager configuration";
  }
  const std::string server = JsonGetString(inputJson, "server");
  const std::string uri = JsonGetString(inputJson, "uri");
  if (server.empty() || uri.empty()) {
    if (error) *error = "ReadMcpResource requires server and uri";
    return "Error: missing server or uri";
  }
  std::string bodyJson;
  std::string readError;
  if (!mcpClientManager_->ReadResourceFromTransport(
          server, uri, &bodyJson, &readError)) {
    if (error) *error = readError;
    return "Error: " + readError;
  }
  return TruncateResult(bodyJson, maxResultSize);
}

std::string ToolOrchestrator::ExecuteWebFetch(
    const std::string& inputJson,
    int maxResultSize,
    std::string* error) const {
  std::string url = JsonGetString(inputJson, "url");
  if (url.empty()) {
    if (error) *error = "WebFetch requires 'url' parameter";
    return "Error: missing url";
  }
  if (!(StartsWithNoCase(url, "http://") || StartsWithNoCase(url, "https://"))) {
    if (error) *error = "WebFetch only supports http/https URLs";
    return "Error: unsupported URL scheme";
  }

  HttpResponse response;
  std::string requestError;
  if (!HttpGet(url, &response, &requestError)) {
    if (error) *error = requestError;
    return "Error: " + requestError;
  }
  if (response.statusCode >= 400) {
    if (error) *error = "HTTP " + std::to_string(response.statusCode);
    return "Error: HTTP " + std::to_string(response.statusCode);
  }

  std::string result;
  if (response.contentType.find("text/html") != std::string::npos ||
      response.contentType.empty()) {
    result = BuildMarkdownFromHtml(url, response.body);
  } else {
    result = "Source: " + url + "\n\n" + response.body;
  }
  if (error) *error = "";
  return TruncateResult(result, maxResultSize);
}

std::string ToolOrchestrator::ExecuteWebSearch(
    const std::string& inputJson,
    int maxResultSize,
    std::string* error) const {
  std::string query = JsonGetString(inputJson, "query");
  if (query.empty()) {
    if (error) *error = "WebSearch requires 'query' parameter";
    return "Error: missing query";
  }
  int num = 5;
  try {
    json parsed = json::parse(inputJson);
    if (parsed.contains("num") && parsed["num"].is_number_integer()) {
      num = parsed["num"].get<int>();
    }
  } catch (...) {
  }
  if (num < 1) num = 1;
  if (num > 10) num = 10;

  const std::string searchUrl =
      "https://www.bing.com/search?q=" + UrlEncode(query);
  HttpResponse response;
  std::string requestError;
  if (!HttpGet(searchUrl, &response, &requestError)) {
    if (error) *error = requestError;
    return "Error: " + requestError;
  }
  if (response.statusCode >= 400) {
    if (error) *error = "HTTP " + std::to_string(response.statusCode);
    return "Error: HTTP " + std::to_string(response.statusCode);
  }

  const std::vector<std::pair<std::string, std::string> > results =
      ParseSearchResults(response.body, num);
  if (results.empty()) {
    if (error) *error = "no search results parsed";
    return "Error: no search results parsed";
  }

  std::ostringstream output;
  output << "Search query: " << query << "\n";
  output << "Source: Bing HTML\n\n";
  for (std::size_t i = 0; i < results.size(); ++i) {
    output << (i + 1) << ". " << results[i].first << "\n";
    output << results[i].second << "\n\n";
  }

  if (error) *error = "";
  return TruncateResult(output.str(), maxResultSize);
}

}  // namespace tools
}  // namespace agent