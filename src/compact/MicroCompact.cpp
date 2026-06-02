#include "compact/MicroCompact.h"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace agent {
namespace compact {

// ============================================================================
// Token estimation
// ============================================================================
int EstimateToolResultTokens(const std::string& content, bool isError) {
  (void)isError;
  if (content.empty()) return 0;
  // Tool results also carry their JSON wrapper overhead (~20 tokens)
  return RoughTokenCount(content) + 20;
}

int EstimateToolUseTokens(const std::string& toolName,
                           const std::string& inputJson) {
  return RoughTokenCount(toolName + inputJson) + 10;
}

int EstimateMessageTokens(const std::vector<std::string>& messages) {
  int total = 0;
  for (const auto& msg : messages) {
    if (msg.empty()) continue;

    if (msg.find("\"tool_use\"") != std::string::npos ||
        msg.find("tool_use") != std::string::npos) {
      total += RoughTokenCount(msg) + 10;
    } else if (msg.find("\"tool_result\"") != std::string::npos ||
               msg.find("tool_result") != std::string::npos) {
      total += RoughTokenCount(msg) + 20;
    } else {
      total += RoughTokenCount(msg);
    }
  }
  return total;
}

// ============================================================================
// CollectCompactableToolIds
// ============================================================================
std::vector<std::string> CollectCompactableToolIds(
    const std::vector<std::string>& messages,
    const std::vector<std::string>& compactableToolNames) {
  std::vector<std::string> ids;

  for (const auto& msg : messages) {
    // Look for tool_use blocks
    size_t pos = 0;
    while ((pos = msg.find("\"name\"", pos)) != std::string::npos) {
      // Extract tool name
      size_t nameStart = msg.find("\"", pos + 6);
      if (nameStart == std::string::npos) break;
      size_t nameEnd = msg.find("\"", nameStart + 1);
      if (nameEnd == std::string::npos) break;

      std::string toolName = msg.substr(nameStart + 1, nameEnd - nameStart - 1);

      // Check if compactable
      if (std::find(compactableToolNames.begin(), compactableToolNames.end(),
                    toolName) != compactableToolNames.end()) {
        // Extract tool_use ID
        size_t idPos = msg.rfind("\"id\"", nameStart);
        if (idPos != std::string::npos && idPos > pos - 200) {
          size_t idStart = msg.find("\"", idPos + 4);
          if (idStart != std::string::npos) {
            size_t idEnd = msg.find("\"", idStart + 1);
            if (idEnd != std::string::npos) {
              ids.push_back(msg.substr(idStart + 1, idEnd - idStart - 1));
            }
          }
        }
      }
      pos = nameEnd + 1;
    }
  }

  return ids;
}

bool IsCompactableToolUse(const std::string& message,
                           const std::vector<std::string>& compactableToolNames) {
  for (const auto& name : compactableToolNames) {
    if (message.find(name) != std::string::npos &&
        message.find("tool_use") != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool IsToolResult(const std::string& message) {
  return message.find("tool_result") != std::string::npos;
}

// ============================================================================
// ClearCompactableToolResults
// ============================================================================
int ClearCompactableToolResults(
    std::vector<std::string>& messages,
    const std::vector<std::string>& compactableToolNames,
    int keepRecent) {
  // Collect IDs of compactable tool uses
  auto compactableIds = CollectCompactableToolIds(messages, compactableToolNames);
  std::set<std::string> compactableIdSet(compactableIds.begin(),
                                          compactableIds.end());

  // Count tool results that should be kept (most recent N)
  std::vector<int> toolResultIndices;
  for (int i = 0; i < static_cast<int>(messages.size()); ++i) {
    if (IsToolResult(messages[i])) {
      toolResultIndices.push_back(i);
    }
  }

  int cleared = 0;
  int keepCount = 0;
  for (auto it = toolResultIndices.rbegin();
       it != toolResultIndices.rend(); ++it) {
    if (keepCount < keepRecent) {
      ++keepCount;
      continue;
    }
    // Clear this tool result
    std::string& msg = messages[*it];
    int originalSize = static_cast<int>(msg.size());
    msg = "[Old tool result content cleared]";
    ++cleared;
    (void)originalSize;
  }

  return cleared;
}

// ============================================================================
// BuildMicroCompactBoundary
// ============================================================================
std::string BuildMicroCompactBoundary(int toolsCleared, int tokensSaved) {
  std::ostringstream out;
  out << "[microcompact] " << toolsCleared
      << " old tool results compacted (~" << tokensSaved << " tokens saved)";
  return out.str();
}

// ============================================================================
// MaybeTimeBasedMicroCompact
// ============================================================================
std::optional<MicroCompactResult> MaybeTimeBasedMicroCompact(
    const std::vector<std::string>& messages,
    const MicroCompactConfig& config,
    long long lastAssistantTimestampMs,
    long long nowMs) {
  if (!config.enabled) return std::nullopt;
  if (lastAssistantTimestampMs <= 0) return std::nullopt;

  long long gapMs = nowMs - lastAssistantTimestampMs;
  long long thresholdMs = static_cast<long long>(config.gapThresholdMinutes) * 60 * 1000;

  if (gapMs < thresholdMs) return std::nullopt;

  // Time-based trigger: clear old compactable tool results
  auto workMessages = messages;
  int cleared = ClearCompactableToolResults(workMessages,
                                             config.compactableTools,
                                             config.keepRecent);

  if (cleared == 0) return std::nullopt;

  MicroCompactResult result;
  result.messages = workMessages;
  result.wasCompacted = true;
  result.toolsCleared = cleared;
  result.boundaryText = "[time-based microcompact] " +
      std::to_string(cleared) + " tool results cleared (cache TTL expired, " +
      std::to_string(config.gapThresholdMinutes) + "min gap)";
  return result;
}

// ============================================================================
// MicroCompactMessages
// ============================================================================
MicroCompactResult MicroCompactMessages(
    const std::vector<std::string>& messages,
    const MicroCompactConfig& config,
    long long lastAssistantTimestampMs) {
  MicroCompactResult result;
  result.messages = messages;

  // 1. Try time-based trigger first
  long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  auto timeBased = MaybeTimeBasedMicroCompact(
      messages, config, lastAssistantTimestampMs, nowMs);
  if (timeBased) {
    return *timeBased;
  }

  // 2. Count-based trigger: compact when compactable tool results exceed threshold
  auto compactableIds = CollectCompactableToolIds(messages,
                                                   config.compactableTools);
  int compactableCount = static_cast<int>(compactableIds.size());

  if (compactableCount <= config.triggerThreshold) return result;

  // Clear old results
  auto workMessages = messages;
  int cleared = ClearCompactableToolResults(workMessages,
                                             config.compactableTools,
                                             config.keepRecent);

  if (cleared == 0) return result;

  // Estimate tokens saved
  int tokensSaved = 0;
  for (const auto& msg : messages) {
    if (IsToolResult(msg)) tokensSaved += EstimateToolResultTokens(msg, false);
  }
  tokensSaved = tokensSaved / 2;  // Rough: cleared about half

  result.messages = workMessages;
  result.wasCompacted = true;
  result.toolsCleared = cleared;
  result.tokensSaved = tokensSaved;
  result.boundaryText = BuildMicroCompactBoundary(cleared, tokensSaved);
  return result;
}

}  // namespace compact
}  // namespace agent
