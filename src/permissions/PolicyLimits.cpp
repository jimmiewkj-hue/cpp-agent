#include "permissions/PolicyLimits.h"

#include <algorithm>
#include <sstream>

namespace agent {
namespace permissions {

PolicyLimits::PolicyLimits(const PolicyLimitsConfig& config)
    : config_(config) {}

void PolicyLimits::SetConfig(const PolicyLimitsConfig& config) {
  config_ = config;
}

bool PolicyLimits::IsRestricted(const std::string& featureId) const {
  if (!config_.enabled) return false;

  for (const auto& r : config_.restrictions) {
    if (r.featureId == featureId && r.disabled) return true;
  }
  return false;
}

const PolicyRestriction* PolicyLimits::GetRestriction(
    const std::string& featureId) const {
  for (const auto& r : config_.restrictions) {
    if (r.featureId == featureId) return &r;
  }
  return nullptr;
}

bool PolicyLimits::IsBashExecutionAllowed() const {
  return !IsRestricted("bash_execution");
}

bool PolicyLimits::IsNetworkAccessAllowed() const {
  return !IsRestricted("network_access");
}

bool PolicyLimits::IsFileWriteAllowed() const {
  return !IsRestricted("file_write");
}

bool PolicyLimits::IsSubprocessAllowed() const {
  return !IsRestricted("subprocess_spawn");
}

void PolicyLimits::AddRestriction(const PolicyRestriction& restriction) {
  // Remove existing restriction with same featureId
  RemoveRestriction(restriction.featureId);
  config_.restrictions.push_back(restriction);
}

void PolicyLimits::RemoveRestriction(const std::string& featureId) {
  config_.restrictions.erase(
      std::remove_if(config_.restrictions.begin(), config_.restrictions.end(),
                     [&](const PolicyRestriction& r) {
                       return r.featureId == featureId;
                     }),
      config_.restrictions.end());
}

void PolicyLimits::LoadDefaultRestrictions() {
  // Common safe defaults ? can be overridden by org policy
  config_.restrictions.clear();

  // Block obviously dangerous operations by default
  config_.restrictions.push_back({
    "shell_injection", true,
    "Shell injection patterns are blocked by default",
    "default"
  });

  config_.restrictions.push_back({
    "path_traversal", true,
    "Path traversal outside workspace is blocked",
    "default"
  });

  config_.restrictions.push_back({
    "destructive_system_commands", true,
    "Commands like rm -rf /, format, del /f/s are blocked",
    "default"
  });
}

std::string PolicyLimits::FormatRestrictionsSummary() const {
  std::ostringstream out;

  if (!config_.enabled) {
    out << "[Policy Limits] DISABLED ? all features allowed\n";
    return out.str();
  }

  int disabledCount = 0;
  for (const auto& r : config_.restrictions) {
    if (r.disabled) ++disabledCount;
  }

  out << "[Policy Limits] " << disabledCount << " feature(s) restricted";
  if (config_.failOpen) out << " (fail-open mode)";
  out << "\n";

  for (const auto& r : config_.restrictions) {
    if (!r.disabled) continue;
    out << "  - " << r.featureId << ": " << r.reason;
    if (!r.source.empty()) out << " [" << r.source << "]";
    out << "\n";
  }

  return out.str();
}

}  // namespace permissions
}  // namespace agent
