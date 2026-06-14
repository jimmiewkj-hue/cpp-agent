#pragma once

// Centralized string utilities for cpp-agent.
// Consolidates Utf8ToWide/WideToUtf8/Trim/SplitLines/EscapeJson and other
// helpers that were previously duplicated across main.cpp, ModelClient.cpp,
// ProcessRunner.cpp, QueryLoop.cpp, ToolOrchestrator.cpp, McpClientManager.cpp,
// SubAgentManager.cpp, MemoryIndex.cpp, MemoryScanner.cpp, and PermissionEngine.cpp.

#include <string>
#include <vector>

namespace agent {
namespace infra {

// ---------- Encoding conversions (Windows: UTF-8 <-> UTF-16) ----------

// Convert a UTF-8 string to a wide string (UTF-16 on Windows, UTF-32 on Linux).
std::wstring Utf8ToWide(const std::string& text);

// Convert a wide string back to UTF-8.
std::string WideToUtf8(const std::wstring& text);

// ---------- Whitespace / trimming ----------

// Trim leading and trailing whitespace (space, tab, CR, LF).
// Also strips UTF-8 BOM if present at the beginning.
std::string Trim(const std::string& value);

// Trim only leading whitespace.
std::string TrimLeft(const std::string& value);

// Trim only trailing whitespace.
std::string TrimRight(const std::string& value);

// ---------- Splitting / joining ----------

// Split a string by newline into lines. Empty input returns empty vector.
// A trailing newline does not produce an extra empty line.
std::vector<std::string> SplitLines(const std::string& text);

// Split a string by an arbitrary delimiter.
std::vector<std::string> SplitString(const std::string& text,
                                     const std::string& delimiter);

// Join strings with a separator.
std::string JoinStrings(const std::vector<std::string>& parts,
                        const std::string& separator);

// ---------- JSON escaping ----------

// Escape a string for safe embedding inside a JSON string value.
// Handles: quotes, backslash, newlines, carriage returns, tabs, and
// control characters (< 0x20) as \uXXXX.
std::string EscapeJson(const std::string& s);

// ---------- Case conversion ----------

// Convert ASCII characters to lowercase.
std::string ToLower(const std::string& text);

// Convert ASCII characters to uppercase.
std::string ToUpper(const std::string& text);

// ---------- Truncation ----------

// Truncate a string to maxLen characters, appending "..." if truncated.
std::string Shorten(const std::string& value, std::size_t maxLength);

// ---------- Path helpers ----------

// Return the parent directory of a path. Works with both / and \ separators.
std::string ParentPath(const std::string& path);

// Join two path segments with the platform separator.
std::string JoinPath(const std::string& lhs, const std::string& rhs);

// Check if a path is absolute (drive letter on Windows, leading / on POSIX).
bool IsAbsolutePath(const std::string& path);

// Normalize all separators to the platform default.
std::string NormalizeSeparators(const std::string& path);

// ---------- Misc ----------

// Check if a string starts with a given prefix.
bool StartsWith(const std::string& text, const std::string& prefix);

// Check if a string ends with a given suffix.
bool EndsWith(const std::string& text, const std::string& suffix);

// Check if a string contains a substring (case-sensitive).
bool Contains(const std::string& text, const std::string& needle);

// Case-insensitive substring search (ASCII only).
bool ContainsIgnoreCase(const std::string& text, const std::string& needle);

// Check if an environment variable value is a truthy boolean.
// Recognizes: "1", "true", "TRUE", "yes", "YES", "on", "ON".
bool IsTruthyEnvValue(const std::string& value);

}  // namespace infra
}  // namespace agent
