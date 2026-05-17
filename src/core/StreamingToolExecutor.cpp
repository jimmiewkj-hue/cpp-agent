#include "core/StreamingToolExecutor.h"

#include "permissions/PermissionEngine.h"

namespace agent {
namespace core {

StreamingToolExecutor::StreamingToolExecutor(
    tools::ToolOrchestrator& orchestrator,
    const std::vector<Message>& messages)
    : orchestrator_(orchestrator),
      messages_(messages) {}

void StreamingToolExecutor::AddTool(const ContentBlock& toolUse) {
  if (discarded_) return;

  TrackedTool tracked;
  tracked.id = toolUse.asToolUse.id;
  tracked.block = toolUse;
  const std::string& name = toolUse.asToolUse.name;
  tracked.isConcurrencySafe =
      (name == "FileRead" || name == "Grep" || name == "Glob");
  tracked.status = ToolExecStatus::Queued;
  tools_.push_back(tracked);
}

bool StreamingToolExecutor::HasPendingTools() const {
  for (const auto& tool : tools_) {
    if (tool.status == ToolExecStatus::Queued ||
        tool.status == ToolExecStatus::Executing) {
      return true;
    }
  }
  return false;
}

bool StreamingToolExecutor::HasCompletedTools() const {
  for (const auto& tool : tools_) {
    if (tool.resultReady) return true;
  }
  return false;
}

std::vector<ContentBlock> StreamingToolExecutor::YieldCompletedResults() {
  std::vector<ContentBlock> results;
  for (auto& tool : tools_) {
    if (tool.resultReady) {
      results.push_back(tool.result);
      tool.resultReady = false;
    }
  }
  return results;
}

void StreamingToolExecutor::ExecutePending() {
  if (discarded_ || hasErrored_) return;

  auto canUseTool = [](const ContentBlock&, const std::vector<Message>&) {
    PermissionDecision d;
    d.behavior = PermissionBehavior::Allow;
    d.reason = "streaming auto-allow";
    return d;
  };

  std::vector<ContentBlock> blocksToExecute;
  for (auto& tool : tools_) {
    if (tool.status == ToolExecStatus::Queued) {
      blocksToExecute.push_back(tool.block);
      tool.status = ToolExecStatus::Executing;
    }
  }

  if (blocksToExecute.empty()) return;

  tools::ToolOrchestrator::ExecuteResult execResult =
      orchestrator_.Execute(blocksToExecute, canUseTool, messages_);

  std::size_t resultIdx = 0;
  for (auto& tool : tools_) {
    if (tool.status == ToolExecStatus::Executing) {
      if (resultIdx < execResult.userMessages.size()) {
        const auto& msg = execResult.userMessages[resultIdx];
        if (!msg.content.empty()) {
          tool.result = msg.content[0];
          tool.resultReady = true;
        }
        ++resultIdx;
      }
      tool.status = ToolExecStatus::Completed;
    }
  }

  if (execResult.errorCount > 0) {
    hasErrored_ = true;
  }
}

void StreamingToolExecutor::Discard() {
  discarded_ = true;
  tools_.clear();
}

}  // namespace core
}  // namespace agent
