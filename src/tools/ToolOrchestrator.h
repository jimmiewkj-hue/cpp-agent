#pragma once

#include "core/AgentTypes.h"
#include "infra/ProcessRunner.h"

#include <string>
#include <vector>

namespace agent {
namespace agents { class SubAgentManager; }
namespace mcp { class McpClientManager; }
namespace tools {

class ToolRegistry;

struct ToolBatch {
  bool concurrentSafe = false;
  std::vector<core::ContentBlock> blocks;
};

class ToolOrchestrator {
 public:
  ToolOrchestrator();

  void SetToolRegistry(const ToolRegistry* registry);
  const ToolRegistry* GetToolRegistry() const { return toolRegistry_; }
  void SetSubAgentManager(agents::SubAgentManager* subAgentManager);
  void SetMcpClientManager(mcp::McpClientManager* mcpClientManager);
  void SetWorkspaceRoot(const std::string& workspaceRoot);
  const std::string& workspaceRoot() const { return workspaceRoot_; }

  std::vector<ToolBatch> PartitionToolCalls(
      const std::vector<core::ContentBlock>& toolUseBlocks) const;

  struct ExecuteResult {
    std::vector<core::Message> userMessages;
    int deniedCount = 0;
    int errorCount = 0;
  };

  ExecuteResult Execute(const std::vector<core::ContentBlock>& toolUseBlocks,
                        core::CanUseTool canUseTool,
                        const std::vector<core::Message>& messages) const;

 private:
  std::string ExecuteToolBlock(
      const core::ContentBlock& block,
      int maxResultSize,
      std::string* error) const;

  std::string ExecuteBash(const std::string& inputJson,
                          int maxResultSize,
                          std::string* error) const;
  std::string ExecuteFileRead(const std::string& inputJson,
                              int maxResultSize,
                              std::string* error) const;
  std::string ExecuteFileWrite(const std::string& inputJson,
                               int maxResultSize,
                               std::string* error) const;
  std::string ExecuteGrep(const std::string& inputJson,
                          int maxResultSize,
                          std::string* error) const;
  std::string ExecuteGlob(const std::string& inputJson,
                          int maxResultSize,
                          std::string* error) const;
  std::string ExecuteAgent(const std::string& inputJson,
                          int maxResultSize,
                          std::string* error) const;
  std::string ExecuteTodoWrite(const std::string& inputJson,
                               int maxResultSize,
                               std::string* error) const;
  std::string ExecuteTaskCreate(const std::string& inputJson,
                                int maxResultSize,
                                std::string* error) const;
  std::string ExecuteTaskGet(const std::string& inputJson,
                             int maxResultSize,
                             std::string* error) const;
  std::string ExecuteTaskUpdate(const std::string& inputJson,
                                int maxResultSize,
                                std::string* error) const;
  std::string ExecuteTaskList(const std::string& inputJson,
                              int maxResultSize,
                              std::string* error) const;
  std::string ExecuteTaskStop(const std::string& inputJson,
                              int maxResultSize,
                              std::string* error) const;
  std::string ExecuteAskUserQuestion(const std::string& inputJson,
                                     int maxResultSize,
                                     std::string* error) const;
  std::string ExecuteFileEdit(const std::string& inputJson,
                              int maxResultSize,
                              std::string* error) const;
  std::string ExecuteNotebookEdit(const std::string& inputJson,
                                  int maxResultSize,
                                  std::string* error) const;
  std::string ExecuteSkill(const std::string& inputJson,
                           int maxResultSize,
                           std::string* error) const;
  std::string ExecuteListMcpResources(const std::string& inputJson,
                                      int maxResultSize,
                                      std::string* error) const;
  std::string ExecuteReadMcpResource(const std::string& inputJson,
                                     int maxResultSize,
                                     std::string* error) const;
  std::string ExecuteWebFetch(const std::string& inputJson,
                              int maxResultSize,
                              std::string* error) const;
  std::string ExecuteWebSearch(const std::string& inputJson,
                               int maxResultSize,
                               std::string* error) const;

  static std::string TruncateResult(const std::string& result,
                                    int maxSize);

  const ToolRegistry* toolRegistry_ = nullptr;
  agents::SubAgentManager* subAgentManager_ = nullptr;
  mcp::McpClientManager* mcpClientManager_ = nullptr;
  infra::ProcessRunner processRunner_;
  std::string workspaceRoot_;
};

}  // namespace tools
}  // namespace agent
