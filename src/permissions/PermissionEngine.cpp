#include "permissions/PermissionEngine.h"

#include "third_party/nlohmann_json.hpp"

#include <algorithm>

namespace agent {
namespace permissions {

using json = nlohmann::json;

namespace {

std::string ToLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

std::string Trim(std::string value) {
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(),
                           [](unsigned char ch) { return !std::isspace(ch); }));
  value.erase(std::find_if(value.rbegin(), value.rend(),
                           [](unsigned char ch) { return !std::isspace(ch); })
                  .base(),
              value.end());
  return value;
}

std::vector<std::string> ExtractRuleCandidates(
    const core::ContentBlock& toolUse) {
  std::vector<std::string> candidates;
  if (toolUse.type != core::BlockType::ToolUse) return candidates;

  const std::string toolName = ToLowerAscii(toolUse.asToolUse.name);
  if (!toolName.empty()) candidates.push_back(toolName);

  if (toolName != "bash") return candidates;

  std::string command;
  try {
    const json payload = json::parse(toolUse.asToolUse.inputJson);
    if (payload.contains("command") && payload["command"].is_string()) {
      command = payload["command"].get<std::string>();
    } else if (payload.contains("cmd") && payload["cmd"].is_string()) {
      command = payload["cmd"].get<std::string>();
    }
  } catch (...) {
  }

  command = ToLowerAscii(Trim(command));
  if (command.empty()) return candidates;
  candidates.push_back(command);

  std::istringstream stream(command);
  std::string first;
  std::string second;
  stream >> first;
  stream >> second;
  if (!first.empty()) candidates.push_back(first);
  if (!first.empty() && !second.empty()) {
    candidates.push_back(first + " " + second);
  }
  return candidates;
}

bool MatchesRuleAgainstCandidate(const std::string& candidate,
                                 const std::string& rawRule) {
  const std::string rule = ToLowerAscii(Trim(rawRule));
  if (rule.empty() || candidate.empty()) return false;
  if (candidate == rule) return true;
  if (candidate.size() > rule.size() &&
      candidate.compare(0, rule.size(), rule) == 0 &&
      std::isspace(static_cast<unsigned char>(candidate[rule.size()]))) {
    return true;
  }
  return false;
}

}  // namespace

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
  if (MatchesAny(toolUse.asToolUse.name, denyRules_) ||
      MatchesAny(toolUse, denyRules_)) {
    denialState_.RecordDenial();
    return {core::PermissionBehavior::Deny, "matched deny rule"};
  }

  // Step 2: always allow rules
  if (MatchesAny(toolUse.asToolUse.name, allowRules_) ||
      MatchesAny(toolUse, allowRules_)) {
    denialState_.RecordApproval();
    return {core::PermissionBehavior::Allow, "matched allow rule"};
  }

  // Step 3: safe allowlist — skip classifier
  if (MatchesAny(toolUse.asToolUse.name, autoModeAllowlistedTools_) ||
      MatchesAny(toolUse, autoModeAllowlistedTools_)) {
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
  const std::string normalizedCandidate = ToLowerAscii(candidate);
  for (const auto& rule : rules) {
    if (MatchesRuleAgainstCandidate(normalizedCandidate, rule)) return true;
  }
  return false;
}

bool PermissionEngine::MatchesAny(
    const core::ContentBlock& toolUse,
    const std::vector<std::string>& rules) const {
  const std::vector<std::string> candidates = ExtractRuleCandidates(toolUse);
  for (const auto& candidate : candidates) {
    for (const auto& rule : rules) {
      if (MatchesRuleAgainstCandidate(candidate, rule)) return true;
    }
  }
  return false;
}

}  // namespace permissions
}  // namespace agent
