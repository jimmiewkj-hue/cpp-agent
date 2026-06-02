#include "sandbox/SandboxEnforcer.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace agent {
namespace sandbox {

namespace {

static const char* kDangerousPatterns[] = {
  "rm -rf /",
  "format c:",
  "shutdown",
  "Stop-Computer",
  nullptr
};

static const char* kNetworkPrefixes[] = {
  "curl ",
  "wget ",
  "Invoke-WebRequest",
  "Invoke-RestMethod",
  "ssh ",
  "scp ",
  "ftp ",
  nullptr
};

std::string ToLower(const std::string& s) {
  std::string r = s;
  std::transform(r.begin(), r.end(), r.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return r;
}

bool StartsWithIgnoreCase(const std::string& s, const std::string& prefix) {
  if (s.size() < prefix.size()) return false;
  return ToLower(s.substr(0, prefix.size())) == ToLower(prefix);
}

}  // namespace

SandboxEnforcer::SandboxEnforcer(const SandboxConfig& config)
    : config_(config) {}

void SandboxEnforcer::Configure(const SandboxConfig& config) {
  config_ = config;
}

bool SandboxEnforcer::IsActive() const {
  return config_.mode != SandboxMode::FullAccess;
}

SandboxMode SandboxEnforcer::Mode() const {
  return config_.mode;
}

const SandboxConfig& SandboxEnforcer::Config() const {
  return config_;
}

SandboxViolation SandboxEnforcer::CheckCommand(const std::string& command) const {
  if (!IsActive()) return {};

  SandboxViolation result;
  const std::string lower = ToLower(command);

  // Check length
  if (static_cast<int>(command.size()) > config_.maxCommandLength) {
    result.type = SandboxViolationType::CommandBlocked;
    result.detail = "Command exceeds maximum length of " +
        std::to_string(config_.maxCommandLength) + " chars";
    result.command = command.substr(0, 200);
    return result;
  }

  // Check blocklist
  if (MatchesBlocklist(command)) {
    result.type = SandboxViolationType::CommandBlocked;
    result.detail = "Command matches sandbox blocklist";
    result.command = command.substr(0, 200);
    return result;
  }

  // Check allowlist
  if (config_.enforceCommandAllowlist && !MatchesAllowlist(command)) {
    result.type = SandboxViolationType::CommandBlocked;
    result.detail = "Command not in sandbox allowlist";
    result.command = command.substr(0, 200);
    return result;
  }

  // Check dangerous patterns
  if (MatchesDangerousPattern(command)) {
    result.type = SandboxViolationType::CommandBlocked;
    result.detail = "Command matches dangerous pattern";
    result.command = command.substr(0, 200);
    return result;
  }

  // Check shell injection
  if (MatchesShellInjectionPattern(command)) {
    result.type = SandboxViolationType::ShellInjection;
    result.detail = "Possible shell injection pattern detected";
    result.command = command.substr(0, 200);
    return result;
  }

  // Check network access
  if (!config_.allowNetworkAccess) {
    for (int i = 0; kNetworkPrefixes[i] != nullptr; ++i) {
      if (StartsWithIgnoreCase(lower, kNetworkPrefixes[i])) {
        result.type = SandboxViolationType::NetworkAccessBlocked;
        result.detail = "Network access is disabled in sandbox mode";
        result.command = command.substr(0, 200);
        return result;
      }
    }
  }

  return result;
}

SandboxViolation SandboxEnforcer::CheckFilePath(const std::string& path,
                                                bool isWrite) const {
  if (!IsActive()) return {};
  if (config_.mode == SandboxMode::ReadOnly && !isWrite) return {};

  SandboxViolation result;

  // Check traversal
  if (ContainsTraversalAttempt(path)) {
    result.type = SandboxViolationType::PathEscalation;
    result.detail = "Path contains traversal pattern";
    result.command = path;
    return result;
  }

  // Check workspace boundary
  if (!config_.workspaceRoot.empty() && IsAbsolutePath(path)) {
    if (!IsWithinWorkspace(path)) {
      result.type = SandboxViolationType::PathEscalation;
      result.detail = "Path is outside workspace: " + config_.workspaceRoot;
      result.command = path;
      return result;
    }
  }

  // ReadOnly mode blocks writes
  if (config_.mode == SandboxMode::ReadOnly && isWrite) {
    result.type = SandboxViolationType::FileWriteBlocked;
    result.detail = "File writes are blocked in ReadOnly sandbox mode";
    result.command = path;
    return result;
  }

  return result;
}

SandboxViolation SandboxEnforcer::CheckNetworkAccess(const std::string& url) const {
  if (!IsActive()) return {};
  if (config_.allowNetworkAccess) return {};

  SandboxViolation result;
  result.type = SandboxViolationType::NetworkAccessBlocked;
  result.detail = "Network access is disabled in sandbox mode";
  result.command = url;
  return result;
}

CommandSafetyAssessment SandboxEnforcer::AssessSafety(
    const std::string& command) const {
  CommandSafetyAssessment a;
  if (MatchesDangerousPattern(command)) {
    a.isSafe = false;
    a.needsSandbox = true;
    a.warnings.push_back("Matches dangerous command pattern");
  }
  if (MatchesShellInjectionPattern(command)) {
    a.isSafe = false;
    a.needsSandbox = true;
    a.warnings.push_back("Contains shell injection pattern");
  }
  return a;
}

bool SandboxEnforcer::MatchesDangerousPattern(const std::string& command) const {
  std::string lower = ToLower(command);
  for (int i = 0; kDangerousPatterns[i] != nullptr; ++i) {
    if (lower.find(ToLower(kDangerousPatterns[i])) != std::string::npos)
      return true;
  }
  return false;
}

bool SandboxEnforcer::MatchesShellInjectionPattern(const std::string& command) const {
  std::string lower = ToLower(command);
  const char* patterns[] = {"Invoke-Expression", "iex ", "Start-Process -Verb RunAs", nullptr};
  for (int i = 0; patterns[i] != nullptr; ++i) {
    if (lower.find(ToLower(patterns[i])) != std::string::npos)
      return true;
  }
  return false;
}

bool SandboxEnforcer::MatchesAllowlist(const std::string& command) const {
  if (config_.allowedCommands.empty()) return true;
  std::string lower = ToLower(command);
  for (const auto& allowed : config_.allowedCommands) {
    if (lower.find(ToLower(allowed)) != std::string::npos)
      return true;
  }
  return false;
}

bool SandboxEnforcer::MatchesBlocklist(const std::string& command) const {
  std::string lower = ToLower(command);
  const char* blocked[] = {"shutdown", "Stop-Computer", "Restart-Computer",
                           "format", "diskpart", "bcdedit", nullptr};
  for (int i = 0; blocked[i] != nullptr; ++i) {
    if (lower.find(ToLower(blocked[i])) != std::string::npos)
      return true;
  }
  for (const auto& b : config_.blockedCommands) {
    if (lower.find(ToLower(b)) != std::string::npos)
      return true;
  }
  return false;
}

bool SandboxEnforcer::ContainsTraversalAttempt(const std::string& path) const {
  const char* patterns[] = {"../", "..\\", "%USERPROFILE%",
                            "%APPDATA%", "%WINDIR%", nullptr};
  for (int i = 0; patterns[i] != nullptr; ++i) {
    if (path.find(patterns[i]) != std::string::npos)
      return true;
  }
  return false;
}

bool SandboxEnforcer::IsWithinWorkspace(const std::string& path) const {
  if (config_.workspaceRoot.empty()) return true;
  std::string np = path, nr = config_.workspaceRoot;
  std::replace(np.begin(), np.end(), '/', '\\');
  std::replace(nr.begin(), nr.end(), '/', '\\');
  if (!nr.empty() && nr.back() != '\\') nr += '\\';
  return ToLower(np).find(ToLower(nr)) == 0;
}

bool SandboxEnforcer::IsAbsolutePath(const std::string& path) const {
  if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) &&
      path[1] == ':') return true;
  if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') return true;
  return false;
}

bool SandboxEnforcer::ExtractDomain(const std::string& url,
                                    std::string* domain) const {
  if (!domain) return false;
  std::string u = url;
  size_t proto = u.find("://");
  if (proto != std::string::npos) u = u.substr(proto + 3);
  size_t slash = u.find('/');
  size_t colon = u.find(':');
  *domain = u.substr(0, std::min(slash, colon));
  return !domain->empty();
}

}  // namespace sandbox
}  // namespace agent
