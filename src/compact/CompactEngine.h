#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <optional>

namespace agent {
namespace compact {

// P0-03: Compact engine (aligned with local-ace compact.ts, 60KB).
// Core compaction data structures, message ordering, boundary annotation,
// hook instruction merging, and post-compact message assembly.

// ============================================================================
// Compaction result (aligned with local-ace CompactionResult)
// ============================================================================
struct CompactionResult {
  std::string boundaryMarker;            // System message marking compaction
  std::vector<std::string> summaryMessages;  // User messages containing summaries
  std::vector<std::string> attachments;      // Plan/skill attachments
  std::vector<std::string> hookResults;      // Hook-provided followups
  std::vector<std::string> messagesToKeep;   // Recent messages to preserve
  std::string userDisplayMessage;            // Message to show user during compact
  int preCompactTokenCount = 0;
  int postCompactTokenCount = 0;
  int truePostCompactTokenCount = 0;
};

// ============================================================================
// Compact metadata for boundary messages
// ============================================================================
struct PreservedSegment {
  std::string headUuid;   // First preserved message UUID
  std::string anchorUuid; // Anchor point (last summary or boundary)
  std::string tailUuid;   // Last preserved message UUID
};

struct CompactMetadata {
  std::optional<PreservedSegment> preservedSegment;
  int messagesCompacted = 0;
  int tokensSaved = 0;
  std::string compactType;  // "auto", "manual", "reactive", "session_memory"
};

// ============================================================================
// Recompaction info (aligned with local-ace RecompactionInfo)
// ============================================================================
struct RecompactionInfo {
  bool isRecompactionInChain = false;
  int turnsSincePreviousCompact = 0;
  std::string previousCompactTurnId;
  int autoCompactThreshold = 0;
};

// ============================================================================
// Compact token budgets (aligned with local-ace constants)
// ============================================================================
struct CompactBudgets {
  static constexpr int PostCompactMaxFilesToRestore = 5;
  static constexpr int PostCompactTokenBudget = 50000;
  static constexpr int PostCompactMaxTokensPerFile = 5000;
  static constexpr int PostCompactMaxTokensPerSkill = 5000;
  static constexpr int PostCompactSkillsTokenBudget = 25000;
};

// ============================================================================
// Core functions
// ============================================================================

// Build the base post-compact messages array from a CompactionResult.
// Order: boundaryMarker ? summaryMessages ? messagesToKeep ? attachments ? hookResults
// (aligned with local-ace buildPostCompactMessages)
std::vector<std::string> BuildPostCompactMessages(const CompactionResult& result);

// Annotate a compact boundary with relink metadata for messagesToKeep.
// Preserved messages keep their original parentUuids on disk.
// anchorUuid = what sits immediately before keep[0] in the desired chain.
// (aligned with local-ace annotateBoundaryWithPreservedSegment)
std::string AnnotateBoundaryWithPreservedSegment(
    const std::string& boundary,
    const std::string& anchorUuid,
    const std::vector<std::string>& messagesToKeep);

// Merges user-supplied custom instructions with hook-provided instructions.
// User instructions come first; hook instructions are appended.
// (aligned with local-ace mergeHookInstructions)
std::string MergeHookInstructions(
    const std::string& userInstructions,
    const std::string& hookInstructions);

// Strip image blocks from messages (base64 inline images).
// (aligned with local-ace stripImagesFromMessages)
std::string StripImagesFromMessage(const std::string& message);

// Strip reinjected attachment markers from already-attached content.
// (aligned with local-ace stripReinjectedAttachments)
std::string StripReinjectedAttachments(const std::string& message);

// Truncate conversation head when Prompt Too Long error is detected.
// Preserves system prompt + user goal, removes middle messages.
// (aligned with local-ace truncateHeadForPTLRetry)
struct PTLTruncateResult {
  std::vector<std::string> truncatedMessages;
  std::string errorMessage;
  bool wasTruncated = false;
};
PTLTruncateResult TruncateHeadForPTLRetry(
    const std::vector<std::string>& messages,
    const std::string& systemPrompt,
    int maxTokens);

// Build error messages for common compact failures
std::string BuildNotEnoughMessagesError();
std::string BuildPromptTooLongError();
std::string BuildUserAbortError();
std::string BuildIncompleteResponseError();

// Create a plan attachment if one is pending in the compact context.
// Returns empty string if no plan is pending.
// (aligned with local-ace createPlanAttachmentIfNeeded)
std::string CreatePlanAttachmentIfNeeded(
    const std::string& planContent,
    int maxTokens = CompactBudgets::PostCompactMaxTokensPerFile);

// Create a skill attachment if one is pending.
// (aligned with local-ace createSkillAttachmentIfNeeded)
std::string CreateSkillAttachmentIfNeeded(
    const std::string& skillContent,
    int maxTokens = CompactBudgets::PostCompactMaxTokensPerSkill);

}  // namespace compact
}  // namespace agent
