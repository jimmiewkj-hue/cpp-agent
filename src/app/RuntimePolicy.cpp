#include "app/RuntimePolicy.h"

#include <sstream>

namespace agent {
namespace app {

namespace {

bool ShouldExposeBaseTool(const tools::ToolSchema& tool,
                          bool interactiveSession) {
  if (!interactiveSession && tool.name == "AskUserQuestion") {
    return false;
  }
  return true;
}

}  // namespace

std::vector<tools::ToolSchema> GetSessionBaseTools(bool interactiveSession) {
  const std::vector<tools::ToolSchema> baseTools =
      tools::ToolRegistry::GetAllBaseTools();
  std::vector<tools::ToolSchema> filtered;
  filtered.reserve(baseTools.size());
  for (const auto& tool : baseTools) {
    if (ShouldExposeBaseTool(tool, interactiveSession)) {
      filtered.push_back(tool);
    }
  }
  return filtered;
}

void RegisterSessionBaseTools(tools::ToolRegistry* registry,
                              bool interactiveSession) {
  if (registry == nullptr) return;
  for (const auto& tool : GetSessionBaseTools(interactiveSession)) {
    registry->RegisterTool(tool);
  }
}

std::string BuildWorkspaceSystemPrompt(const std::string& workspaceRoot,
                                       bool workspaceTrusted) {
  std::ostringstream prompt;
  prompt
      << "You are a helpful coding agent. Use the available tools to inspect "
      << "code, explain findings, and make careful changes when requested. "
      << "For analysis / investigation / understanding tasks, you MUST first "
      << "explore the workspace with Glob, Grep, or Read tools to understand "
      << "the existing files BEFORE creating or modifying any files. "
      << "Never write or edit files without first reading or searching the "
      << "relevant parts of the codebase. "
      << "When a tool result contains important facts, decisions, or file "
      << "creation progress that you will need later, write those facts down "
      << "in your next assistant message because older tool results may be "
      << "compacted or truncated later. ";
  if (workspaceTrusted && !workspaceRoot.empty()) {
    prompt
        << "The trusted workspace root is `" << workspaceRoot << "`. "
        << "Treat relative file paths as paths inside this workspace. "
        << "Create and modify project files inside this workspace, not inside "
        << "the session or memory directories unless the user explicitly asks "
        << "you to manage session memory. "
        << "This runtime is on Windows and shell commands execute in "
      << "PowerShell, not bash. Prefer explicit tools such as Read, Write, "
      << "Grep, Glob, Task*, NotebookEdit, and MCP resource tools over raw "
      << "shell commands whenever possible. For file and directory discovery, "
      << "do not use Bash or PowerShell listing commands like ls, dir, or "
      << "Get-ChildItem when Glob, Read, or Grep can answer the question. "
      << "When reading large files, do not read the whole file blindly: use "
      << "Read with offset/limit for a targeted line range, or use Grep "
      << "first to locate the relevant section. "
        << "If the user references files outside the workspace, read them via "
        << "an explicit absolute local path or ask the user to copy them into "
        << "the workspace first.";
  } else {
    prompt
        << "No workspace is currently trusted. Do not assume relative paths "
        << "refer to a project; use explicit absolute local paths when the "
        << "user references files outside the current session state. "
        << "This runtime is on Windows and shell commands execute in "
        << "PowerShell.";
  }
  return prompt.str();
}

std::vector<std::string> BuildStartupMessages(bool interactiveSession,
                                              bool workspaceTrusted,
                                              const std::string& workspaceRoot,
                                              std::size_t loadedHookFileCount) {
  std::vector<std::string> messages;
  if (!interactiveSession) return messages;

  messages.push_back("Ready. Type a prompt or command.");
  if (workspaceTrusted && !workspaceRoot.empty()) {
    messages.push_back("Trusted workspace: " + workspaceRoot);
  } else {
    messages.push_back(
        "Workspace not trusted. Use absolute paths for external files.");
  }
  if (loadedHookFileCount > 0) {
    messages.push_back("Loaded hook config files: " +
                       std::to_string(loadedHookFileCount));
  }
  return messages;
}

}  // namespace app
}  // namespace agent
