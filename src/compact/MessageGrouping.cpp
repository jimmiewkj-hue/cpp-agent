// MessageGrouping.cpp — aligned with local-ace services/compact/grouping.ts
#include "compact/MessageGrouping.h"

namespace agent {
namespace compact {

std::vector<std::vector<core::Message>> GroupMessagesByApiRound(
    const std::vector<core::Message>& messages) {
  std::vector<std::vector<core::Message>> groups;
  std::vector<core::Message> current;
  std::string lastAssistantId;  // message.uuid of most recent assistant

  // In well-formed conversations the API contract guarantees every
  // tool_use is resolved before the next assistant turn, so lastAssistantId
  // alone is a sufficient boundary gate.
  for (const auto& msg : messages) {
    if (msg.role == core::MessageRole::Assistant &&
        !msg.uuid.empty() &&
        msg.uuid != lastAssistantId &&
        !current.empty()) {
      groups.push_back(std::move(current));
      current.clear();
      current.push_back(msg);
    } else {
      current.push_back(msg);
    }
    if (msg.role == core::MessageRole::Assistant) {
      lastAssistantId = msg.uuid;
    }
  }

  if (!current.empty()) {
    groups.push_back(std::move(current));
  }

  return groups;
}

}  // namespace compact
}  // namespace agent
