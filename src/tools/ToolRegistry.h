#pragma once

#include "core/AgentTypes.h"
#include "core/StateTypes.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace agent {
namespace tools {

enum class ToolExecCategory {
  ReadOnly,
  FileWrite,
  ShellCommand,
  SubAgent,
  McpTool,
};

struct ToolSchema {
  std::string name;
  std::string description;
  std::string inputSchemaJson;
  ToolExecCategory category = ToolExecCategory::ReadOnly;
  bool readOnlyHint = false;
  bool destructiveHint = false;
  int maxResultSizeChars = 100000;
};

class ToolRegistry {
 public:
  void RegisterTool(const ToolSchema& schema);
  bool HasTool(const std::string& name) const;
  const ToolSchema* FindTool(const std::string& name) const;
  std::vector<ToolSchema> ListTools() const;
  bool IsConcurrencySafe(const std::string& name) const;
  bool IsReadOnly(const std::string& name) const;
  int MaxResultSizeChars(const std::string& name) const;

  bool IsInSafeAllowlist(const std::string& name) const;
  void AddToSafeAllowlist(const std::string& name);

  static std::vector<ToolSchema> GetAllBaseTools();
  std::vector<ToolSchema> AssembleToolPool(
      const std::vector<std::string>& mcpToolNames,
      const std::vector<std::string>& mcpToolDescriptions,
      const std::vector<std::string>& mcpToolSchemasJson,
      const std::vector<bool>& mcpReadOnlyHints,
      const std::vector<bool>& mcpDestructiveHints) const;

 private:
  std::vector<ToolSchema> tools_;
  std::vector<std::string> safeAllowlist_;
  mutable std::mutex mutex_;
};

}  // namespace tools
}  // namespace agent
