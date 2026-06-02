#include "compact/ContextCollapse.h"

#include <algorithm>
#include <sstream>

namespace agent {
namespace compact {

// ============================================================================
// ContextCollapser
// ============================================================================

ContextCollapser::ContextCollapser(const CollapseConfig& config)
    : config_(config) {}

void ContextCollapser::SetConfig(const CollapseConfig& config) {
  config_ = config;
}

bool ContextCollapser::ShouldCollapse(int estimatedTokens) const {
  return estimatedTokens > config_.maxTokens;
}

CollapseResult ContextCollapser::Collapse(
    const std::vector<std::string>& messages,
    const std::vector<int>& messageTokenCounts) {
  CollapseResult result;

  int totalTokens = 0;
  for (int t : messageTokenCounts) totalTokens += t;

  if (!ShouldCollapse(totalTokens)) {
    result.collapsedMessages = messages;
    return result;
  }

  // Strategy: keep recent messages up to keepRecentMessages, collapse older ones
  int keepCount = std::min(config_.keepRecentMessages,
                           static_cast<int>(messages.size()));
  int collapseFrom = static_cast<int>(messages.size()) - keepCount;
  if (collapseFrom < 0) collapseFrom = 0;

  // Find tool boundaries to preserve
  auto boundaries = FindToolBoundaries(messages);

  // Build collapsed output
  std::vector<std::string> collapsed;
  std::vector<std::string> toSummarize;

  for (int i = 0; i < collapseFrom; ++i) {
    // Check if this message is part of a tool boundary that extends into keep range
    bool inExtendedBoundary = false;
    if (config_.preserveToolBoundaries) {
      for (int b : boundaries) {
        if (b >= collapseFrom && b < static_cast<int>(messages.size())) {
          // This boundary's tool_use is in keep range, check if tool_result is in collapse range
          if (i >= 0 && messages[i].find("tool_result") != std::string::npos) {
            // Check if this is paired with a tool_use in keep range
            inExtendedBoundary = true;
            break;
          }
        }
      }
    }
    if (inExtendedBoundary) {
      collapsed.push_back(messages[i]);
    } else {
      toSummarize.push_back(messages[i]);
      ++result.messagesRemoved;
    }
  }

  // Keep recent messages
  for (int i = collapseFrom; i < static_cast<int>(messages.size()); ++i) {
    collapsed.push_back(messages[i]);
  }

  // Build summary of removed messages
  if (!toSummarize.empty()) {
    result.summary = BuildSummary(toSummarize);
    // Insert summary at collapse boundary
    if (!collapsed.empty()) {
      collapsed.insert(collapsed.begin(),
                       MakeCollapseBoundary(result.messagesRemoved,
                                            totalTokens - (keepCount * 100)));
    }
  }

  result.collapsedMessages = collapsed;
  result.tokensSaved = totalTokens - (keepCount * 100);
  result.wasCollapsed = true;
  return result;
}

std::string ContextCollapser::BuildSummary(
    const std::vector<std::string>& collapsedMessages) const {
  std::ostringstream summary;
  summary << "[Context collapse summary: " << collapsedMessages.size()
          << " messages collapsed]\n";

  // Extract key signals from collapsed messages
  int toolCalls = 0;
  int errors = 0;
  for (const auto& msg : collapsedMessages) {
    if (msg.find("tool_use") != std::string::npos) ++toolCalls;
    if (msg.find("error") != std::string::npos ||
        msg.find("Error") != std::string::npos) ++errors;
  }
  summary << "Tool calls: " << toolCalls << ", errors: " << errors << "\n";

  if (summary.str().size() > static_cast<size_t>(config_.summaryMaxTokens * 4)) {
    return summary.str().substr(0, config_.summaryMaxTokens * 4);
  }
  return summary.str();
}

std::string ContextCollapser::MakeCollapseBoundary(int messagesRemoved,
                                                    int tokensSaved) {
  std::ostringstream out;
  out << "[Context Collapse] " << messagesRemoved
      << " messages collapsed (~" << tokensSaved << " tokens saved)";
  return out.str();
}

std::vector<int> ContextCollapser::FindToolBoundaries(
    const std::vector<std::string>& messages) const {
  std::vector<int> boundaries;
  for (int i = 0; i < static_cast<int>(messages.size()); ++i) {
    if (messages[i].find("tool_use") != std::string::npos ||
        messages[i].find("ToolUse") != std::string::npos) {
      boundaries.push_back(i);
    }
  }
  return boundaries;
}

// ============================================================================
// MessageGrouper
// ============================================================================

std::vector<MessageGroup> MessageGrouper::GroupMessages(
    const std::vector<std::string>& messages,
    const std::vector<int>& tokenCounts) {
  std::vector<MessageGroup> groups;
  if (messages.empty()) return groups;

  MessageGroup current;
  std::string currentType;

  for (int i = 0; i < static_cast<int>(messages.size()); ++i) {
    std::string type;
    if (messages[i].find("tool_use") != std::string::npos ||
        messages[i].find("tool_result") != std::string::npos) {
      type = "tool_chain";
    } else if (messages[i].find("user") != std::string::npos) {
      type = "user_turn";
    } else if (messages[i].find("system") != std::string::npos ||
               messages[i].find("[Collapse]") != std::string::npos) {
      type = "system";
    } else {
      type = "assistant";
    }

    if (currentType.empty()) {
      currentType = type;
      current.groupType = type;
    }

    if (type != currentType && !current.messageIndices.empty()) {
      if (currentType == "tool_chain" && type == "tool_chain") {
        // Keep tool chains together
      } else {
        groups.push_back(current);
        current = MessageGroup{};
        current.groupType = type;
        currentType = type;
      }
    }

    current.messageIndices.push_back(i);
    if (i < static_cast<int>(tokenCounts.size())) {
      current.totalTokens += tokenCounts[i];
    }
  }

  if (!current.messageIndices.empty()) {
    groups.push_back(current);
  }

  return groups;
}

bool MessageGrouper::IsCompleteToolChain(
    const MessageGroup& group,
    const std::vector<std::string>& messages) const {
  if (group.groupType != "tool_chain") return false;
  bool hasToolUse = false;
  bool hasToolResult = false;
  for (int idx : group.messageIndices) {
    if (idx >= static_cast<int>(messages.size())) continue;
    if (messages[idx].find("tool_use") != std::string::npos) hasToolUse = true;
    if (messages[idx].find("tool_result") != std::string::npos) hasToolResult = true;
  }
  return hasToolUse && hasToolResult;
}

std::vector<MessageGroup> MessageGrouper::MergeCompactableGroups(
    const std::vector<MessageGroup>& groups,
    int maxTokensPerGroup) {
  std::vector<MessageGroup> result;
  MessageGroup current;

  for (const auto& g : groups) {
    if (!g.compactable) {
      if (!current.messageIndices.empty()) {
        result.push_back(current);
        current = MessageGroup{};
      }
      result.push_back(g);
      continue;
    }

    if (current.totalTokens + g.totalTokens <= maxTokensPerGroup) {
      // Merge
      for (int idx : g.messageIndices) current.messageIndices.push_back(idx);
      current.totalTokens += g.totalTokens;
      current.groupType = "merged";
    } else {
      if (!current.messageIndices.empty()) result.push_back(current);
      current = g;
    }
  }

  if (!current.messageIndices.empty()) result.push_back(current);
  return result;
}

// ============================================================================
// PostCompactCleanup
// ============================================================================

CleanupResult PostCompactCleanup::Cleanup(std::vector<std::string>& messages) {
  CleanupResult result;
  result.orphanedResultsRemoved = RemoveOrphanedToolResults(messages);
  result.duplicateBoundariesRemoved = RemoveDuplicateBoundaries(messages);
  result.emptyMessagesRemoved = RemoveEmptyMessages(messages);
  return result;
}

int PostCompactCleanup::RemoveOrphanedToolResults(
    std::vector<std::string>& messages) {
  // Collect all tool_use IDs
  std::vector<std::string> toolUseIds;
  for (const auto& msg : messages) {
    if (msg.find("tool_use") == std::string::npos) continue;
    // Simple heuristic: extract tool_use identifiers
    size_t pos = msg.find("\"id\"");
    if (pos != std::string::npos) {
      size_t start = msg.find("\"", pos + 4);
      if (start != std::string::npos) {
        size_t end = msg.find("\"", start + 1);
        if (end != std::string::npos) {
          toolUseIds.push_back(msg.substr(start + 1, end - start - 1));
        }
      }
    }
  }

  int removed = 0;
  auto it = messages.begin();
  while (it != messages.end()) {
    if (it->find("tool_result") != std::string::npos) {
      bool found = false;
      for (const auto& id : toolUseIds) {
        if (it->find(id) != std::string::npos) {
          found = true;
          break;
        }
      }
      if (!found) {
        it = messages.erase(it);
        ++removed;
        continue;
      }
    }
    ++it;
  }
  return removed;
}

int PostCompactCleanup::RemoveDuplicateBoundaries(
    std::vector<std::string>& messages) {
  int removed = 0;
  std::string lastBoundary;
  auto it = messages.begin();
  while (it != messages.end()) {
    if (it->find("[Collapse]") != std::string::npos ||
        it->find("[microcompact]") != std::string::npos ||
        it->find("[snip_boundary]") != std::string::npos) {
      if (*it == lastBoundary) {
        it = messages.erase(it);
        ++removed;
        continue;
      }
      lastBoundary = *it;
    }
    ++it;
  }
  return removed;
}

int PostCompactCleanup::RemoveEmptyMessages(
    std::vector<std::string>& messages) {
  int removed = 0;
  auto it = messages.begin();
  while (it != messages.end()) {
    bool empty = true;
    for (char c : *it) {
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
        empty = false;
        break;
      }
    }
    if (empty) {
      it = messages.erase(it);
      ++removed;
      continue;
    }
    ++it;
  }
  return removed;
}

}  // namespace compact
}  // namespace agent
