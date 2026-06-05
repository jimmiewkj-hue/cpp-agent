#include "tools/ToolRegistry.h"
#include "third_party/nlohmann_json.hpp"

#include <algorithm>
#include <cctype>

namespace agent {
namespace tools {

using json = nlohmann::json;

// ============================================================================
// P0-03: Case-insensitive comparison for tool name matching (aligned with local-ace)
// ============================================================================
namespace {
bool CaseInsensitiveCompare(const std::string& a, const std::string& b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(),
                    [](char ca, char cb) {
                      return std::tolower(static_cast<unsigned char>(ca)) ==
                             std::tolower(static_cast<unsigned char>(cb));
                    });
}
}  // namespace

// ============================================================================
// RegisterTool (unique_ptr<Tool>)
// ============================================================================
void ToolRegistry::RegisterTool(std::unique_ptr<Tool> tool) {
  if (!tool) return;
  std::lock_guard<std::mutex> lock(mutex_);
  tools_.push_back(std::move(tool));
}

// ============================================================================
// RegisterTool (ToolSchema — backward compatibility wrapper)
// ============================================================================
void ToolRegistry::RegisterTool(const ToolSchema& schema) {
  ToolDef def;
  def.name = schema.name;
  def.description = schema.description;
  def.inputSchemaJson = schema.inputSchemaJson;
  def.readOnlyHint = schema.readOnlyHint;
  def.destructiveHint = schema.destructiveHint;
  def.maxResultSizeChars = schema.maxResultSizeChars;
  def.isConcurrencySafe = [hint = schema.readOnlyHint](const json&) { return hint; };
  def.call = [](const json&, const ToolUseContext&, ToolProgressCallback) -> ToolCallResult {
    ToolCallResult result;
    result.ok = true;
    result.metadata["delegated"] = true;
    return result;
  };
  RegisterTool(BuildTool(std::move(def)));
}

// ============================================================================
// FindTool (case-insensitive, checks primary name + aliases)
// ============================================================================
const Tool* ToolRegistry::FindTool(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  // P0-03: Case-insensitive matching (aligned with local-ace)
  for (const auto& tool : tools_) {
    if (CaseInsensitiveCompare(tool->Name(), name)) return tool.get();
    for (const auto& alias : tool->Aliases()) {
      if (CaseInsensitiveCompare(alias, name)) return tool.get();
    }
  }
  return nullptr;
}

Tool* ToolRegistry::FindTool(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  // P0-03: Case-insensitive matching (aligned with local-ace)
  for (auto& tool : tools_) {
    if (CaseInsensitiveCompare(tool->Name(), name)) return tool.get();
    for (const auto& alias : tool->Aliases()) {
      if (CaseInsensitiveCompare(alias, name)) return tool.get();
    }
  }
  return nullptr;
}

// ============================================================================
// HasTool
// ============================================================================
bool ToolRegistry::HasTool(const std::string& name) const {
  return FindTool(name) != nullptr;
}

// ============================================================================
// ListTools
// ============================================================================
std::vector<const Tool*> ToolRegistry::ListTools() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<const Tool*> result;
  result.reserve(tools_.size());
  for (const auto& tool : tools_) {
    result.push_back(tool.get());
  }
  return result;
}

// ============================================================================
// ListToolSchemas
// ============================================================================
std::vector<ToolSchema> ToolRegistry::ListToolSchemas() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ToolSchema> result;
  result.reserve(tools_.size());
  for (const auto& tool : tools_) {
    ToolSchema schema;
    schema.name = tool->Name();
    schema.description = tool->UserFacingDescription();
    schema.inputSchemaJson = tool->InputSchemaJson();
    schema.readOnlyHint = tool->IsReadOnly({});
    schema.destructiveHint = tool->IsDestructive({});
    schema.maxResultSizeChars = tool->MaxResultSizeChars();
    result.push_back(schema);
  }
  return result;
}

// ============================================================================
// GetAllBaseTools (static factory — aligned with local-ace getAllBaseTools)
// ============================================================================
std::vector<std::unique_ptr<Tool>> ToolRegistry::GetAllBaseTools() {
  std::vector<std::unique_ptr<Tool>> tools;

  // Bash tool (aligned with local-ace BashTool/PowerShellTool)
  {
    ToolDef def;
    def.name = "Bash";
    def.description =
        "Executes a given command in a PowerShell shell.\n"
        "- The shell is stateful: environment variables, current directory, "
        "and other state persist across commands.\n"
        "- Commands execute in the workspace root by default.\n"
        "- Long-running commands can be configured with a timeout (default 120s).\n"
        "- Use PowerShell commands (Select-String, Get-ChildItem) instead of "
        "Unix equivalents (grep, ls).\n"
        "- For Python, use python or python -c for inline scripts.";
    def.inputSchemaJson = "{\n"
      "  \"type\": \"object\",\n"
      "  \"properties\": {\n"
      "    \"command\": {\"type\": \"string\", \"description\": \"The shell command to execute\"},\n"
      "    \"description\": {\"type\": \"string\", \"description\": \"A short description of what this command does\"},\n"
      "    \"timeout\": {\"type\": \"number\", \"description\": \"Optional timeout in milliseconds (default 120000)\"}\n"
      "  },\n"
      "  \"required\": [\"command\"]\n"
      "}";
    def.destructiveHint = true;
    def.maxResultSizeChars = 400000;
    def.aliases = {"bash", "shell", "exec"};

    def.call = [](const json&, const ToolUseContext&, ToolProgressCallback) -> ToolCallResult {
      ToolCallResult result;
      result.ok = true;
      result.metadata["delegated"] = true;
      return result;
    };

    tools.push_back(BuildTool(std::move(def)));
  }

  // Read tool
  {
    ToolDef def;
    def.name = "Read";
    def.description = "Reads a file from the local filesystem. You can access any file directly by using this tool.";
    def.inputSchemaJson = R"({
      "type": "object",
      "properties": {
        "file_path": {"type": "string", "description": "The path to the file to read"},
        "offset": {"type": "integer", "minimum": 1, "description": "Optional 1-based start line for partial reads"},
        "limit": {"type": "integer", "minimum": 1, "description": "Optional number of lines to return starting at offset"}
      },
      "required": ["file_path"]
    })";
    def.readOnlyHint = true;
    def.isConcurrencySafe = [](const json&) { return true; };
    def.maxResultSizeChars = 100000;
    def.aliases = {"FileRead", "read_file", "file_read"};
    def.searchHint = "read file content";

    def.call = [](const json&, const ToolUseContext&, ToolProgressCallback) -> ToolCallResult {
      ToolCallResult result;
      result.ok = true;
      result.metadata["delegated"] = true;
      return result;
    };

    tools.push_back(BuildTool(std::move(def)));
  }

  // Write tool
  {
    ToolDef def;
    def.name = "Write";
    def.description = "Writes a file to the local filesystem.";
    def.inputSchemaJson = R"({
      "type": "object",
      "properties": {
        "file_path": {"type": "string", "description": "The path to the file to write"},
        "content": {"type": "string", "description": "The content to write to the file"}
      },
      "required": ["file_path", "content"]
    })";
    def.destructiveHint = true;
    def.maxResultSizeChars = 100000;
    def.aliases = {"FileWrite", "write_file", "file_write"};
    def.searchHint = "write create save file";

    def.call = [](const json&, const ToolUseContext&, ToolProgressCallback) -> ToolCallResult {
      ToolCallResult result;
      result.ok = true;
      result.metadata["delegated"] = true;
      return result;
    };

    tools.push_back(BuildTool(std::move(def)));
  }

  // Glob tool
  {
    ToolDef def;
    def.name = "Glob";
    def.description = "Find files matching a glob pattern.";
    def.inputSchemaJson = R"({
      "type": "object",
      "properties": {
        "pattern": {"type": "string", "description": "The glob pattern to match files against"}
      },
      "required": ["pattern"]
    })";
    def.readOnlyHint = true;
    def.maxResultSizeChars = 50000;
    def.aliases = {"glob", "ls"};
    def.searchHint = "find list files directory";

    def.call = [](const json&, const ToolUseContext&, ToolProgressCallback) -> ToolCallResult {
      ToolCallResult result;
      result.ok = true;
      result.metadata["delegated"] = true;
      return result;
    };

    tools.push_back(BuildTool(std::move(def)));
  }

  // Grep tool
  {
    ToolDef def;
    def.name = "Grep";
    def.description = "Search for a pattern in files.";
    def.inputSchemaJson = R"({
      "type": "object",
      "properties": {
        "pattern": {"type": "string", "description": "The regex pattern to search for"},
        "path": {"type": "string", "description": "The path to search in"}
      },
      "required": ["pattern"]
    })";
    def.readOnlyHint = true;
    def.maxResultSizeChars = 50000;
    def.aliases = {"grep", "search"};
    def.searchHint = "search grep find text pattern";

    def.call = [](const json&, const ToolUseContext&, ToolProgressCallback) -> ToolCallResult {
      ToolCallResult result;
      result.ok = true;
      result.metadata["delegated"] = true;
      return result;
    };

    tools.push_back(BuildTool(std::move(def)));
  }

  // TodoWrite tool
  {
    ToolDef def;
    def.name = "TodoWrite";
    def.description = "Create and manage a task list for your current coding session.";
    def.inputSchemaJson = R"({
      "type": "object",
      "properties": {
        "todos": {"type": "array", "description": "The list of todo items"}
      },
      "required": ["todos"]
    })";
    def.readOnlyHint = false;
    def.maxResultSizeChars = 10000;
    def.aliases = {"TodoWrite", "todo_write"};

    def.call = [](const json&, const ToolUseContext&, ToolProgressCallback) -> ToolCallResult {
      ToolCallResult result;
      result.ok = true;
      result.metadata["delegated"] = true;
      return result;
    };

    tools.push_back(BuildTool(std::move(def)));
  }

  // AskUserQuestion tool
  {
    ToolDef def;
    def.name = "AskUserQuestion";
    def.description = "Ask the user a question to gather additional information.";
    def.inputSchemaJson = R"({
      "type": "object",
      "properties": {
        "questions": {"type": "array", "description": "List of questions"}
      },
      "required": ["questions"]
    })";
    def.readOnlyHint = false;
    def.isConcurrencySafe = [](const json&) { return false; };
    def.maxResultSizeChars = 10000;
    def.aliases = {"ask_user_question", "AskUser"};

    def.call = [](const json&, const ToolUseContext&, ToolProgressCallback) -> ToolCallResult {
      ToolCallResult result;
      result.ok = true;
      result.metadata["delegated"] = true;
      return result;
    };

    tools.push_back(BuildTool(std::move(def)));
  }

  return tools;
}

// ============================================================================
// AssembleToolPool
// ============================================================================
std::vector<std::unique_ptr<Tool>> ToolRegistry::AssembleToolPool(
    const std::vector<std::string>& mcpToolNames,
    const std::vector<std::string>& mcpToolDescriptions,
    const std::vector<std::string>& mcpToolSchemasJson,
    const std::vector<bool>& mcpReadOnlyHints,
    const std::vector<bool>& mcpDestructiveHints) {

  auto tools = GetAllBaseTools();

  for (size_t i = 0; i < mcpToolNames.size(); ++i) {
    ToolDef def;
    def.name = "mcp__" + mcpToolNames[i];
    def.description = i < mcpToolDescriptions.size() ? mcpToolDescriptions[i] : "";
    def.inputSchemaJson = i < mcpToolSchemasJson.size() ? mcpToolSchemasJson[i] : "{}";
    def.readOnlyHint = i < mcpReadOnlyHints.size() ? mcpReadOnlyHints[i] : false;
    def.destructiveHint = i < mcpDestructiveHints.size() ? mcpDestructiveHints[i] : false;
    def.maxResultSizeChars = 100000;

    def.call = [](const json&, const ToolUseContext&, ToolProgressCallback) -> ToolCallResult {
      ToolCallResult result;
      result.ok = true;
      result.metadata["delegated"] = true;
      result.metadata["mcp"] = true;
      return result;
    };

    tools.push_back(BuildTool(std::move(def)));
  }

  return tools;
}

// ============================================================================
// IsConcurrencySafe / IsReadOnly / MaxResultSizeChars
// ============================================================================
bool ToolRegistry::IsConcurrencySafe(const std::string& name) const {
  const Tool* tool = FindTool(name);
  if (!tool) return false;
  return tool->IsConcurrencySafe({});
}

bool ToolRegistry::IsReadOnly(const std::string& name) const {
  const Tool* tool = FindTool(name);
  if (!tool) return false;
  return tool->IsReadOnly({});
}

int ToolRegistry::MaxResultSizeChars(const std::string& name) const {
  const Tool* tool = FindTool(name);
  if (!tool) return 0;
  return tool->MaxResultSizeChars();
}

// ============================================================================
// Safe allowlist
// ============================================================================
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


// ============================================================================
// GetAllBaseToolSchemas (backward compatibility)
// ============================================================================
std::vector<ToolSchema> ToolRegistry::GetAllBaseToolSchemas() {
  auto tools = GetAllBaseTools();
  std::vector<ToolSchema> schemas;
  schemas.reserve(tools.size());
  for (auto& tool : tools) {
    ToolSchema schema;
    schema.name = tool->Name();
    schema.description = tool->UserFacingDescription();
    schema.inputSchemaJson = tool->InputSchemaJson();
    schema.readOnlyHint = tool->IsReadOnly({});
    schema.destructiveHint = tool->IsDestructive({});
    schema.maxResultSizeChars = tool->MaxResultSizeChars();
    schemas.push_back(schema);
  }
  return schemas;
}

}  // namespace tools
}  // namespace agent