#pragma once

#include "hooks/HookTypes.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace agent {
namespace hooks {

// ============================================================
// Hook Configuration Manager
// Mirrors local-ace: hooksConfigSnapshot.ts + getMatchingHooks()
// ============================================================

class HookConfig {
 public:
  HookConfig() = default;

  // Load hooks from JSON configuration (settings.json format)
  bool LoadFromJson(const std::string& json);

  // Add a single hook definition
  void AddHook(const HookDefinition& hook);
  void RemoveHooksForEvent(HookEventType event);

  // Get all registered hooks
  const std::vector<HookDefinition>& GetHooks() const;

  // Find hooks matching a given event and optional match query
  // mirrors local-ace getMatchingHooks()
  std::vector<MatchedHook> GetMatchingHooks(
      HookEventType event,
      const HookInput& hookInput,
      const std::string& matchQuery = "") const;

  // Check if any hooks are registered for an event
  bool HasHooksForEvent(HookEventType event) const;

  // Security: check if hooks should be disabled
  bool IsWorkspaceTrusted() const { return workspaceTrusted_; }
  void SetWorkspaceTrusted(bool trusted) { workspaceTrusted_ = trusted; }

  bool IsHooksGloballyDisabled() const { return globallyDisabled_; }
  void SetGloballyDisabled(bool disabled) { globallyDisabled_ = disabled; }

  bool IsSimpleMode() const { return simpleMode_; }
  void SetSimpleMode(bool simple) { simpleMode_ = simple; }

  // Managed hooks (set by policy/admin)
  bool ShouldAllowManagedHooksOnly() const { return managedHooksOnly_; }
  void SetManagedHooksOnly(bool managed) { managedHooksOnly_ = managed; }

  // Clear all hooks
  void Clear();

  // Session-level hook registration (mirrors sessionHooks.ts)
  using SessionHookCallback = std::function<HookResult(
      const HookInput& input,
      const std::string& toolUseID)>;

  void RegisterSessionHook(HookEventType event, SessionHookCallback callback);
  void ClearSessionHooks();
  const std::vector<std::pair<HookEventType, SessionHookCallback>>&
      GetSessionHooks() const;

 private:
  bool HookMatchesQuery(const HookDefinition& hook,
                         const HookInput& input,
                         const std::string& matchQuery) const;

  std::vector<HookDefinition> hooks_;
  std::vector<std::pair<HookEventType, SessionHookCallback>> sessionHooks_;
  bool workspaceTrusted_ = false;
  bool globallyDisabled_ = false;
  bool simpleMode_ = false;
  bool managedHooksOnly_ = false;
  mutable std::mutex mutex_;
};

}  // namespace hooks
}  // namespace agent