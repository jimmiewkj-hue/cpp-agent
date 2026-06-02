#pragma once

#include <string>
#include <vector>
#include <set>

namespace agent {
namespace permissions {

// ============================================================================
// PathValidator — workspace path security (aligned with local-ace pathValidation.ts)
// ============================================================================
class PathValidator {
 public:
  explicit PathValidator(const std::string& workspaceRoot);

  // Check if a path is safe to access (read or write)
  bool IsPathSafe(const std::string& path) const;

  // Check if a path is inside the workspace (for write operations)
  bool IsInsideWorkspace(const std::string& path) const;

  // Check if a path is allowed for read operations
  bool IsReadAllowed(const std::string& path) const;

  // Resolve a potentially relative path to absolute
  std::string ResolvePath(const std::string& path) const;

  // Normalize path (resolve .., ., double slashes)
  static std::string NormalizePath(const std::string& path);

  // Add trusted directories (outside workspace but safe to read)
  void AddTrustedDirectory(const std::string& dir);
  void AddTrustedExtension(const std::string& ext);

  // Dangerous pattern detection
  bool ContainsDangerousPattern(const std::string& path) const;

 private:
  std::string NormalizePathInternal(const std::string& path) const;
  std::string workspaceRoot_;
  std::string normalizedWorkspaceRoot_;
  std::set<std::string> trustedDirs_;
  std::set<std::string> trustedExtensions_;

  // Dangerous patterns (symlink attacks, traversal, etc.)
  static const std::vector<std::string> kDangerousPatterns;
};

}  // namespace permissions
}  // namespace agent