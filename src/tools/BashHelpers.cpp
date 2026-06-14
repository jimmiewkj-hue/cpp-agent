#include "tools/BashHelpers.h"
#include "infra/StringUtil.h"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace agent {
namespace tools {
namespace detail {

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

namespace {
std::string ToLowerAscii(std::string value) {
  for (std::size_t i = 0; i < value.size(); ++i) {
    value[i] = static_cast<char>(
        std::tolower(static_cast<unsigned char>(value[i])));
  }
  return value;
}
}  // namespace

std::string NormalizeWindowsShellCommand(const std::string& command) {
  const std::string trimmed = infra::Trim(command);
  if (trimmed.empty()) return trimmed;

  // GEMMA-ENHANCE: Translate common Linux/Bash shell idioms to PowerShell
  // equivalents BEFORE any other normalization.
  {
    std::string result = trimmed;
    bool shellModified = false;

    // && -> ; (PowerShell statement separator)
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
          out.push_back(';');
          ++i;
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

    // 1>/dev/null -> $null and >/dev/null -> $null
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
      return NormalizeWindowsShellCommand(result);
    }
  }

  // Handle piped Unix commands (| grep, | head, | tail)
  {
    std::string result = trimmed;
    bool modified = false;

    std::size_t grepPos = result.find("| grep ");
    if (grepPos != std::string::npos) {
      std::size_t contentStart = grepPos + 7;
      std::size_t patternEnd = result.find(" |", contentStart);
      if (patternEnd == std::string::npos) patternEnd = result.size();
      std::string grepContent = result.substr(contentStart, patternEnd - contentStart);

      bool caseInsensitive = false;
      bool invertMatch = false;
      std::string accumulatedPattern;
      std::istringstream iss(grepContent);
      std::string token;
      bool inPattern = false;
      while (iss >> token) {
        if (token.size() >= 2 && token[0] == '-' && !inPattern) {
          for (size_t ci = 1; ci < token.size(); ++ci) {
            if (token[ci] == 'i' || token[ci] == 'I') caseInsensitive = true;
            if (token[ci] == 'v' || token[ci] == 'V') invertMatch = true;
          }
        } else {
          inPattern = true;
          if (!accumulatedPattern.empty()) accumulatedPattern += " ";
          accumulatedPattern += token;
        }
      }

      std::string cleanedPattern = accumulatedPattern;
      while (!cleanedPattern.empty() && cleanedPattern.back() == ' ') cleanedPattern.pop_back();
      while (!cleanedPattern.empty() && cleanedPattern.front() == ' ') cleanedPattern.erase(0, 1);
      if (cleanedPattern.size() >= 2 &&
          cleanedPattern.front() == '"' && cleanedPattern.back() == '"') {
        cleanedPattern = cleanedPattern.substr(1, cleanedPattern.size() - 2);
      }

      if (!cleanedPattern.empty()) {
        std::ostringstream psArgs;
        psArgs << "| Select-String -Pattern "
               << QuoteForPowerShellSingleQuoted(cleanedPattern);
        if (caseInsensitive) psArgs << " -CaseSensitive:$false";
        if (invertMatch) psArgs << " -NotMatch";
        result = result.substr(0, grepPos) + psArgs.str() + result.substr(patternEnd);
        modified = true;
      }
    }

    // | head -N -> | Select-Object -First N
    std::size_t headPos = result.find("| head ");
    if (headPos != std::string::npos) {
      std::size_t numStart = headPos + 7;
      if (numStart < result.size() && result[numStart] == '-') {
        ++numStart;
        std::size_t numEnd = numStart;
        while (numEnd < result.size() && std::isdigit(static_cast<unsigned char>(result[numEnd]))) ++numEnd;
        if (numEnd > numStart) {
          std::string count = result.substr(numStart, numEnd - numStart);
          result = result.substr(0, headPos) + "| Select-Object -First " + count + result.substr(numEnd);
          modified = true;
        }
      } else {
        result = result.substr(0, headPos) + "| Select-Object -First 10" + result.substr(numStart);
        modified = true;
      }
    }

    // | tail -N -> | Select-Object -Last N
    std::size_t tailPos = result.find("| tail ");
    if (tailPos != std::string::npos) {
      std::size_t numStart = tailPos + 7;
      if (numStart < result.size() && result[numStart] == '-') {
        ++numStart;
        std::size_t numEnd = numStart;
        while (numEnd < result.size() && std::isdigit(static_cast<unsigned char>(result[numEnd]))) ++numEnd;
        if (numEnd > numStart) {
          std::string count = result.substr(numStart, numEnd - numStart);
          result = result.substr(0, tailPos) + "| Select-Object -Last " + count + result.substr(numEnd);
          modified = true;
        }
      } else {
        result = result.substr(0, tailPos) + "| Select-Object -Last 10" + result.substr(numStart);
        modified = true;
      }
    }

    if (modified) return result;
  }

  const std::vector<ShellToken> tokens = TokenizeShellCommand(trimmed);
  if (tokens.empty()) return trimmed;
  const std::string commandName = ToLowerAscii(tokens[0].text);

  if (commandName == "pwd") return "Get-Location";

  if (commandName == "which" && tokens.size() >= 2) {
    return "Get-Command " + QuoteForPowerShellSingleQuoted(tokens[1].text) +
           " | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue";
  }

  if (commandName == "rm" && tokens.size() >= 2) {
    bool recurse = false, force = false;
    std::vector<std::string> paths;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::string& cur = tokens[i].text;
      if (!cur.empty() && cur[0] == '-') {
        for (std::size_t j = 1; j < cur.size(); ++j) {
          char f = static_cast<char>(std::tolower(static_cast<unsigned char>(cur[j])));
          if (f == 'r') recurse = true;
          if (f == 'f') force = true;
        }
        continue;
      }
      paths.push_back(cur);
    }
    if (paths.empty()) return trimmed;
    std::ostringstream n;
    n << "Remove-Item";
    if (recurse) n << " -Recurse";
    if (force) n << " -Force";
    for (const auto& p : paths) n << " -Path " << QuoteForPowerShellSingleQuoted(p);
    return n.str();
  }

  if (commandName == "cp" && tokens.size() >= 3) {
    bool recurse = false;
    std::vector<std::string> args;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::string& cur = tokens[i].text;
      if (!cur.empty() && cur[0] == '-') {
        for (std::size_t j = 1; j < cur.size(); ++j) {
          if (std::tolower(static_cast<unsigned char>(cur[j])) == 'r') recurse = true;
        }
        continue;
      }
      args.push_back(cur);
    }
    if (args.size() < 2) return trimmed;
    std::string dest = args.back(); args.pop_back();
    std::ostringstream n;
    n << "Copy-Item";
    if (recurse) n << " -Recurse";
    for (const auto& s : args) n << " -Path " << QuoteForPowerShellSingleQuoted(s);
    n << " -Destination " << QuoteForPowerShellSingleQuoted(dest);
    return n.str();
  }

  if (commandName == "mv" && tokens.size() >= 3) {
    std::vector<std::string> args;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      if (!tokens[i].text.empty() && tokens[i].text[0] == '-') continue;
      args.push_back(tokens[i].text);
    }
    if (args.size() < 2) return trimmed;
    std::string dest = args.back(); args.pop_back();
    std::ostringstream n;
    n << "Move-Item";
    for (const auto& s : args) n << " -Path " << QuoteForPowerShellSingleQuoted(s);
    n << " -Destination " << QuoteForPowerShellSingleQuoted(dest);
    return n.str();
  }

  if (commandName == "ls") {
    bool useForce = false;
    std::vector<std::string> paths;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::string& cur = tokens[i].text;
      if (!cur.empty() && cur[0] == '-') {
        for (std::size_t j = 1; j < cur.size(); ++j) {
          char f = static_cast<char>(std::tolower(static_cast<unsigned char>(cur[j])));
          if (f == 'a') useForce = true;
          else if (f == 'l' || f == 'h') { /* skip */ }
          else return trimmed;
        }
        continue;
      }
      paths.push_back(cur);
    }
    std::ostringstream n;
    n << "Get-ChildItem";
    if (useForce) n << " -Force";
    for (const auto& p : paths) n << " -Path " << QuoteForPowerShellSingleQuoted(p);
    n << " | Select-Object Mode,LastWriteTime,Length,Name";
    return n.str();
  }

  if (commandName == "dir") {
    bool bareNames = false, filesOnly = false, dirsOnly = false, recurse = false, useForce = false;
    std::vector<std::string> paths;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      std::string cur = ToLowerAscii(tokens[i].text);
      if (!cur.empty() && (cur[0] == '/' || cur[0] == '-')) {
        if (cur == "/b" || cur == "-b") { bareNames = true; continue; }
        if (cur == "/s" || cur == "-s") { recurse = true; continue; }
        if (cur == "/a" || cur == "-a") { useForce = true; continue; }
        if (cur == "/a-d" || cur == "-a-d" || cur == "/a:-d" || cur == "-a:-d") { filesOnly = true; continue; }
        if (cur == "/ad" || cur == "-ad" || cur == "/a:d" || cur == "-a:d" || cur == "/a+d" || cur == "-a+d") { dirsOnly = true; continue; }
        return trimmed;
      }
      paths.push_back(tokens[i].text);
    }
    if (filesOnly && dirsOnly) return trimmed;
    std::ostringstream n;
    n << "Get-ChildItem";
    if (useForce) n << " -Force";
    if (recurse) n << " -Recurse";
    if (filesOnly) n << " -File";
    if (dirsOnly) n << " -Directory";
    for (const auto& p : paths) n << " -Path " << QuoteForPowerShellSingleQuoted(p);
    if (bareNames) n << " | ForEach-Object { $_.Name }";
    else n << " | Select-Object Mode,LastWriteTime,Length,Name";
    return n.str();
  }

  if (commandName == "cat") {
    std::ostringstream n;
    n << "Get-Content";
    for (std::size_t i = 1; i < tokens.size(); ++i) n << " " << QuoteForPowerShellSingleQuoted(tokens[i].text);
    return n.str();
  }

  if (commandName == "head" && tokens.size() >= 2) {
    int count = 10;
    std::string filePath;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::string& tok = tokens[i].text;
      if (tok.size() >= 2 && tok[0] == '-' && std::isdigit(static_cast<unsigned char>(tok[1])))
        count = std::atoi(tok.substr(1).c_str());
      else if (tok == "-n" && i + 1 < tokens.size())
        count = std::atoi(tokens[++i].text.c_str());
      else filePath = tok;
    }
    if (!filePath.empty())
      return "Get-Content " + QuoteForPowerShellSingleQuoted(filePath) + " -First " + std::to_string(count);
    return "Select-Object -First " + std::to_string(count);
  }

  if (commandName == "tail" && tokens.size() >= 2) {
    int count = 10;
    std::string filePath;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::string& tok = tokens[i].text;
      if (tok.size() >= 2 && tok[0] == '-' && std::isdigit(static_cast<unsigned char>(tok[1])))
        count = std::atoi(tok.substr(1).c_str());
      else if (tok == "-n" && i + 1 < tokens.size())
        count = std::atoi(tokens[++i].text.c_str());
      else filePath = tok;
    }
    if (!filePath.empty())
      return "Get-Content " + QuoteForPowerShellSingleQuoted(filePath) + " -Last " + std::to_string(count);
    return "Select-Object -Last " + std::to_string(count);
  }

  if (commandName == "wc" && tokens.size() >= 2) {
    if (tokens[1].text == "-l" && tokens.size() >= 3)
      return "(Get-Content " + QuoteForPowerShellSingleQuoted(tokens[2].text) + " | Measure-Object -Line).Lines";
  }

  if (commandName == "touch" && tokens.size() >= 2)
    return "New-Item -ItemType File -Force -Path " + QuoteForPowerShellSingleQuoted(tokens[1].text) + " | Out-Null";

  if (commandName == "mkdir" && tokens.size() >= 2) {
    bool recursive = false;
    std::vector<ShellToken> paths;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::string& cur = tokens[i].text;
      if (cur == "-p" || cur == "--parents") { recursive = true; continue; }
      if (!cur.empty() && cur[0] == '-') continue;
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
      const std::string braceContent = path.substr(braceStart + 1, braceEnd - braceStart - 1);
      std::size_t commaPos = 0, searchStart = 0;
      while (true) {
        commaPos = braceContent.find(',', searchStart);
        std::string part = braceContent.substr(searchStart,
            commaPos == std::string::npos ? std::string::npos : commaPos - searchStart);
        part = infra::Trim(part);
        if (!part.empty()) { expandedPaths.push_back(prefix + part + suffix); expandedAny = true; }
        if (commaPos == std::string::npos) break;
        searchStart = commaPos + 1;
      }
      if (!expandedAny) expandedPaths.push_back(path);
    }
    std::ostringstream n;
    n << "New-Item -ItemType Directory";
    if (recursive) n << " -Force";
    for (const auto& p : expandedPaths) n << " -Path " << QuoteForPowerShellSingleQuoted(p);
    return n.str();
  }

  if (commandName == "find" && tokens.size() >= 2)
    return "Get-ChildItem -Recurse -Name " + QuoteForPowerShellSingleQuoted(tokens[1].text);

  if (commandName == "grep" && tokens.size() >= 2) {
    std::ostringstream n;
    bool caseInsensitive = false, invertMatch = false;
    std::string pattern;
    std::vector<std::string> paths;
    bool inFlags = true;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::string& tok = tokens[i].text;
      if (inFlags) {
        if (tok == "-i" || tok == "--ignore-case") { caseInsensitive = true; continue; }
        if (tok == "-v" || tok == "--invert-match") { invertMatch = true; continue; }
        if (tok.size() >= 2 && tok[0] == '-' && tok.find_first_not_of("-iIvVeEwWFf") == std::string::npos) {
          for (size_t ci = 1; ci < tok.size(); ++ci) {
            if (tok[ci] == 'i' || tok[ci] == 'I') caseInsensitive = true;
            if (tok[ci] == 'v' || tok[ci] == 'V') invertMatch = true;
          }
          continue;
        }
        inFlags = false;
      }
      if (pattern.empty()) pattern = tok;
      else paths.push_back(tok);
    }
    if (pattern.empty()) return trimmed;
    n << "Select-String -Pattern " << QuoteForPowerShellSingleQuoted(pattern);
    if (caseInsensitive) n << " -CaseSensitive:$false";
    if (invertMatch) n << " -NotMatch";
    for (const auto& p : paths) n << " -Path " << QuoteForPowerShellSingleQuoted(p);
    return n.str();
  }

  if (command.find("/dev/null") != std::string::npos) {
    std::string rewritten = command;
    for (size_t pos = 0; (pos = rewritten.find("/dev/null", pos)) != std::string::npos; pos += 5)
      rewritten.replace(pos, 9, "");
    return rewritten;
  }

  return trimmed;
}

}  // namespace detail
}  // namespace tools
}  // namespace agent
