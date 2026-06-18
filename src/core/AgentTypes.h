#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace agent {
namespace core {

enum class MessageRole { User, Assistant, System };

enum class BlockType { Text, ToolUse, ToolResult, Image };

// Model family detection for adapting prompts, tool formats, and thresholds.
// Aligned with local-ace's model-specific behavior.
enum class ModelFamily { Claude, Qwen, Gemma, MiMo, Generic };

// Detect model family from model name string.
inline ModelFamily DetectModelFamily(const std::string& model) {
  std::string lower;
  lower.reserve(model.size());
  for (char ch : model) lower.push_back(static_cast<char>(std::tolower(ch)));
  if (lower.find("claude") != std::string::npos) return ModelFamily::Claude;
  if (lower.find("qwen") != std::string::npos) return ModelFamily::Qwen;
  if (lower.find("gemma") != std::string::npos) return ModelFamily::Gemma;
  if (lower.find("mimo") != std::string::npos) return ModelFamily::MiMo;
  return ModelFamily::Generic;
}

// Get context window size for a model family. Aligned with local-ace's
// getContextWindowForModel(). Supports env override via CPP_AGENT_CONTEXT_WINDOW.
inline int GetContextWindowForFamily(const std::string& model) {
  // Allow per-session override (aligned with local-ace CLAUDE_CODE_MAX_CONTEXT_TOKENS)
  if (const char* envOverride = std::getenv("CPP_AGENT_CONTEXT_WINDOW")) {
    int parsed = std::atoi(envOverride);
    if (parsed > 0) return parsed;
  }
  switch (DetectModelFamily(model)) {
    case ModelFamily::Claude: return 200000;   // Claude 3.5/4 standard
    case ModelFamily::Qwen:   return 200000;  // Qwen3 128K context
    case ModelFamily::Gemma:  return 200000;  // Gemma 128K context
    case ModelFamily::MiMo:   return 200000;  // MiMo 128K context
    case ModelFamily::Generic: return 200000; // Safe default for unknown models
  }
  return 200000;
}

// Get default max output tokens for a model family.
inline int GetMaxOutputTokensForFamily(const std::string& model) {
  switch (DetectModelFamily(model)) {
    case ModelFamily::Claude: return 8192;
    case ModelFamily::Qwen:   return 16384;
    case ModelFamily::Gemma:  return 16384;
    case ModelFamily::MiMo:   return 16384;  // MiMo supports larger outputs
    case ModelFamily::Generic: return 8192;
  }
  return 8192;
}

// Whether the model prefers OpenAI function-calling format.
inline bool ModelPrefersOpenAIFormat(const std::string& model) {
  ModelFamily fam = DetectModelFamily(model);
  return fam == ModelFamily::Qwen || fam == ModelFamily::Gemma ||
         fam == ModelFamily::MiMo || fam == ModelFamily::Generic;
}

// P2-4: Centralized per-family policy. Consolidates the 8+ scattered
// DetectModelFamily switch/if branches in QueryLoop.cpp and ModelClient.cpp
// into a single lookup point. Each field has a sensible default that matches
// the Generic/Claude baseline.
struct ModelFamilyPolicy {
  int maxVerifyNudges = 2;            // write-verify nudge budget
  int maxNoToolNudges = 10;           // no-tool-use continuation nudges
  int truncationTextThreshold = 2000; // bytes before suspecting max-tokens
  int truncationContinuationThreshold = 3;  // forced continuations before escalation
  double defaultTemperature = -1.0;   // -1 = not set (caller decides)
  int maxOutputTokens = 8192;         // default max output tokens
  bool preferChineseNudges = false;   // use Chinese no-tool nudge text
  // P0-2: max consecutive tool-only turns (tool call with negligible reasoning
  // text) before hard-terminating with "tool_only_loop". Local quantized models
  // (Gemma/Qwen) get a tighter cap than cloud models.
  int maxToolOnlyTurns = 2;
  // P0-1: message-count threshold to trigger autocompact/collapse proactively.
  // 0 = disabled (rely on token-estimate threshold only). Local quantized
  // models with a real n_ctx far below the nominal 200K context window need a
  // message-count trigger because the token estimate under-counts short CJK
  // messages, leaving context to grow unbounded (observed 1->160 messages).
  int compactMessageThreshold = 0;
  // RPT-1: max chars of text-only output (no tool_use) in a single turn before
  // hard-terminating with "oversized_planning_text". Logs show Gemma emitting
  // 1432-event / 75s planning monologues with zero tool calls; nudges failed to
  // break these 33% of the time. 0 = disabled.
  int maxTextOnlyOutputChars = 0;
  // RPT-2: session-wide cap on ForcedContinuation transitions. The per-cycle
  // maxNoToolNudges budget resets each turn, so without a global ceiling the
  // loop can ForceContinue 19+ times (observed). 0 = disabled.
  int maxForcedContinuations = 0;
};

// Get the centralized policy for a given model string.
inline ModelFamilyPolicy GetModelFamilyPolicy(const std::string& model) {
  ModelFamilyPolicy p;
  const ModelFamily fam = DetectModelFamily(model);
  switch (fam) {
    case ModelFamily::Qwen:
      p.maxVerifyNudges = 3;
      p.maxNoToolNudges = 15;
      p.truncationTextThreshold = 1500;
      p.truncationContinuationThreshold = 2;
      p.defaultTemperature = 0.7;
      p.maxOutputTokens = 16384;
      p.preferChineseNudges = true;
      p.maxToolOnlyTurns = 3;
      p.compactMessageThreshold = 60;
      p.maxTextOnlyOutputChars = 5000;
      p.maxForcedContinuations = 10;
      break;
    case ModelFamily::Gemma:
      p.maxVerifyNudges = 3;
      p.maxNoToolNudges = 30;
      p.truncationTextThreshold = 2000;
      p.truncationContinuationThreshold = 3;
      p.defaultTemperature = 0.6;
      p.maxOutputTokens = 16384;
      p.maxToolOnlyTurns = 4;
      p.compactMessageThreshold = 80;
      p.maxTextOnlyOutputChars = 6000;
      p.maxForcedContinuations = 12;
      break;
    case ModelFamily::MiMo:
      p.maxVerifyNudges = 2;
      p.maxNoToolNudges = 10;
      p.truncationTextThreshold = 1500;
      p.truncationContinuationThreshold = 2;
      p.defaultTemperature = 0.7;
      p.maxOutputTokens = 16384;
      p.maxToolOnlyTurns = 3;
      p.compactMessageThreshold = 50;
      p.maxTextOnlyOutputChars = 4000;
      p.maxForcedContinuations = 8;
      break;
    case ModelFamily::Claude:
    case ModelFamily::Generic:
    default:
      // Defaults already set in struct init
      break;
  }
  return p;
}

enum class QueryStage {
  ToolResultBudget,
  Snip,
  Microcompact,
  Collapse,
  Autocompact,
  ModelCall,
  Validator,
  StopHooks,
  RunTools,
  Completed
};

struct TextBlock {
  std::string text;
};

struct ToolUseBlock {
  std::string id;
  std::string name;
  std::string inputJson;
};

struct ToolResultBlock {
  std::string toolUseId;
  std::string content;
  bool isError = false;
};

struct ImageBlock {
  std::string base64Data;
  std::string mediaType;
};

struct Usage {
  int inputTokens = 0;
  int outputTokens = 0;
  int cacheReadInputTokens = 0;
  int cacheCreationInputTokens = 0;
};

struct ContentBlock {
  BlockType type = BlockType::Text;
  TextBlock asText;
  ToolUseBlock asToolUse;
  ToolResultBlock asToolResult;
  ImageBlock asImage;

  static ContentBlock MakeText(const std::string& text) {
    ContentBlock b;
    b.type = BlockType::Text;
    b.asText.text = text;
    return b;
  }

  static ContentBlock MakeImage(const std::string& base64Data,
                                const std::string& mediaType) {
    ContentBlock b;
    b.type = BlockType::Image;
    b.asImage.base64Data = base64Data;
    b.asImage.mediaType = mediaType;
    return b;
  }

  static ContentBlock MakeToolUse(const std::string& id,
                                  const std::string& name,
                                  const std::string& inputJson) {
    ContentBlock b;
    b.type = BlockType::ToolUse;
    b.asToolUse.id = id;
    b.asToolUse.name = name;
    b.asToolUse.inputJson = inputJson;
    return b;
  }

  static ContentBlock MakeToolResult(const std::string& toolUseId,
                                     const std::string& content,
                                     bool isError = false) {
    ContentBlock b;
    b.type = BlockType::ToolResult;
    b.asToolResult.toolUseId = toolUseId;
    b.asToolResult.content = content;
    b.asToolResult.isError = isError;
    return b;
  }
};

struct Message {
  MessageRole role = MessageRole::User;
  std::vector<ContentBlock> content;
  std::string uuid;
  bool isMeta = false;
  Usage usage;
  std::string stopReason;
  bool isApiErrorMessage = false;

  std::vector<ContentBlock> toolUseBlocks() const {
    std::vector<ContentBlock> result;
    for (const auto& b : content) {
      if (b.type == BlockType::ToolUse) result.push_back(b);
    }
    return result;
  }

  std::vector<ContentBlock> toolResultBlocks() const {
    std::vector<ContentBlock> result;
    for (const auto& b : content) {
      if (b.type == BlockType::ToolResult) result.push_back(b);
    }
    return result;
  }

  bool hasToolUse() const {
    for (const auto& b : content) {
      if (b.type == BlockType::ToolUse) return true;
    }
    return false;
  }
};

//  3 states, mapped to local-ace behavior: allow / deny / ask
//  'ask' means: classifier (auto-mode) or interactive confirmation required
enum class PermissionBehavior { Allow, Deny, Ask };

struct PermissionDecision {
  PermissionBehavior behavior = PermissionBehavior::Allow;
  std::string reason;
};

//  Callback for per-tool permission check, mirrors local-ace CanUseToolFn.
//  Implementations may call a classifier, prompt the user, or return immediately.
using CanUseTool = std::function<PermissionDecision(
    const ContentBlock& toolUse, const std::vector<Message>& messages)>;

struct QueryLoopEvent {
  enum class Type {
    StageChanged,
    AssistantMessage,
    UserMessage,
    ToolProgress,
    ToolResult,
    CompactionBoundary,
    Tombstone,
    LoopCompleted,
    ContextUsageUpdate,
  };
  Type type = Type::LoopCompleted;
  QueryStage stage = QueryStage::Completed;
  Message message;
  std::string terminalReason;
  int estimatedTokens = 0;
  int contextWindow = 0;
};

}  // namespace core
}  // namespace agent
