#include "tools/ToolOrchestrator.h"

#include "agents/SubAgentManager.h"
#include "mcp/McpClientManager.h"
#include "tools/ToolRegistry.h"
#include "third_party/nlohmann_json.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

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

  const std::vector<ShellToken> tokens = TokenizeShellCommand(trimmed);
  if (tokens.empty()) return trimmed;
  const std::string commandName = ToLowerAscii(tokens[0].text);

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
    std::string count = "10";
    std::string target;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      if ((tokens[i].text == "-n" || tokens[i].text == "--lines") &&
          i + 1 < tokens.size()) {
        count = tokens[i + 1].text;
        ++i;
      } else {
        target = tokens[i].text;
      }
    }
    if (!target.empty()) {
      return "Get-Content " + QuoteForPowerShellSingleQuoted(target) +
             " -TotalCount " + count;
    }
  }

  if (commandName == "tail" && tokens.size() >= 2) {
    std::string count = "10";
    std::string target;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      if ((tokens[i].text == "-n" || tokens[i].text == "--lines") &&
          i + 1 < tokens.size()) {
        count = tokens[i + 1].text;
        ++i;
      } else {
        target = tokens[i].text;
      }
    }
    if (!target.empty()) {
      return "Get-Content " + QuoteForPowerShellSingleQuoted(target) +
             " -Tail " + count;
    }
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

  if (commandName == "grep" && tokens.size() >= 3) {
    return "Select-String -Pattern " +
           QuoteForPowerShellSingleQuoted(tokens[1].text) + " -Path " +
           QuoteForPowerShellSingleQuoted(tokens[2].text);
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
    out << "#" << task.value("id", std::string("?")) << " ["
        << task.value("status", std::string("pending")) << "] "
        << task.value("subject", task.value("content", std::string("(untitled)")));
    if (task.contains("owner") && task["owner"].is_string() &&
        !task["owner"].get<std::string>().empty()) {
      out << " (" << task["owner"].get<std::string>() << ")";
    }
    if (i + 1 < tasks.size()) out << "\n";
  }
  return out.str();
}

bool CaseInsensitiveCompare(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
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

}  // namespace

ToolOrchestrator::ToolOrchestrator() {}

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

  if (CaseInsensitiveCompare(name, "Bash")) {
    return ExecuteBash(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "FileRead") ||
      CaseInsensitiveCompare(name, "Read")) {
    return ExecuteFileRead(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "FileWrite") ||
      CaseInsensitiveCompare(name, "Write")) {
    return ExecuteFileWrite(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "Grep")) {
    return ExecuteGrep(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "Glob")) {
    return ExecuteGlob(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "Agent")) {
    return ExecuteAgent(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "TodoWrite")) {
    return ExecuteTodoWrite(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "TaskCreate")) {
    return ExecuteTaskCreate(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "TaskGet")) {
    return ExecuteTaskGet(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "TaskUpdate")) {
    return ExecuteTaskUpdate(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "TaskList")) {
    return ExecuteTaskList(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "TaskStop")) {
    return ExecuteTaskStop(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "AskUserQuestion")) {
    return ExecuteAskUserQuestion(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "FileEdit")) {
    return ExecuteFileEdit(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "NotebookEdit")) {
    return ExecuteNotebookEdit(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "Skill")) {
    return ExecuteSkill(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "ListMcpResources")) {
    return ExecuteListMcpResources(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "ReadMcpResource")) {
    return ExecuteReadMcpResource(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "WebFetch")) {
    return ExecuteWebFetch(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "WebSearch")) {
    return ExecuteWebSearch(inputJson, maxResultSize, error);
  }

  if (error) {
    *error = "unknown tool: " + name;
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

  const std::string normalizedCommand = NormalizeWindowsShellCommand(command);

  infra::ProcessRunOptions options;
  options.executable = "powershell.exe";
  options.arguments = {"-NoProfile", "-Command", normalizedCommand};
  if (!workspaceRoot_.empty()) {
    options.workingDirectory = workspaceRoot_;
  }
  options.timeoutMs = 120000;

  infra::ProcessRunResult result = processRunner_.Run(options);

  std::ostringstream output;
  if (normalizedCommand != command) {
    output << "[normalized command] " << normalizedCommand << "\n";
  }
  if (result.spawnFailed) {
    if (error) *error = result.errorMessage;
    output << "Error: " << result.errorMessage;
  } else if (result.timedOut) {
    if (error) *error = "command timed out after 120s";
    output << "Error: command timed out after 120s\n";
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
  std::string result = summary.empty()
      ? "Todo list updated.\n" + RenderTaskSummary(tasks)
      : summary + "\n" + RenderTaskSummary(tasks);
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

  std::string result;
  std::size_t pos = 0;
  int count = 0;
  while ((pos = content.find(oldStr, pos)) != std::string::npos) {
    content.replace(pos, oldStr.size(), newStr);
    pos += newStr.size();
    ++count;
    if (!replaceAll) break;
  }

  if (count == 0) {
    if (error) *error = "old_string not found in file";
    return "Error: search string not found in " + filePath;
  }

  std::ofstream output(filePath, std::ios::binary | std::ios::trunc);
  if (!output) {
    if (error) *error = "Cannot write file: " + filePath;
    return "Error: cannot write to " + filePath;
  }
  output << content;

  if (replaceAll) {
    return TruncateResult(
        "File edited: " + filePath + " (" + std::to_string(count) +
            " occurrences replaced)",
        maxResultSize);
  }
  return TruncateResult(
      "File edited: " + filePath + " (1 occurrence replaced)",
      maxResultSize);
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
