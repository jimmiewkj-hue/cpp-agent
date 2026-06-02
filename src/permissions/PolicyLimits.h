#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>

namespace agent {
namespace permissions {

// P0-03: Policy limits (aligned with local-ace policyLimits).
// Fetches and enforces organization-level policy restrictions.
// Controls which CLI features are disabled based on org policy.

struct PolicyRestriction {
  std::string featureId;     // e.g., "bash_execution", "network_access"
  bool disabled = false;      // If true, feature is blocked
  std::string reason;         // Human-readable reason
  std::string source;         // "organization", "team", "user"
};

struct PolicyLimitsConfig {
  bool enabled = true;
  bool failOpen = true;       // If policy fetch fails, allow everything
  int refreshIntervalMs = 300000;  // 5 minutes
  std::vector<PolicyRestriction> restrictions;
};

class PolicyLimits {
 public:
  explicit PolicyLimits(const PolicyLimitsConfig& config = {});

  // Check if a specific feature is restricted
  bool IsRestricted(const std::string& featureId) const;

  // Get the restriction for a feature (nullptr if not restricted)
  const PolicyRestriction* GetRestriction(const std::string& featureId) const;

  // Check if Bash execution is allowed
  bool IsBashExecutionAllowed() const;

  // Check if network access is allowed
  bool IsNetworkAccessAllowed() const;

  // Check if file system writes are allowed
  bool IsFileWriteAllowed() const;

  // Check if subprocess spawning is allowed
  bool IsSubprocessAllowed() const;

  // Add/remove restrictions at runtime
  void AddRestriction(const PolicyRestriction& restriction);
  void RemoveRestriction(const std::string& featureId);

  // Configure
  void SetConfig(const PolicyLimitsConfig& config);
  const PolicyLimitsConfig& Config() const { return config_; }

  // Build a human-readable summary of active restrictions
  std::string FormatRestrictionsSummary() const;

  // Load default restrictions for common scenarios
  void LoadDefaultRestrictions();

 private:
  PolicyLimitsConfig config_;
};

}  // namespace permissions
}  // namespace agent
