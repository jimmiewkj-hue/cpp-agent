#pragma once

#include "core/AgentTypes.h"
#include "core/StateTypes.h"

#include <functional>
#include <string>
#include <vector>

namespace agent {
namespace permissions {

struct BashSafetyDecision {
  bool allow = false;
  std::string reason;
};

class BashClassifier {
 public:
  BashClassifier();

  void SetApiKey(const std::string& key);

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
};

}  // namespace permissions
}  // namespace agent
