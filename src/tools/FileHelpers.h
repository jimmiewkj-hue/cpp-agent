#pragma once

// File I/O, path manipulation, HTTP, JSON, task store, and glob helpers
// for ToolOrchestrator. Extracted from ToolOrchestrator.cpp (Task 7.2).

#include <string>
#include <utility>
#include <vector>

#include <windows.h>
#include <winhttp.h>

#include "third_party/nlohmann_json.hpp"

namespace agent {
namespace tools {
namespace detail {

// Path manipulation
std::string ParentPath(const std::string& path);
std::string JoinPath(const std::string& lhs, const std::string& rhs);
bool IsAbsolutePath(const std::string& path);
std::string NormalizeSeparators(std::string path);
std::string ToLowerAscii(std::string value);
std::string EnsureTrailingSeparator(std::string path);
std::string GetFullPathString(const std::string& path);
bool IsPathWithinWorkspace(const std::string& workspaceRoot,
                           const std::string& candidate);
std::string ResolveToolPath(const std::string& requestedPath,
                            const std::string& workspaceRoot,
                            bool requireInsideWorkspace,
                            std::string* error);
bool EnsureDirectoryRecursive(const std::string& path);

// File I/O
std::string ReadFileContent(const std::string& path, std::string* error);
bool WriteFileContent(const std::string& path,
                      const std::string& content,
                      std::string* error);

// Glob/file listing
struct FileEntry {
  std::string name;
  bool isDirectory = false;
  long long size = 0;
};

std::vector<FileEntry> GlobFiles(const std::string& directory,
                                 const std::string& pattern);
bool WildcardMatch(const std::string& text, const std::string& pattern);

// Grep
std::string GrepFile(const std::string& filePath,
                     const std::string& pattern,
                     int maxMatches);
void GrepDirectory(const std::string& dirPath,
                   const std::string& pattern,
                   int maxMatches,
                   int* matchCount,
                   std::ostringstream* output);

// String helpers used by file operations
bool StartsWithNoCase(const std::string& value, const std::string& prefix);
std::string ReplaceAll(std::string value,
                       const std::string& from,
                       const std::string& to);

// FileEdit fuzzy matching helpers
enum NormalizeFlags {
  NORM_CRLF   = 1 << 0,
  NORM_QUOTES = 1 << 1,
  NORM_WS     = 1 << 2,
};

void DesanitizeXmlEntities(std::string& s);
bool IsMarkdownFile(const std::string& path);
void NormalizeCRLF(std::string& s);
void NormalizeQuotes(std::string& s);
std::string StripTrailingWhitespace(const std::string& s);
std::string MapNormalizedMatchToOriginal(
    const std::string& original,
    const std::string& normalized,
    std::size_t normPos,
    std::size_t normLen,
    int flags = NORM_CRLF);
std::string PreserveQuoteStyle(
    const std::string& modelOldStr,
    const std::string& actualOldStr,
    const std::string& modelNewStr);

// HTML stripping (used by WebFetch)
std::string StripTags(const std::string& html);
std::string HtmlToText(const std::string& html);

// URL encoding/decoding
std::string UrlEncode(const std::string& value);
std::string UrlDecode(const std::string& value);

// URL parsing
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

bool ParseUrl(const std::string& url, ParsedUrl* parsed);
bool HttpGet(const std::string& url,
             HttpResponse* response,
             std::string* error,
             int redirectDepth = 0);
std::string ExtractHtmlTitle(const std::string& html);
std::vector<std::pair<std::string, std::string>> ParseSearchResults(
    const std::string& html, int maxResults);
std::string BuildMarkdownFromHtml(const std::string& url,
                                  const std::string& html);

// JSON helpers
std::string JsonGetString(const std::string& jsonStr,
                          const std::string& key,
                          const std::string& fallback = std::string());
std::string JsonGetStringMultiKey(const std::string& jsonStr,
                                  const std::vector<std::string>& keys,
                                  const std::string& fallback = std::string());
bool JsonGetBool(const std::string& jsonStr,
                 const std::string& key,
                 bool fallback = false);
int JsonGetInt(const std::string& jsonStr,
               const std::string& key,
               int fallback = 0);

// Task store helpers
std::string GetStateRootForTools(const std::string& workspaceRoot);
std::string GetTaskStorePath(const std::string& workspaceRoot);
using json = nlohmann::json;
json LoadTaskStore(const std::string& workspaceRoot);
bool SaveTaskStore(const std::string& workspaceRoot,
                   const json& tasks,
                   std::string* error);
int FindTaskIndex(const json& tasks, const std::string& taskId);
std::string NextTaskId(const json& tasks);
std::string RenderTaskSummary(const json& tasks);

// Line ending normalization
std::string NormalizeLineEndings(const std::string& input);

}  // namespace detail
}  // namespace tools
}  // namespace agent
