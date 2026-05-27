#include "tools/ToolRegistry.h"

#include <algorithm>
#include <cctype>

namespace agent {
namespace tools {

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
    t.name = "Read";
    t.description = "Read a file from the filesystem; relative paths resolve from the trusted workspace root";
    t.inputSchemaJson = R"({"type":"object","properties":{"file_path":{"type":"string","description":"Path to the file. Use an absolute path for files outside the current workspace."}},"required":["file_path"]})";
    t.category = ToolExecCategory::ReadOnly;
    t.readOnlyHint = true;
    t.destructiveHint = false;
    t.maxResultSizeChars = 0;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "FileRead";
    t.description = "Read a file from the filesystem; relative paths resolve from the trusted workspace root";
    t.inputSchemaJson = R"({"type":"object","properties":{"file_path":{"type":"string","description":"Path to the file. Use an absolute path for files outside the current workspace."}},"required":["file_path"]})";
    t.category = ToolExecCategory::ReadOnly;
    t.readOnlyHint = true;
    t.destructiveHint = false;
    t.maxResultSizeChars = 0;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "Write";
    t.description = "Write content to a file inside the trusted workspace; use relative paths for project files";
    t.inputSchemaJson = R"({"type":"object","properties":{"file_path":{"type":"string","description":"Destination path inside the trusted workspace. Relative paths are preferred for project files."},"content":{"type":"string"}},"required":["file_path","content"]})";
    t.category = ToolExecCategory::FileWrite;
    t.readOnlyHint = false;
    t.destructiveHint = true;
    t.maxResultSizeChars = 100000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "FileWrite";
    t.description = "Write content to a file inside the trusted workspace; use relative paths for project files";
    t.inputSchemaJson = R"({"type":"object","properties":{"file_path":{"type":"string","description":"Destination path inside the trusted workspace. Relative paths are preferred for project files."},"content":{"type":"string"}},"required":["file_path","content"]})";
    t.category = ToolExecCategory::FileWrite;
    t.readOnlyHint = false;
    t.destructiveHint = true;
    t.maxResultSizeChars = 100000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "Grep";
    t.description = "Search for a pattern in files; relative paths resolve from the trusted workspace root";
    t.inputSchemaJson = R"({"type":"object","properties":{"pattern":{"type":"string"},"path":{"type":"string","description":"Optional file or directory path. Use an absolute path for locations outside the current workspace."}},"required":["pattern"]})";
    t.category = ToolExecCategory::ReadOnly;
    t.readOnlyHint = true;
    t.destructiveHint = false;
    t.maxResultSizeChars = 200000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "Glob";
    t.description = "Find files matching a glob pattern; relative paths resolve from the trusted workspace root";
    t.inputSchemaJson = R"({"type":"object","properties":{"pattern":{"type":"string"},"path":{"type":"string","description":"Optional directory path. Use an absolute path for locations outside the current workspace."}},"required":["pattern"]})";
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
  {
    ToolSchema t;
    t.name = "TodoWrite";
    t.description = "Create and manage a structured task list for this session";
    t.inputSchemaJson = R"({"type":"object","properties":{"todos":{"type":"array","items":{"type":"object","properties":{"content":{"type":"string"},"status":{"type":"string","enum":["pending","in_progress","completed"]},"id":{"type":"string"},"priority":{"type":"string","enum":["high","medium","low"]}},"required":["content","status","id","priority"]}}},"required":["todos"]})";
    t.category = ToolExecCategory::FileWrite;
    t.readOnlyHint = false;
    t.destructiveHint = true;
    t.maxResultSizeChars = 100000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "TaskCreate";
    t.description = "Create a structured task for the current session task list";
    t.inputSchemaJson = R"({"type":"object","properties":{"subject":{"type":"string"},"description":{"type":"string"},"activeForm":{"type":"string"},"metadata":{"type":"object"}},"required":["subject","description"]})";
    t.category = ToolExecCategory::FileWrite;
    t.readOnlyHint = false;
    t.destructiveHint = true;
    t.maxResultSizeChars = 100000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "TaskGet";
    t.description = "Get one task from the current session task list by id";
    t.inputSchemaJson = R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})";
    t.category = ToolExecCategory::ReadOnly;
    t.readOnlyHint = true;
    t.destructiveHint = false;
    t.maxResultSizeChars = 100000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "TaskUpdate";
    t.description = "Update one task in the current session task list";
    t.inputSchemaJson = R"({"type":"object","properties":{"id":{"type":"string"},"subject":{"type":"string"},"description":{"type":"string"},"activeForm":{"type":"string"},"status":{"type":"string","enum":["pending","in_progress","completed","blocked","cancelled"]},"owner":{"type":"string"},"blockedBy":{"type":"array","items":{"type":"string"}},"metadata":{"type":"object"}},"required":["id"]})";
    t.category = ToolExecCategory::FileWrite;
    t.readOnlyHint = false;
    t.destructiveHint = true;
    t.maxResultSizeChars = 100000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "TaskList";
    t.description = "List all tasks for the current session task list";
    t.inputSchemaJson = R"({"type":"object","properties":{}})";
    t.category = ToolExecCategory::ReadOnly;
    t.readOnlyHint = true;
    t.destructiveHint = false;
    t.maxResultSizeChars = 100000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "TaskStop";
    t.description = "Stop or cancel a task in the current session task list";
    t.inputSchemaJson = R"({"type":"object","properties":{"id":{"type":"string"},"reason":{"type":"string"}},"required":["id"]})";
    t.category = ToolExecCategory::FileWrite;
    t.readOnlyHint = false;
    t.destructiveHint = true;
    t.maxResultSizeChars = 100000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "AskUserQuestion";
    t.description = "Ask the user clarifying questions when more information is needed";
    t.inputSchemaJson = R"({"type":"object","properties":{"questions":{"type":"array","items":{"type":"object","properties":{"question":{"type":"string"},"header":{"type":"string"},"options":{"type":"array","items":{"type":"object","properties":{"label":{"type":"string"},"description":{"type":"string"}},"required":["label","description"]}},"multiSelect":{"type":"boolean"}},"required":["question","header","options","multiSelect"]}}},"required":["questions"]})";
    t.category = ToolExecCategory::ReadOnly;
    t.readOnlyHint = true;
    t.destructiveHint = false;
    t.maxResultSizeChars = 50000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "FileEdit";
    t.description = "Make precise edits to an existing file inside the trusted workspace";
    t.inputSchemaJson = R"({"type":"object","properties":{"file_path":{"type":"string","description":"Path to an existing file inside the trusted workspace."},"old_string":{"type":"string"},"new_string":{"type":"string"},"replace_all":{"type":"boolean"}},"required":["file_path","old_string","new_string"]})";
    t.category = ToolExecCategory::FileWrite;
    t.readOnlyHint = false;
    t.destructiveHint = true;
    t.maxResultSizeChars = 100000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "NotebookEdit";
    t.description = "Edit a Jupyter notebook cell by replacing, inserting, or deleting a cell";
    t.inputSchemaJson = R"({"type":"object","properties":{"notebook_path":{"type":"string"},"cell_id":{"type":"string"},"new_source":{"type":"string"},"cell_type":{"type":"string","enum":["code","markdown"]},"edit_mode":{"type":"string","enum":["replace","insert","delete"]}},"required":["notebook_path"]})";
    t.category = ToolExecCategory::FileWrite;
    t.readOnlyHint = false;
    t.destructiveHint = true;
    t.maxResultSizeChars = 100000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "Skill";
    t.description = "Execute a built-in skill in a forked sub-agent context";
    t.inputSchemaJson = R"({"type":"object","properties":{"command":{"type":"string"},"args":{"type":"string"}},"required":["command"]})";
    t.category = ToolExecCategory::SubAgent;
    t.readOnlyHint = false;
    t.destructiveHint = true;
    t.maxResultSizeChars = 400000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "ListMcpResources";
    t.description = "List resources exposed by connected MCP servers";
    t.inputSchemaJson = R"({"type":"object","properties":{"server":{"type":"string"}}})";
    t.category = ToolExecCategory::ReadOnly;
    t.readOnlyHint = true;
    t.destructiveHint = false;
    t.maxResultSizeChars = 100000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "ReadMcpResource";
    t.description = "Read one MCP resource by server name and resource URI";
    t.inputSchemaJson = R"({"type":"object","properties":{"server":{"type":"string"},"uri":{"type":"string"}},"required":["server","uri"]})";
    t.category = ToolExecCategory::ReadOnly;
    t.readOnlyHint = true;
    t.destructiveHint = false;
    t.maxResultSizeChars = 100000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "WebFetch";
    t.description = "Fetch the contents of a URL and convert HTML to markdown";
    t.inputSchemaJson = R"({"type":"object","properties":{"url":{"type":"string","description":"The URL to fetch"}},"required":["url"]})";
    t.category = ToolExecCategory::ReadOnly;
    t.readOnlyHint = true;
    t.destructiveHint = false;
    t.maxResultSizeChars = 200000;
    tools.push_back(t);
  }
  {
    ToolSchema t;
    t.name = "WebSearch";
    t.description = "Search the web for information";
    t.inputSchemaJson = R"({"type":"object","properties":{"query":{"type":"string","description":"The search query"},"num":{"type":"integer","description":"Max results"}},"required":["query"]})";
    t.category = ToolExecCategory::ReadOnly;
    t.readOnlyHint = true;
    t.destructiveHint = false;
    t.maxResultSizeChars = 100000;
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
