#pragma once

#include <functional>
#include <string>
#include <vector>

namespace agent {
namespace sandbox {

// P0-03: Sandbox enforcement aligned with local-ace sandbox concepts.
// Provides command filtering, network restriction, and filesystem boundary
// enforcement for the Bash tool and file operations.

enum class SandboxViolationType {
  None,
  CommandBlocked,        // Command matched a deny list
  NetworkAccessBlocked,  // Attempted external network access
  PathEscalation,        // Attempted to access path outside workspace
  FileWriteBlocked,      // Write to a protected location
  ShellInjection,        // Possible shell injection pattern
};

struct SandboxViolation {
  SandboxViolationType type = SandboxViolationType::None;
  std::string detail;
  std::string command;  // The offending command or path
};

enum class SandboxMode {
  ReadOnly,      // All writes blocked, reads allowed within workspace
  WorkspaceWrite,// Writes allowed only within workspace
  FullAccess,    // No restrictions (development/debug)
};

struct SandboxConfig {
  SandboxMode mode = SandboxMode::WorkspaceWrite;
  std::string workspaceRoot;
  bool allowNetworkAccess = false;
  bool allowSubprocesses = true;  // Allow launching child processes
  bool enforceCommandAllowlist = false;
  std::vector<std::string> allowedCommands;    // If enforceCommandAllowlist, only these
  std::vector<std::string> blockedCommands;    // Always blocked regardless
  std::vector<std::string> allowedDomains;     // If allowNetworkAccess, restrict to these
  int maxCommandLength = 100000;
  int maxFileReadSize = 100 * 1024 * 1024;    // 100MB default
};

// P0-03: Command safety patterns (aligned with local-ace shouldUseSandbox)
struct CommandSafetyAssessment {
  bool isSafe = true;
  bool needsSandbox = false;
  std::string reason;
  std::vector<std::string> warnings;
};

class SandboxEnforcer {
 public:
  explicit SandboxEnforcer(const SandboxConfig& config);
  ~SandboxEnforcer() = default;

  // Configure the sandbox after construction
  void Configure(const SandboxConfig& config);

  // Check a Bash command before execution
  SandboxViolation CheckCommand(const std::string& command) const;

  // Check a file path for workspace boundary violations
  SandboxViolation CheckFilePath(const std::string& path,
                                 bool isWrite = false) const;

  // Check network access (URL/domain validation)
  SandboxViolation CheckNetworkAccess(const std::string& url) const;

  // Assess command safety without blocking (for classification)
  CommandSafetyAssessment AssessSafety(const std::string& command) const;

  // Whether sandbox is active (not FullAccess)
  bool IsActive() const;

  // Get current mode
  SandboxMode Mode() const;

  // Get config
  const SandboxConfig& Config() const;

 private:
  // Pattern matching for dangerous commands
  bool MatchesDangerousPattern(const std::string& command) const;
  bool MatchesShellInjectionPattern(const std::string& command) const;
  bool MatchesAllowlist(const std::string& command) const;
  bool MatchesBlocklist(const std::string& command) const;

  // Path traversal detection
  bool ContainsTraversalAttempt(const std::string& path) const;
  bool IsWithinWorkspace(const std::string& path) const;
  bool IsAbsolutePath(const std::string& path) const;

  // Network URL parsing
  bool ExtractDomain(const std::string& url, std::string* domain) const;

  SandboxConfig config_;
};

}  // namespace sandbox
}  // namespace agent
