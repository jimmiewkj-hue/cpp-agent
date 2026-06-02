#pragma once

#include "tools/Tool.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agent {
namespace tools {

// ============================================================================
// ToolRegistry - collection of Tool instances (aligned with local-ace tools[])
// ============================================================================
class ToolRegistry {
 public:
  ToolRegistry() = default;

  // Register a tool. Takes ownership.
  void RegisterTool(std::unique_ptr<Tool> tool);

  // Backward compatibility: register from ToolSchema (creates wrapper Tool)
  void RegisterTool(const ToolSchema& schema);

  // Look up a tool by name (checks primary name + aliases)
  const Tool* FindTool(const std::string& name) const;
  Tool* FindTool(const std::string& name);

  // Check if a tool exists
  bool HasTool(const std::string& name) const;

  // List all registered tools
  std::vector<const Tool*> ListTools() const;

  // List all tool schemas as JSON for API requests
  std::vector<ToolSchema> ListToolSchemas() const;


  // Get all base tools as ToolSchema (backward compatibility for tests)
  static std::vector<ToolSchema> GetAllBaseToolSchemas();
  // Get all base tools (static factory for built-in tools)
  static std::vector<std::unique_ptr<Tool>> GetAllBaseTools();

  // Assemble full tool pool (base + MCP)
  std::vector<std::unique_ptr<Tool>> AssembleToolPool(
      const std::vector<std::string>& mcpToolNames,
      const std::vector<std::string>& mcpToolDescriptions,
      const std::vector<std::string>& mcpToolSchemasJson,
      const std::vector<bool>& mcpReadOnlyHints,
      const std::vector<bool>& mcpDestructiveHints);

  // Backward compatibility: delegate to FindTool
  bool IsConcurrencySafe(const std::string& name) const;
  bool IsReadOnly(const std::string& name) const;
  int MaxResultSizeChars(const std::string& name) const;

  // Safe allowlist
  bool IsInSafeAllowlist(const std::string& name) const;
  void AddToSafeAllowlist(const std::string& name);

 private:
  std::vector<std::unique_ptr<Tool>> tools_;
  std::vector<std::string> safeAllowlist_;
  mutable std::mutex mutex_;
};

}  // namespace tools
}  // namespace agent