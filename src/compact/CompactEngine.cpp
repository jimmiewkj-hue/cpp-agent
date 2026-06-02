#include "compact/CompactEngine.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <regex>

namespace agent {
namespace compact {

// ============================================================================
// BuildPostCompactMessages (aligned with local-ace)
// ============================================================================
std::vector<std::string> BuildPostCompactMessages(const CompactionResult& result) {
  std::vector<std::string> messages;

  // 1. Boundary marker
  if (!result.boundaryMarker.empty()) {
    messages.push_back(result.boundaryMarker);
  }

  // 2. Summary messages
  for (const auto& sm : result.summaryMessages) {
    if (!sm.empty()) messages.push_back(sm);
  }

  // 3. Messages to keep (recent context)
  for (const auto& mk : result.messagesToKeep) {
    if (!mk.empty()) messages.push_back(mk);
  }

  // 4. Attachments (plans, skills)
  for (const auto& att : result.attachments) {
    if (!att.empty()) messages.push_back(att);
  }

  // 5. Hook results
  for (const auto& hr : result.hookResults) {
    if (!hr.empty()) messages.push_back(hr);
  }

  return messages;
}

// ============================================================================
// AnnotateBoundaryWithPreservedSegment (aligned with local-ace)
// ============================================================================
std::string AnnotateBoundaryWithPreservedSegment(
    const std::string& boundary,
    const std::string& anchorUuid,
    const std::vector<std::string>& messagesToKeep) {
  if (messagesToKeep.empty()) return boundary;

  std::ostringstream annotated;
  annotated << boundary;

  if (boundary.find("preservedSegment") == std::string::npos) {
    annotated << "\n[compact:preserved head=" << messagesToKeep.front().substr(0, 32)
              << " anchor=" << anchorUuid.substr(0, 32)
              << " tail=" << messagesToKeep.back().substr(0, 32) << "]";
  }

  return annotated.str();
}

// ============================================================================
// MergeHookInstructions (aligned with local-ace)
// ============================================================================
std::string MergeHookInstructions(
    const std::string& userInstructions,
    const std::string& hookInstructions) {
  if (hookInstructions.empty()) return userInstructions;
  if (userInstructions.empty()) return hookInstructions;
  return userInstructions + "\n\n" + hookInstructions;
}

// ============================================================================
// StripImagesFromMessage (aligned with local-ace)
// ============================================================================
std::string StripImagesFromMessage(const std::string& message) {
  // Remove base64-encoded image blocks: data:image/...;base64,...
  static const std::regex imagePattern(
      R"(data:image\/[^;]+;base64,[A-Za-z0-9+/=]+)",
      std::regex::optimize);

  std::string result = std::regex_replace(message, imagePattern, "[image removed]");

  // Also strip markdown image syntax: ![alt](data:image...)
  static const std::regex mdImagePattern(
      R"(!\[.*?\]\(data:image\/[^)]+\))",
      std::regex::optimize);

  result = std::regex_replace(result, mdImagePattern, "[image removed]");
  return result;
}

// ============================================================================
// StripReinjectedAttachments (aligned with local-ace)
// ============================================================================
std::string StripReinjectedAttachments(const std::string& message) {
  std::string result = message;

  // Remove [Plan attachment: ...] markers
  static const std::regex planPattern(R"(\[Plan attachment:.*?\])");
  result = std::regex_replace(result, planPattern, "");

  // Remove [Skill attachment: ...] markers
  static const std::regex skillPattern(R"(\[Skill attachment:.*?\])");
  result = std::regex_replace(result, skillPattern, "");

  return result;
}

// ============================================================================
// TruncateHeadForPTLRetry (aligned with local-ace)
// ============================================================================
PTLTruncateResult TruncateHeadForPTLRetry(
    const std::vector<std::string>& messages,
    const std::string& systemPrompt,
    int maxTokens) {
  PTLTruncateResult result;

  if (messages.size() < 3) {
    result.truncatedMessages = messages;
    result.errorMessage = "Not enough messages to truncate for PTL retry";
    return result;
  }

  // Estimate tokens (rough: 4 chars ? 1 token)
  auto estimateTokens = [](const std::string& text) -> int {
    return static_cast<int>(text.size()) / 4;
  };

  int sysTokens = estimateTokens(systemPrompt);
  int availableTokens = maxTokens - sysTokens;

  // Keep system prompt-equivalent + first user message + last N messages
  std::vector<std::string> truncated;

  // Always keep first user message (the original goal - index 1 after system prompt)
  if (messages.size() >= 2) {
    truncated.push_back(messages[1]);
  }

  // Add recent messages from the end until token budget is exhausted
  int usedTokens = 0;
  if (!truncated.empty()) usedTokens = estimateTokens(truncated[0]);

  for (int i = static_cast<int>(messages.size()) - 1; i >= 2 && usedTokens < availableTokens; --i) {
    int msgTokens = estimateTokens(messages[i]);
    if (usedTokens + msgTokens > availableTokens) break;
    // Insert after the first user message
    truncated.insert(truncated.begin() + 1, messages[i]);
    usedTokens += msgTokens;
  }

  result.truncatedMessages = truncated;
  result.wasTruncated = truncated.size() < messages.size();

  // Add truncation boundary marker
  if (result.wasTruncated) {
    std::ostringstream boundary;
    int oldTokens = 0;
    for (const auto& m : messages) oldTokens += estimateTokens(m);
    boundary << "[Context: " << (messages.size() - truncated.size())
             << " messages truncated due to context limit (~"
             << (oldTokens - usedTokens) << " tokens saved)]";
    truncated.insert(truncated.begin(), boundary.str());
  }

  return result;
}

// ============================================================================
// Error message builders (aligned with local-ace constants)
// ============================================================================
std::string BuildNotEnoughMessagesError() {
  return "Not enough messages to compact. Need at least one user-assistant exchange.";
}

std::string BuildPromptTooLongError() {
  return "The conversation has exceeded the context window limit. Some older "
         "messages have been summarized to continue the conversation. You can "
         "continue from where you left off.";
}

std::string BuildUserAbortError() {
  return "API Error: Request was aborted.";
}

std::string BuildIncompleteResponseError() {
  return "API Error: Incomplete response received. The model may have been "
         "interrupted. Please retry or continue from the last completed point.";
}

// ============================================================================
// CreatePlanAttachmentIfNeeded (aligned with local-ace)
// ============================================================================
std::string CreatePlanAttachmentIfNeeded(
    const std::string& planContent,
    int maxTokens) {
  if (planContent.empty()) return std::string();

  std::string truncated = planContent;
  // Truncate to token budget (4 chars ? 1 token)
  int maxChars = maxTokens * 4;
  if (static_cast<int>(truncated.size()) > maxChars) {
    truncated = truncated.substr(0, maxChars - 3) + "...";
  }

  return "[Plan attachment]\n" + truncated + "\n[/Plan attachment]";
}

// ============================================================================
// CreateSkillAttachmentIfNeeded (aligned with local-ace)
// ============================================================================
std::string CreateSkillAttachmentIfNeeded(
    const std::string& skillContent,
    int maxTokens) {
  if (skillContent.empty()) return std::string();

  std::string truncated = skillContent;
  int maxChars = maxTokens * 4;
  if (static_cast<int>(truncated.size()) > maxChars) {
    truncated = truncated.substr(0, maxChars - 3) + "...";
  }

  return "[Skill attachment]\n" + truncated + "\n[/Skill attachment]";
}

}  // namespace compact
}  // namespace agent
