#include "infra/StringUtil.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace agent {
namespace infra {

// =====================================================================
// Encoding conversions
// =====================================================================

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty()) return {};
#ifdef _WIN32
  const int sz = MultiByteToWideChar(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  if (sz <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(sz), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                      static_cast<int>(text.size()), &wide[0], sz);
  return wide;
#else
  // On Linux/macOS, wchar_t is typically UTF-32; for now we do a simple
  // byte-copy which is correct for ASCII and good enough for path operations
  // when the filesystem APIs accept UTF-8 directly.
  return std::wstring(text.begin(), text.end());
#endif
}

std::string WideToUtf8(const std::wstring& text) {
  if (text.empty()) return {};
#ifdef _WIN32
  const int sz = WideCharToMultiByte(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (sz <= 0) return {};
  std::string utf8(static_cast<std::size_t>(sz), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                      static_cast<int>(text.size()), &utf8[0], sz,
                      nullptr, nullptr);
  return utf8;
#else
  return std::string(text.begin(), text.end());
#endif
}

// =====================================================================
// Whitespace / trimming
// =====================================================================

namespace {
constexpr const char* kWhitespace = " \t\r\n";
}  // namespace

std::string TrimLeft(const std::string& value) {
  const std::size_t start = value.find_first_not_of(kWhitespace);
  if (start == std::string::npos) return {};
  return value.substr(start);
}

std::string TrimRight(const std::string& value) {
  const std::size_t end = value.find_last_not_of(kWhitespace);
  if (end == std::string::npos) return {};
  return value.substr(0, end + 1);
}

std::string Trim(const std::string& value) {
  std::size_t begin = value.find_first_not_of(kWhitespace);
  if (begin == std::string::npos) return {};
  // Strip UTF-8 BOM if present
  if (value.size() >= begin + 3 &&
      static_cast<unsigned char>(value[begin]) == 0xEF &&
      static_cast<unsigned char>(value[begin + 1]) == 0xBB &&
      static_cast<unsigned char>(value[begin + 2]) == 0xBF) {
    begin += 3;
    begin = value.find_first_not_of(kWhitespace, begin);
    if (begin == std::string::npos) return {};
  }
  const std::size_t end = value.find_last_not_of(kWhitespace);
  return value.substr(begin, end - begin + 1);
}

// =====================================================================
// Splitting / joining
// =====================================================================

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(line);
  }
  if (lines.empty() && !text.empty()) lines.push_back(text);
  return lines;
}

std::vector<std::string> SplitString(const std::string& text,
                                     const std::string& delimiter) {
  std::vector<std::string> parts;
  if (delimiter.empty()) {
    parts.push_back(text);
    return parts;
  }
  std::size_t start = 0;
  std::size_t pos = 0;
  while ((pos = text.find(delimiter, start)) != std::string::npos) {
    parts.push_back(text.substr(start, pos - start));
    start = pos + delimiter.size();
  }
  parts.push_back(text.substr(start));
  return parts;
}

std::string JoinStrings(const std::vector<std::string>& parts,
                        const std::string& separator) {
  std::ostringstream oss;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) oss << separator;
    oss << parts[i];
  }
  return oss.str();
}

// =====================================================================
// JSON escaping
// =====================================================================

std::string EscapeJson(const std::string& s) {
  std::ostringstream o;
  for (char c : s) {
    switch (c) {
      case '"':  o << "\\\""; break;
      case '\\': o << "\\\\"; break;
      case '\b': o << "\\b";  break;
      case '\f': o << "\\f";  break;
      case '\n': o << "\\n";  break;
      case '\r': o << "\\r";  break;
      case '\t': o << "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          // Encode control characters as \u00XX
          static const char hex[] = "0123456789abcdef";
          o << "\\u00" << hex[(c >> 4) & 0x0F] << hex[c & 0x0F];
        } else {
          o << c;
        }
        break;
    }
  }
  return o.str();
}

// =====================================================================
// Case conversion
// =====================================================================

std::string ToLower(const std::string& text) {
  std::string result;
  result.reserve(text.size());
  for (char ch : text) {
    result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return result;
}

std::string ToUpper(const std::string& text) {
  std::string result;
  result.reserve(text.size());
  for (char ch : text) {
    result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
  }
  return result;
}

// =====================================================================
// Truncation
// =====================================================================

std::string Shorten(const std::string& value, std::size_t maxLength) {
  if (value.size() <= maxLength) return value;
  if (maxLength <= 3) return value.substr(0, maxLength);
  return value.substr(0, maxLength - 3) + "...";
}

// =====================================================================
// Path helpers
// =====================================================================

std::string ParentPath(const std::string& path) {
  const std::size_t pos = path.find_last_of("\\/");
  if (pos == std::string::npos) return {};
  if (pos == 0) return path.substr(0, 1);
#ifdef _WIN32
  // Don't strip past drive letter: "C:\" parent is "C:\"
  if (pos == 2 && path.size() >= 3 && path[1] == ':')
    return path.substr(0, 3);
#endif
  return path.substr(0, pos);
}

std::string JoinPath(const std::string& lhs, const std::string& rhs) {
  if (lhs.empty()) return rhs;
  if (rhs.empty()) return lhs;
  const char last = lhs[lhs.size() - 1];
  if (last == '\\' || last == '/') return lhs + rhs;
#ifdef _WIN32
  return lhs + "\\" + rhs;
#else
  return lhs + "/" + rhs;
#endif
}

bool IsAbsolutePath(const std::string& path) {
#ifdef _WIN32
  if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) &&
      path[1] == ':') {
    return true;
  }
  return path.size() >= 2 &&
         ((path[0] == '\\' && path[1] == '\\') ||
          (path[0] == '/' && path[1] == '/'));
#else
  return !path.empty() && path[0] == '/';
#endif
}

std::string NormalizeSeparators(const std::string& path) {
  std::string result = path;
#ifdef _WIN32
  std::replace(result.begin(), result.end(), '/', '\\');
#else
  std::replace(result.begin(), result.end(), '\\', '/');
#endif
  return result;
}

// =====================================================================
// Misc
// =====================================================================

bool StartsWith(const std::string& text, const std::string& prefix) {
  if (prefix.size() > text.size()) return false;
  return text.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(const std::string& text, const std::string& suffix) {
  if (suffix.size() > text.size()) return false;
  return text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

bool ContainsIgnoreCase(const std::string& text, const std::string& needle) {
  return ToLower(text).find(ToLower(needle)) != std::string::npos;
}

bool IsTruthyEnvValue(const std::string& value) {
  return value == "1" || value == "true" || value == "TRUE" ||
         value == "yes" || value == "YES" || value == "on" || value == "ON";
}

}  // namespace infra
}  // namespace agent
