#include "tools/FileHelpers.h"
#include "infra/StringUtil.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#endif

namespace agent {
namespace tools {
namespace detail {

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
      const std::wstring wideCurrent = infra::Utf8ToWide(current);
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
  const std::wstring widePath = infra::Utf8ToWide(path);
  DWORD length = GetFullPathNameW(widePath.c_str(),
                                  static_cast<DWORD>(buffer.size()),
                                  &buffer[0], nullptr);
  if (length == 0 || length >= buffer.size()) return std::string();
  return NormalizeSeparators(infra::WideToUtf8(std::wstring(&buffer[0], length)));
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
  const std::string trimmed = infra::Trim(requestedPath);
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
        const std::string lowerTag = ToLowerAscii(infra::Trim(tag));
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
  return infra::Trim(text);
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
  if (!WinHttpCrackUrl(infra::Utf8ToWide(url).c_str(), 0, 0, &components)) {
    return false;
  }

  parsed->secure = components.nScheme == INTERNET_SCHEME_HTTPS;
  parsed->port = components.nPort;
  parsed->host = infra::WideToUtf8(
      std::wstring(components.lpszHostName, components.dwHostNameLength));
  parsed->path = infra::WideToUtf8(
      std::wstring(components.lpszUrlPath, components.dwUrlPathLength));
  parsed->path += infra::WideToUtf8(
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
  *value = infra::WideToUtf8(buffer);
  return true;
}

static const wchar_t* kWebUserAgent = L"cpp-agent/1.0";

bool HttpGet(const std::string& url,
             HttpResponse* response,
             std::string* error,
             int redirectDepth) {
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
      session, infra::Utf8ToWide(parsed.host).c_str(), parsed.port, 0);
  if (!connection) {
    if (error) *error = "WinHttpConnect failed";
    WinHttpCloseHandle(session);
    return false;
  }

  DWORD flags = parsed.secure ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET request = WinHttpOpenRequest(
      connection, L"GET", infra::Utf8ToWide(parsed.path).c_str(), nullptr,
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
  return infra::Trim(StripTags(html.substr(start + 1, end - start - 1)));
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
    std::string text = infra::Trim(HtmlToText(html.substr(tagEnd + 1, close - tagEnd - 1)));
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
                          const std::string& fallback) {
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
                                  const std::string& fallback) {
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
                 bool fallback) {
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
               int fallback) {
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
  return JoinPath(infra::WideToUtf8(std::wstring(&cwd[0], length)), ".cpp-agent");
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
  HANDLE handle = CreateFileW(infra::Utf8ToWide(path).c_str(), GENERIC_READ,
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
  HANDLE handle = CreateFileW(infra::Utf8ToWide(path).c_str(), GENERIC_WRITE, 0, nullptr,
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
  HANDLE findHandle = FindFirstFileW(infra::Utf8ToWide(searchPath).c_str(), &findData);
  if (findHandle == INVALID_HANDLE_VALUE) {
    return;
  }

  do {
    const std::string name = infra::WideToUtf8(findData.cFileName);
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
  const DWORD attrs = GetFileAttributesW(infra::Utf8ToWide(searchRoot).c_str());
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
            infra::Utf8ToWide(searchRoot).c_str(), GetFileExInfoStandard, &data)) {
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
  HANDLE findHandle = FindFirstFileW(infra::Utf8ToWide(searchPath).c_str(), &findData);
  if (findHandle == INVALID_HANDLE_VALUE) return;

  do {
    if (*matchCount >= maxMatches) break;
    const std::string fileName = infra::WideToUtf8(findData.cFileName);
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
    int flags) {
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

}  // namespace detail
}  // namespace tools
}  // namespace agent
