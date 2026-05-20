#include "tools/ToolOrchestrator.h"

#include "agents/SubAgentManager.h"
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
static const wchar_t* kWebUserAgent = L"cpp-agent/1.0";

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

  std::istringstream stream(trimmed);
  std::vector<std::string> tokens;
  std::string token;
  while (stream >> token) tokens.push_back(token);
  if (tokens.empty()) return trimmed;
  if (!StartsWithCaseInsensitive(tokens[0], "ls")) return trimmed;

  bool useForce = false;
  std::vector<std::string> paths;
  for (std::size_t i = 1; i < tokens.size(); ++i) {
    const std::string& current = tokens[i];
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
    normalized << " -Path '" << path << "'";
  }
  return normalized.str();
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
      const DWORD attrs = GetFileAttributesA(current.c_str());
      if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryA(current.c_str(), nullptr) &&
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
  char buffer[MAX_PATH] = {0};
  DWORD length = GetFullPathNameA(path.c_str(), MAX_PATH, buffer, nullptr);
  if (length == 0 || length >= MAX_PATH) return std::string();
  return NormalizeSeparators(std::string(buffer, length));
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
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (error) *error = "failed to open file: " + path;
    return std::string();
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

bool WriteFileContent(const std::string& path,
                      const std::string& content,
                      std::string* error) {
  const std::string parent = ParentPath(path);
  if (!parent.empty() && !EnsureDirectoryRecursive(parent)) {
    if (error) *error = "failed to create parent directory: " + parent;
    return false;
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    if (error) *error = "failed to write file: " + path;
    return false;
  }
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!output.good()) {
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

std::vector<FileEntry> GlobFiles(const std::string& directory,
                                 const std::string& pattern) {
  std::vector<FileEntry> entries;
  std::string searchPath = directory;
  if (!searchPath.empty() && searchPath.back() != '\\') {
    searchPath.push_back('\\');
  }
  searchPath += pattern;

  WIN32_FIND_DATAA findData;
  HANDLE findHandle = FindFirstFileA(searchPath.c_str(), &findData);
  if (findHandle == INVALID_HANDLE_VALUE) {
    return entries;
  }

  do {
    if (std::strcmp(findData.cFileName, ".") == 0 ||
        std::strcmp(findData.cFileName, "..") == 0) {
      continue;
    }
    FileEntry entry;
    entry.name = findData.cFileName;
    entry.isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    LARGE_INTEGER size;
    size.LowPart = findData.nFileSizeLow;
    size.HighPart = findData.nFileSizeHigh;
    entry.size = size.QuadPart;
    entries.push_back(entry);
  } while (FindNextFileA(findHandle, &findData));

  FindClose(findHandle);
  return entries;
}

std::string GrepFile(const std::string& filePath,
                     const std::string& pattern,
                     int maxMatches) {
  std::ifstream input(filePath);
  if (!input) return std::string();

  std::ostringstream result;
  std::string line;
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

  WIN32_FIND_DATAA findData;
  HANDLE findHandle = FindFirstFileA(searchPath.c_str(), &findData);
  if (findHandle == INVALID_HANDLE_VALUE) return;

  do {
    if (*matchCount >= maxMatches) break;
    if (std::strcmp(findData.cFileName, ".") == 0 ||
        std::strcmp(findData.cFileName, "..") == 0) {
      continue;
    }
    std::string fullPath = dirPath;
    if (!fullPath.empty() && fullPath.back() != '\\') fullPath.push_back('\\');
    fullPath += findData.cFileName;

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
  } while (FindNextFileA(findHandle, &findData));

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
  if (CaseInsensitiveCompare(name, "AskUserQuestion")) {
    return ExecuteAskUserQuestion(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "FileEdit")) {
    return ExecuteFileEdit(inputJson, maxResultSize, error);
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

  DWORD attrs = GetFileAttributesA(filePath.c_str());
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

  std::string readErr;
  std::string content = ReadFileContent(filePath, &readErr);
  if (content.empty() && !readErr.empty()) {
    if (error) *error = readErr;
    return "Error: " + readErr;
  }

  std::ostringstream output;
  output << "<file path=\"" << filePath << "\">\n";
  output << NormalizeLineEndings(content);
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

  DWORD attrs = GetFileAttributesA(filePath.c_str());
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

  DWORD attrs = GetFileAttributesA(searchPath.c_str());
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
            false));
        result.userMessages.push_back(askMsg);
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
    const std::string&,
    int maxResultSize,
    std::string* error) const {
  if (error) *error = "";
  return TruncateResult(
      "Todo list updated. Use the todo list to track your progress.",
      maxResultSize);
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
