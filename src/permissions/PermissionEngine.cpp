#include "permissions/PermissionEngine.h"

#include <algorithm>

namespace agent {
namespace permissions {

void PermissionEngine::AddAlwaysAllowRule(const std::string& token) {
  allowRules_.push_back(token);
}

void PermissionEngine::AddAlwaysDenyRule(const std::string& token) {
  denyRules_.push_back(token);
}

void PermissionEngine::SetClassifierCallback(ClassifierCallback callback) {
  classifierCallback_ = std::move(callback);
}

void PermissionEngine::SetFailClosed(bool failClosed) {
  failClosed_ = failClosed;
}

void PermissionEngine::AddAutoModeAllowlistedTool(const std::string& token) {
  if (std::find(autoModeAllowlistedTools_.begin(),
                autoModeAllowlistedTools_.end(),
                token) == autoModeAllowlistedTools_.end()) {
    autoModeAllowlistedTools_.push_back(token);
  }
}

void PermissionEngine::SetPermissionMode(core::PermissionMode mode) {
  permissionMode_ = mode;
}

core::PermissionMode PermissionEngine::GetPermissionMode() const {
  return permissionMode_;
}

core::CanUseTool PermissionEngine::BuildCanUseTool() {
  return [this](const core::ContentBlock& toolUse,
                const std::vector<core::Message>& messages) {
    return Evaluate(toolUse, messages);
  };
}

core::PermissionDecision PermissionEngine::Evaluate(
    const core::ContentBlock& toolUse,
    const std::vector<core::Message>& messages) {
  // Step 0: mode-based shortcuts
  if (permissionMode_ == core::PermissionMode::BypassPermissions) {
    return {core::PermissionBehavior::Allow, "bypass permissions mode"};
  }
  if (permissionMode_ == core::PermissionMode::Plan) {
    return {core::PermissionBehavior::Allow, "plan mode — tools not actually executed"};
  }

  // Step 1: always deny rules
  if (MatchesAny(toolUse.asToolUse.name, denyRules_)) {
    denialState_.RecordDenial();
    return {core::PermissionBehavior::Deny, "matched deny rule"};
  }

  // Step 2: always allow rules
  if (MatchesAny(toolUse.asToolUse.name, allowRules_)) {
    denialState_.RecordApproval();
    return {core::PermissionBehavior::Allow, "matched allow rule"};
  }

  // Step 3: safe allowlist — skip classifier
  if (MatchesAny(toolUse.asToolUse.name, autoModeAllowlistedTools_)) {
    denialState_.RecordApproval();
    return {core::PermissionBehavior::Allow, "in auto-mode safe allowlist"};
  }

  // Step 4: circuit breaker
  if (denialState_.IsCircuitBroken()) {
    return {core::PermissionBehavior::Ask,
            "circuit broken — maxConsecutive=" +
                std::to_string(denialState_.maxConsecutive) +
                " maxTotal=" +
                std::to_string(denialState_.maxTotal) +
                " — manual confirmation required"};
  }

  // Step 5: classifier callback (auto-mode YOLO classifier)
  if (classifierCallback_) {
    try {
      core::PermissionDecision decision =
          classifierCallback_(toolUse, messages);
      if (decision.behavior == core::PermissionBehavior::Deny) {
        denialState_.RecordDenial();
        return decision;
      }
      denialState_.RecordApproval();
      return decision;
    } catch (...) {
      // classifier unavailable → fail-open or fail-closed
      if (failClosed_) {
        denialState_.RecordDenial();
        return {core::PermissionBehavior::Deny,
                "classifier unavailable, fail-closed gate active"};
      }
      return {core::PermissionBehavior::Ask,
              "classifier unavailable, manual confirmation required"};
    }
  }

  return {core::PermissionBehavior::Ask,
          "no rule matched; requires confirmation"};
}

const core::DenialTrackingState& PermissionEngine::denialState() const {
  return denialState_;
}

bool PermissionEngine::IsCircuitBroken() const {
  return denialState_.IsCircuitBroken();
}

void PermissionEngine::ResetDenialState() {
  denialState_.Reset();
}

bool PermissionEngine::MatchesAny(
    const std::string& candidate,
    const std::vector<std::string>& rules) const {
  for (const auto& rule : rules) {
    if (rule == candidate) return true;
  }
  return false;
}

}  // namespace permissions
}  // namespace agent
