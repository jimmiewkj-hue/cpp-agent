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

class PermissionEngine {
 public:
  void AddAlwaysAllowRule(const std::string& token);
  void AddAlwaysDenyRule(const std::string& token);
  void SetClassifierCallback(ClassifierCallback callback);
  void SetFailClosed(bool failClosed);
  void AddAutoModeAllowlistedTool(const std::string& token);

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

  std::vector<std::string> allowRules_;
  std::vector<std::string> denyRules_;
  std::vector<std::string> autoModeAllowlistedTools_;
  ClassifierCallback classifierCallback_;
  core::DenialTrackingState denialState_;
  bool failClosed_ = true;
};

}  // namespace permissions
}  // namespace agent
