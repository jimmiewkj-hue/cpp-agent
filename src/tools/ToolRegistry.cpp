#include "tools/ToolRegistry.h"

#include <algorithm>
#include <cctype>

namespace agent {
namespace tools {

static std::string ToolCategoryToStr(ToolExecCategory cat) {
  switch (cat) {
    case ToolExecCategory::ReadOnly:     return "ReadOnly";
    case ToolExecCategory::FileWrite:    return "FileWrite";
    case ToolExecCategory::ShellCommand: return "ShellCommand";
    case ToolExecCategory::SubAgent:     return "SubAgent";
    case ToolExecCategory::McpTool:      return "McpTool";
  }
  return "Unknown";
}

std::vector<ToolSchema> ToolRegistry::GetAllBaseTools() {
  std::vector<ToolSchema> tools;

  {
    ToolSchema t;
    t.name = "Bash";
    t.description = "Execute a shell command";
    t.inputSchemaJson = R"({"type":"object","properties":{"command":{"type":"string","description":"The command to execute"}},"required":["command"]})";
    t.category = ToolExecCategory::ShellCommand;
    t.readOnlyHint = false;
    t.destructiveHint = true;
    t.maxResultSizeChars = 400000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "FileRead";
    t.description = "Read a file from the filesystem";
    t.inputSchemaJson = R"({"type":"object","properties":{"file_path":{"type":"string","description":"Path to the file"}},"required":["file_path"]})";
    t.category = ToolExecCategory::ReadOnly;
    t.readOnlyHint = true;
    t.destructiveHint = false;
    t.maxResultSizeChars = 0;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "FileWrite";
    t.description = "Write content to a file";
    t.inputSchemaJson = R"({"type":"object","properties":{"file_path":{"type":"string"},"content":{"type":"string"}},"required":["file_path","content"]})";
    t.category = ToolExecCategory::FileWrite;
    t.readOnlyHint = false;
    t.destructiveHint = true;
    t.maxResultSizeChars = 100000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "Grep";
    t.description = "Search for a pattern in files";
    t.inputSchemaJson = R"({"type":"object","properties":{"pattern":{"type":"string"},"path":{"type":"string"}},"required":["pattern"]})";
    t.category = ToolExecCategory::ReadOnly;
    t.readOnlyHint = true;
    t.destructiveHint = false;
    t.maxResultSizeChars = 200000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "Glob";
    t.description = "Find files matching a glob pattern";
    t.inputSchemaJson = R"({"type":"object","properties":{"pattern":{"type":"string"},"path":{"type":"string"}},"required":["pattern"]})";
    t.category = ToolExecCategory::ReadOnly;
    t.readOnlyHint = true;
    t.destructiveHint = false;
    t.maxResultSizeChars = 100000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "Agent";
    t.description = "Spawn a sub-agent to handle complex multi-step tasks";
    t.inputSchemaJson = R"({"type":"object","properties":{"prompt":{"type":"string"},"description":{"type":"string"},"subagent_type":{"type":"string"},"run_in_background":{"type":"boolean"}},"required":["prompt"]})";
    t.category = ToolExecCategory::SubAgent;
    t.readOnlyHint = false;
    t.destructiveHint = true;
    t.maxResultSizeChars = 400000;
    tools.push_back(t);
  }

  return tools;
}

std::vector<ToolSchema> ToolRegistry::AssembleToolPool(
    const std::vector<std::string>& mcpToolNames,
    const std::vector<std::string>& mcpToolDescriptions,
    const std::vector<std::string>& mcpToolSchemasJson,
    const std::vector<bool>& mcpReadOnlyHints,
    const std::vector<bool>& mcpDestructiveHints) const {
  std::vector<ToolSchema> pool;

  {
    const auto list = ListTools();
    pool.insert(pool.end(), list.begin(), list.end());
  }

  {
    auto base = GetAllBaseTools();
    for (const auto& b : base) {
      bool exists = false;
      for (const auto& p : pool) {
        if (p.name == b.name) { exists = true; break; }
      }
      if (!exists) pool.push_back(b);
    }
  }

  for (std::size_t i = 0; i < mcpToolNames.size(); ++i) {
    ToolSchema mcp;
    mcp.name = mcpToolNames[i];
    mcp.description = i < mcpToolDescriptions.size()
                          ? mcpToolDescriptions[i] : "";
    mcp.inputSchemaJson = i < mcpToolSchemasJson.size()
                              ? mcpToolSchemasJson[i] : "{}";
    mcp.readOnlyHint = i < mcpReadOnlyHints.size() && mcpReadOnlyHints[i];
    mcp.destructiveHint =
        i < mcpDestructiveHints.size() && mcpDestructiveHints[i];
    mcp.category = ToolExecCategory::McpTool;

    bool exists = false;
    for (const auto& p : pool) {
      if (p.name == mcp.name) { exists = true; break; }
    }
    if (!exists) {
      pool.push_back(mcp);
    }
  }

  std::sort(pool.begin(), pool.end(),
            [](const ToolSchema& a, const ToolSchema& b) {
              return a.name < b.name;
            });

  return pool;
}

void ToolRegistry::RegisterTool(const ToolSchema& schema) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& existing : tools_) {
    if (existing.name == schema.name) {
      existing = schema;
      return;
    }
  }
  tools_.push_back(schema);
}

bool ToolRegistry::HasTool(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& tool : tools_) {
    if (tool.name == name) return true;
  }
  return false;
}

const ToolSchema* ToolRegistry::FindTool(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& tool : tools_) {
    if (tool.name == name) return &tool;
  }
  return nullptr;
}

std::vector<ToolSchema> ToolRegistry::ListTools() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tools_;
}

bool ToolRegistry::IsConcurrencySafe(const std::string& name) const {
  const ToolSchema* tool = FindTool(name);
  if (!tool) return false;
  return tool->readOnlyHint;
}

bool ToolRegistry::IsReadOnly(const std::string& name) const {
  const ToolSchema* tool = FindTool(name);
  if (!tool) return false;
  return tool->category == ToolExecCategory::ReadOnly;
}

int ToolRegistry::MaxResultSizeChars(const std::string& name) const {
  const ToolSchema* tool = FindTool(name);
  if (!tool) return 0;
  return tool->maxResultSizeChars;
}

bool ToolRegistry::IsInSafeAllowlist(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::find(safeAllowlist_.begin(), safeAllowlist_.end(), name) !=
         safeAllowlist_.end();
}

void ToolRegistry::AddToSafeAllowlist(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!IsInSafeAllowlist(name)) {
    safeAllowlist_.push_back(name);
  }
}

}  // namespace tools
}  // namespace agent
