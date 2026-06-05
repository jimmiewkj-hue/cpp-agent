#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <vector>

namespace agent {
namespace core {

enum class MessageRole { User, Assistant, System };

enum class BlockType { Text, ToolUse, ToolResult, Image };

// Model family detection for adapting prompts, tool formats, and thresholds.
// Aligned with local-ace's model-specific behavior.
enum class ModelFamily { Claude, Qwen, Gemma, Generic };

// Detect model family from model name string.
inline ModelFamily DetectModelFamily(const std::string& model) {
  std::string lower;
  lower.reserve(model.size());
  for (char ch : model) lower.push_back(static_cast<char>(std::tolower(ch)));
  if (lower.find("claude") != std::string::npos) return ModelFamily::Claude;
  if (lower.find("qwen") != std::string::npos) return ModelFamily::Qwen;
  if (lower.find("gemma") != std::string::npos) return ModelFamily::Gemma;
  return ModelFamily::Generic;
}

// Get context window size for a model family. Aligned with local-ace's
// getContextWindowForModel().
inline int GetContextWindowForFamily(const std::string& model) {
  switch (DetectModelFamily(model)) {
    case ModelFamily::Claude: return 200000;
    case ModelFamily::Qwen:   return 32768;   // Qwen 3 typical
    case ModelFamily::Gemma:  return 32768;   // Gemma 4 typical
    case ModelFamily::Generic: return 32768;
  }
  return 32768;
}

// Get default max output tokens for a model family.
inline int GetMaxOutputTokensForFamily(const std::string& model) {
  switch (DetectModelFamily(model)) {
    case ModelFamily::Claude: return 8192;
    case ModelFamily::Qwen:   return 8192;
    case ModelFamily::Gemma:  return 8192;
    case ModelFamily::Generic: return 8192;
  }
  return 8192;
}

// Whether the model prefers OpenAI function-calling format.
inline bool ModelPrefersOpenAIFormat(const std::string& model) {
  ModelFamily fam = DetectModelFamily(model);
  return fam == ModelFamily::Qwen || fam == ModelFamily::Gemma ||
         fam == ModelFamily::Generic;
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
  };
  Type type = Type::LoopCompleted;
  QueryStage stage = QueryStage::Completed;
  Message message;
  std::string terminalReason;
};

}  // namespace core
}  // namespace agent
