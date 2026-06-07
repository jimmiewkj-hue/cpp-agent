#include "app/RuntimePolicy.h"
#include "third_party/nlohmann_json.hpp"

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
  auto baseToolPtrs = tools::ToolRegistry::GetAllBaseTools();
  std::vector<tools::ToolSchema> baseTools;
  baseTools.reserve(baseToolPtrs.size());
  for (const auto& t : baseToolPtrs) {
    tools::ToolSchema schema;
    schema.name = t->Name();
    schema.description = t->UserFacingDescription();
    schema.inputSchemaJson = t->InputSchemaJson();
    nlohmann::json emptyInput = nlohmann::json::object();
    schema.readOnlyHint = t->IsReadOnly(emptyInput);
    schema.destructiveHint = t->IsDestructive(emptyInput);
    schema.maxResultSizeChars = t->MaxResultSizeChars();
    baseTools.push_back(schema);
  }
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
      << "compacted or truncated later. "

      // ============================================================
      // Write-Run-Verify closed loop (aligned with local-ace)
      // ============================================================
      << "\n\n# Write-Run-Verify Closed Loop (MANDATORY)\n"
      << "When you create or modify project files (code, config, scripts), "
      << "you MUST verify the result before marking the task as completed:\n"
      << "1. After writing code: run it with Bash and check the output\n"
      << "2. After writing config: validate the syntax\n"
      << "3. If there are tests: run them\n"
      << "4. If it is a library: check it compiles or imports correctly\n"
      << "5. If the output is wrong or has errors: fix the code and re-run\n\n"
      << "Do NOT mark a task as completed until you have verified the output. "
      << "If you cannot verify (no test exists, cannot run the code), say so "
      << "explicitly rather than claiming success. Never claim 'all tests pass' "
      << "when output shows failures, and never characterize incomplete or "
      << "broken work as done.\n\n"

      // ============================================================
      // Task management (aligned with local-ace TodoWrite/TaskUpdate)
      // ============================================================
      << "# Task Management\n"
      << "Use the TodoWrite tool to plan and track your work. Each todo item "
      << "MUST include:\n"
      << "- 'content': imperative form describing what needs to be done\n"
      << "- 'activeForm': present continuous form shown during execution\n"
      << "- 'acceptance_criteria': verifiable criteria that must ALL be met "
      << "to mark this task as completed\n"
      << "- 'status': one of pending, in_progress, completed, failed\n\n"
      << "When all tasks are completed and you have 3+ tasks, at least one "
      << "task MUST be a verification step (run, test, check, or build). "
      << "If none of your tasks involves verification, add one before "
      << "reporting completion.\n\n"

      // ============================================================
      // Error repair loop (aligned with local-ace)
      // ============================================================
      << "# Error Repair Loop\n"
      << "When a command or code execution produces errors:\n"
      << "1. Read the error message carefully\n"
      << "2. Identify the root cause (syntax error, missing dependency, "
      << "wrong path, logic error)\n"
      << "3. Fix the issue using FileEdit or Bash\n"
      << "4. Re-run to verify the fix works\n"
      << "5. If the fix fails, try a different approach instead of "
      << "repeating the same action\n\n"
      << "If an approach fails, diagnose why before switching tactics. "
      << "Do not retry the identical action blindly. If you are genuinely "
      << "stuck after investigation, report what you tried and what failed. "
      << "Do not spend many turns only reading or searching after a failing "
      << "run. Once you have enough evidence, your next turn should edit code, "
      << "run a verification command, or report a concrete blocker.\n\n"

      // ============================================================
      // Completion reporting (aligned with local-ace)
      // ============================================================
      << "# Completion Reporting\n"
      << "Before reporting that you have completed the user's request:\n"
      << "1. Verify ALL files were created/modified correctly\n"
      << "2. Run the code and confirm it produces expected output\n"
      << "3. If there are tests, run them and report results honestly\n"
      << "4. If something does not work, report the specific failure - "
      << "do not skip it or claim success\n"
      << "5. Provide a summary of what was done and what was verified\n\n"
      << "Report outcomes faithfully: if tests fail, say so with the "
      << "relevant output. If you did not run a verification step, say "
      << "that rather than implying it succeeded.\n"

      ;
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
