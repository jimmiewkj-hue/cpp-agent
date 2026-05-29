#pragma once

#include "core/AgentTypes.h"
#include "core/StateTypes.h"

#include <functional>
#include <string>
#include <vector>

namespace agent {
namespace permissions {

using ClassifierCallback = std::function<core::PermissionDecision(
    const core::ContentBlock& toolUse,
    const std::vector<core::Message>& messages)>;
using ManualApprovalCallback = std::function<core::PermissionDecision(
    const core::ContentBlock& toolUse,
    const std::vector<core::Message>& messages,
    const core::PermissionDecision& pendingDecision)>;

class PermissionEngine {
 public:
  void AddAlwaysAllowRule(const std::string& token);
  void AddAlwaysDenyRule(const std::string& token);
  void SetClassifierCallback(ClassifierCallback callback);
  void SetManualApprovalCallback(ManualApprovalCallback callback);
  void SetFailClosed(bool failClosed);
  void AddAutoModeAllowlistedTool(const std::string& token);
  void SetPermissionMode(core::PermissionMode mode);
  core::PermissionMode GetPermissionMode() const;

  core::CanUseTool BuildCanUseTool();

  core::PermissionDecision Evaluate(
      const core::ContentBlock& toolUse,
      const std::vector<core::Message>& messages);

  const core::DenialTrackingState& denialState() const;
  bool IsCircuitBroken() const;
  void ResetDenialState();

 private:
  bool MatchesAny(const std::string& candidate,
                  const std::vector<std::string>& rules) const;
  bool MatchesAny(const core::ContentBlock& toolUse,
                  const std::vector<std::string>& rules) const;

  std::vector<std::string> allowRules_;
  std::vector<std::string> denyRules_;
  std::vector<std::string> autoModeAllowlistedTools_;
  ClassifierCallback classifierCallback_;
  ManualApprovalCallback manualApprovalCallback_;
  core::DenialTrackingState denialState_;
  core::PermissionMode permissionMode_ = core::PermissionMode::Default;
  bool failClosed_ = true;
};

}  // namespace permissions
}  // namespace agent
