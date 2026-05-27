#include "hooks/HookConfig.h"

#include "third_party/nlohmann_json.hpp"

#include <algorithm>

using json = nlohmann::json;

namespace agent {
namespace hooks {

bool HookConfig::LoadFromJson(const std::string& jsonStr) {
  try {
    auto root = json::parse(jsonStr);
    if (!root.contains("hooks") || !root["hooks"].is_array()) {
      return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : root["hooks"]) {
      HookDefinition hook;

      // Parse hook event
      if (entry.contains("event")) {
        hook.matcher.event = ParseHookEventType(entry["event"].get<std::string>());
      } else {
        continue;  // event is required
      }

      // Parse hook type
      if (entry.contains("type")) {
        std::string typeStr = entry["type"].get<std::string>();
        if (typeStr == "command") hook.type = HookType::Command;
        else if (typeStr == "prompt") hook.type = HookType::Prompt;
        else if (typeStr == "agent") hook.type = HookType::Agent;
        else if (typeStr == "http") hook.type = HookType::HTTP;
        else if (typeStr == "callback") hook.type = HookType::Callback;
      }

      // Parse command/prompt/url
      if (entry.contains("command")) hook.command = entry["command"].get<std::string>();
      if (entry.contains("prompt")) hook.prompt = entry["prompt"].get<std::string>();
      if (entry.contains("url")) hook.url = entry["url"].get<std::string>();
      if (entry.contains("agentType")) hook.agentType = entry["agentType"].get<std::string>();

      // Parse optional fields
      if (entry.contains("matchQuery")) {
        hook.matcher.matchQuery = entry["matchQuery"].get<std::string>();
      }
      if (entry.contains("matcher") && entry["matcher"].is_array()) {
        for (const auto& m : entry["matcher"]) {
          hook.matcherPatterns.push_back(m.get<std::string>());
        }
      }
      if (entry.contains("timeout")) {
        hook.timeoutSec = entry["timeout"].get<int>();
      }
      if (entry.contains("statusMessage")) {
        hook.statusMessage = entry["statusMessage"].get<std::string>();
      }

      hooks_.push_back(hook);
    }
    return true;
  } catch (...) {
    return false;
  }
}

void HookConfig::AddHook(const HookDefinition& hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  hooks_.push_back(hook);
}

void HookConfig::RemoveHooksForEvent(HookEventType event) {
  std::lock_guard<std::mutex> lock(mutex_);
  hooks_.erase(
      std::remove_if(hooks_.begin(), hooks_.end(),
                     [event](const HookDefinition& h) {
                       return h.matcher.event == event;
                     }),
      hooks_.end());
}

const std::vector<HookDefinition>& HookConfig::GetHooks() const {
  return hooks_;
}

bool HookConfig::HookMatchesQuery(const HookDefinition& hook,
                                   const HookInput& input,
                                   const std::string& matchQuery) const {
  if (hook.matcher.event != input.eventType) {
    return false;
  }

  // If no match query, match all for this event
  if (hook.matcher.matchQuery.empty() && matchQuery.empty()) {
    return true;
  }

  // Check exact match
  std::string query = matchQuery.empty() ? hook.matcher.matchQuery : matchQuery;
  if (query.empty()) return true;

  // Check against tool name in input
  std::string toolName;
  switch (input.eventType) {
    case HookEventType::PreToolUse:
      toolName = input.preToolUse.tool_name;
      break;
    case HookEventType::PostToolUse:
      toolName = input.postToolUse.tool_name;
      break;
    case HookEventType::PostToolUseFailure:
      toolName = input.postToolUseFailure.tool_name;
      break;
    default:
      break;
  }

  if (!toolName.empty() && toolName == query) return true;

  // Check matcher patterns
  for (const auto& pattern : hook.matcherPatterns) {
    if (pattern == "*" || pattern == query) return true;
    if (pattern.size() > 2 && pattern.back() == '*' &&
        query.find(pattern.substr(0, pattern.size() - 1)) == 0) {
      return true;
    }
  }

  return false;
}

std::vector<MatchedHook> HookConfig::GetMatchingHooks(
    HookEventType event,
    const HookInput& hookInput,
    const std::string& matchQuery) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<MatchedHook> result;

  for (const auto& hook : hooks_) {
    if (HookMatchesQuery(hook, hookInput, matchQuery)) {
      MatchedHook mh;
      mh.hook = hook;
      result.push_back(mh);
    }
  }
  return result;
}

bool HookConfig::HasHooksForEvent(HookEventType event) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& hook : hooks_) {
    if (hook.matcher.event == event) return true;
  }
  return false;
}

void HookConfig::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  hooks_.clear();
}

void HookConfig::RegisterSessionHook(HookEventType event,
                                      SessionHookCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  sessionHooks_.emplace_back(event, std::move(callback));
}

void HookConfig::ClearSessionHooks() {
  std::lock_guard<std::mutex> lock(mutex_);
  sessionHooks_.clear();
}

const std::vector<std::pair<HookEventType, HookConfig::SessionHookCallback>>&
HookConfig::GetSessionHooks() const {
  return sessionHooks_;
}

}  // namespace hooks
}  // namespace agent