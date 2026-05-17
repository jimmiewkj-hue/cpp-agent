#pragma once

#include <functional>
#include <string>
#include <vector>

namespace agent {
namespace core {

enum class MessageRole { User, Assistant, System };

enum class BlockType { Text, ToolUse, ToolResult };

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

  static ContentBlock MakeText(const std::string& text) {
    ContentBlock b;
    b.type = BlockType::Text;
    b.asText.text = text;
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
