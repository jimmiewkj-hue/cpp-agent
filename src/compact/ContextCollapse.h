#pragma once

#include <string>
#include <vector>
#include <functional>
#include <map>

namespace agent {
namespace compact {

// P0-03: Context collapse (aligned with local-ace contextCollapse).
// Provides context window management: summarization, truncation,
// and boundary-preserving collapse strategies.

struct CollapseConfig {
  int maxTokens = 200000;           // Maximum context window
  int keepRecentMessages = 20;      // Messages to keep at the end
  int summaryMaxTokens = 8000;      // Max summary size
  bool preserveUserMessages = true; // Keep original user messages
  bool preserveToolBoundaries = true; // Keep tool_use/tool_result pairs intact
};

struct CollapseResult {
  std::vector<std::string> collapsedMessages;  // Summarized messages
  std::string summary;                          // Generated summary
  int messagesRemoved = 0;
  int tokensSaved = 0;
  bool wasCollapsed = false;
};

class ContextCollapser {
 public:
  explicit ContextCollapser(const CollapseConfig& config = {});

  // Determine if collapse is needed based on estimated token count
  bool ShouldCollapse(int estimatedTokens) const;

  // Collapse a message history into a summarized form.
  // Preserves tool_use/tool_result pairs and recent context.
  CollapseResult Collapse(const std::vector<std::string>& messages,
                           const std::vector<int>& messageTokenCounts);

  // Build a summary of collapsed messages
  std::string BuildSummary(const std::vector<std::string>& collapsedMessages) const;

  // Generate a collapse boundary marker
  static std::string MakeCollapseBoundary(int messagesRemoved, int tokensSaved);

  // Configuration
  void SetConfig(const CollapseConfig& config);
  const CollapseConfig& Config() const { return config_; }

 private:
  // Find tool_use/tool_result pair boundaries
  std::vector<int> FindToolBoundaries(const std::vector<std::string>& messages) const;

  CollapseConfig config_;
};

// ============================================================================
// P0-03: Compact grouping (aligned with local-ace compact/grouping.ts).
// Groups messages into semantic blocks for efficient compaction.
// ============================================================================

struct MessageGroup {
  std::vector<int> messageIndices;  // Indices into the message array
  std::string groupType;            // "user_turn", "tool_chain", "system", "assistant"
  int totalTokens = 0;
  bool compactable = true;
};

class MessageGrouper {
 public:
  // Group messages by their semantic role (user turn, tool chain, etc.)
  std::vector<MessageGroup> GroupMessages(
      const std::vector<std::string>& messages,
      const std::vector<int>& tokenCounts);

  // Check if a group is a complete tool chain (tool_use + tool_results)
  bool IsCompleteToolChain(const MessageGroup& group,
                            const std::vector<std::string>& messages) const;

  // Merge adjacent compactable groups
  std::vector<MessageGroup> MergeCompactableGroups(
      const std::vector<MessageGroup>& groups,
      int maxTokensPerGroup);
};

// ============================================================================
// P0-03: Post-compact cleanup (aligned with local-ace postCompactCleanup.ts).
// Cleans up after compaction: removes orphaned tool results, deduplicates,
// and ensures message consistency.
// ============================================================================

struct CleanupResult {
  int orphanedResultsRemoved = 0;
  int duplicateBoundariesRemoved = 0;
  int emptyMessagesRemoved = 0;
};

class PostCompactCleanup {
 public:
  // Clean up messages after compaction
  CleanupResult Cleanup(std::vector<std::string>& messages);

  // Remove orphaned tool results (results without matching tool_use)
  int RemoveOrphanedToolResults(std::vector<std::string>& messages);

  // Remove duplicate boundary markers
  int RemoveDuplicateBoundaries(std::vector<std::string>& messages);

  // Remove empty/whitespace-only messages
  int RemoveEmptyMessages(std::vector<std::string>& messages);
};

}  // namespace compact
}  // namespace agent
