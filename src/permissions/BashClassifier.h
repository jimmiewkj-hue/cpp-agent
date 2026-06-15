#pragma once

#include "core/AgentTypes.h"
#include "core/StateTypes.h"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace agent {
namespace permissions {

struct BashSafetyDecision {
  bool allow = false;
  std::string reason;
};

// STRENGTHEN-T12: callback signature for LLM-based classification of
// commands that miss the static pattern lists. Returns a decision; if the
// callback is null or fails, the caller falls back to deny-by-default.
// Aligned with PermissionEngine::ClassifierCallback.
using BashClassifierCallback = std::function<BashSafetyDecision(
    const std::string& command,
    const std::vector<core::Message>& context)>;

class BashClassifier {
 public:
  BashClassifier();

  void SetApiKey(const std::string& key);
  // STRENGTHEN-T12: install the optional LLM classifier. When set, commands
  // that miss the read-only allowlist AND the destructive denylist are
  // forwarded to this callback before falling back to deny-by-default.
  // Results are cached per-command for the process lifetime.
  void SetClassifierCallback(BashClassifierCallback callback);

  BashSafetyDecision Classify(const std::string& command,
                              const std::vector<core::Message>& context);

  bool IsReadOnlyCommand(const std::string& command) const;
  std::string BuildClassifierPrompt(const std::string& command,
                                    const std::vector<core::Message>& context);

 private:
  bool MatchesReadOnlyPattern(const std::string& command) const;

  static const std::vector<std::string> kReadOnlyCommands;
  static const std::vector<std::string> kDestructiveCommands;
  static const std::vector<std::string> kReadOnlyPrefixes;

  std::string apiKey_;
  BashClassifierCallback classifierCallback_;
  // STRENGTHEN-T12: per-process cache so the same command isn't re-queried.
  // Keyed by command string; guarded by mutex for thread safety.
  std::map<std::string, BashSafetyDecision> cache_;
  mutable std::mutex cacheMutex_;
};

}  // namespace permissions
}  // namespace agent
