#include "permissions/PathValidator.h"

#include <algorithm>
#include <cctype>
#include <windows.h>

namespace agent {
namespace permissions {

const std::vector<std::string> PathValidator::kDangerousPatterns = {
  "..\\..\\..", "../../..",
  "\\Windows\\System32", "/Windows/System32",
  "\\etc\\passwd", "/etc/passwd",
  "\\etc\\shadow", "/etc/shadow",
  "C:\\Windows", "C:/Windows",
  "boot.ini", "ntldr",
  ".ssh\\id_rsa", ".ssh/id_rsa",
  ".ssh\\authorized_keys",
};

PathValidator::PathValidator(const std::string& workspaceRoot)
    : workspaceRoot_(workspaceRoot),
      normalizedWorkspaceRoot_(NormalizePathInternal(workspaceRoot)) {
  // Default trusted extensions
  trustedExtensions_.insert(".txt");
  trustedExtensions_.insert(".md");
  trustedExtensions_.insert(".cpp");
  trustedExtensions_.insert(".h");
  trustedExtensions_.insert(".hpp");
  trustedExtensions_.insert(".py");
  trustedExtensions_.insert(".js");
  trustedExtensions_.insert(".ts");
  trustedExtensions_.insert(".json");
  trustedExtensions_.insert(".xml");
  trustedExtensions_.insert(".yaml");
  trustedExtensions_.insert(".yml");
  trustedExtensions_.insert(".cfg");
  trustedExtensions_.insert(".ini");
  trustedExtensions_.insert(".log");
  trustedExtensions_.insert(".csv");
  trustedExtensions_.insert(".cmake");
  trustedExtensions_.insert(".css");
  trustedExtensions_.insert(".html");
  trustedExtensions_.insert(".java");
  trustedExtensions_.insert(".rs");
  trustedExtensions_.insert(".go");
}

std::string PathValidator::NormalizePathInternal(const std::string& path) const {
  std::string result = path;
  // Convert forward slashes to backslashes
  std::replace(result.begin(), result.end(), '/', '\\');
  // Remove trailing slashes
  while (!result.empty() && result.back() == '\\') result.pop_back();
  // Resolve to absolute
  if (result.size() >= 2 && std::isalpha(static_cast<unsigned char>(result[0])) && result[1] == ':') {
    // Already absolute: G:\...
    return result;
  }
  // Relative: prepend workspace root
  return normalizedWorkspaceRoot_ + "\\" + result;
}

std::string PathValidator::NormalizePath(const std::string& path) {
  std::string result = path;
  std::replace(result.begin(), result.end(), '/', '\\');
  // Collapse ..
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start < result.size()) {
    std::size_t end = result.find('\\', start);
    if (end == std::string::npos) end = result.size();
    std::string part = result.substr(start, end - start);
    if (part == "..") {
      if (!parts.empty()) parts.pop_back();
    } else if (part != "." && !part.empty()) {
      parts.push_back(part);
    }
    start = end + 1;
  }

  result.clear();
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) result += "\\";
    result += parts[i];
  }
  return result;
}

std::string PathValidator::ResolvePath(const std::string& path) const {
  std::string normalized = NormalizePath(path);
  return NormalizePathInternal(normalized);
}

bool PathValidator::IsInsideWorkspace(const std::string& path) const {
  std::string resolved = ResolvePath(path);
  std::string lower = resolved;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  std::string lowerWs = normalizedWorkspaceRoot_;
  std::transform(lowerWs.begin(), lowerWs.end(), lowerWs.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  // Must start with workspace root
  if (lower.size() < lowerWs.size()) return false;
  if (lower.compare(0, lowerWs.size(), lowerWs) != 0) return false;

  // After workspace root, must have \\ or be exact match
  if (lower.size() == lowerWs.size()) return true;
  return lower[lowerWs.size()] == '\\';
}

bool PathValidator::IsReadAllowed(const std::string& path) const {
  // Inside workspace: always allowed
  if (IsInsideWorkspace(path)) return true;

  // In a trusted directory?
  std::string resolved = ResolvePath(path);
  std::string lower = resolved;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  for (const auto& dir : trustedDirs_) {
    std::string lowerDir = dir;
    std::transform(lowerDir.begin(), lowerDir.end(), lowerDir.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find(lowerDir) == 0) return true;
  }

  return false;
}

bool PathValidator::IsPathSafe(const std::string& path) const {
  // Check dangerous patterns
  if (ContainsDangerousPattern(path)) return false;

  // Must be inside workspace or trusted dir
  return IsInsideWorkspace(path) || IsReadAllowed(path);
}

bool PathValidator::ContainsDangerousPattern(const std::string& path) const {
  std::string lower = path;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  for (const auto& pattern : kDangerousPatterns) {
    std::string lowerPattern = pattern;
    std::transform(lowerPattern.begin(), lowerPattern.end(), lowerPattern.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find(lowerPattern) != std::string::npos) return true;
  }
  return false;
}

void PathValidator::AddTrustedDirectory(const std::string& dir) {
  trustedDirs_.insert(NormalizePath(dir));
}

void PathValidator::AddTrustedExtension(const std::string& ext) {
  trustedExtensions_.insert(ext);
}

}  // namespace permissions
}  // namespace agent