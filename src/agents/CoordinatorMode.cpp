#include "agents/CoordinatorMode.h"

#include <cstdlib>
#include <sstream>
#include <algorithm>

namespace agent {
namespace coordinator {

// ============================================================================
// Coordinator mode detection
// ============================================================================

bool IsCoordinatorMode() {
  // STRENGTHEN-T26: prefer the neutral CPP_AGENT_COORDINATOR_MODE env var;
  // keep CLAUDE_CODE_COORDINATOR_MODE for backward compat with configs
  // migrated from the upstream local-ace project.
  const char* env = std::getenv("CPP_AGENT_COORDINATOR_MODE");
  if (!env) env = std::getenv("CLAUDE_CODE_COORDINATOR_MODE");
  if (!env) return false;
  return std::atoi(env) != 0 || env[0] == '1' || env[0] == 'y' || env[0] == 'Y';
}

std::string MatchSessionMode(const std::string& sessionMode) {
  if (sessionMode.empty()) return std::string();

  bool currentIsCoordinator = IsCoordinatorMode();
  bool sessionIsCoordinator = (sessionMode == "coordinator");

  if (currentIsCoordinator == sessionIsCoordinator) return std::string();

  // Flip the env var (set both the neutral and legacy names for consistency)
  if (sessionIsCoordinator) {
    _putenv("CPP_AGENT_COORDINATOR_MODE=1");
    _putenv("CLAUDE_CODE_COORDINATOR_MODE=1");
    return "Entered coordinator mode to match resumed session.";
  } else {
    _putenv("CPP_AGENT_COORDINATOR_MODE=");
    _putenv("CLAUDE_CODE_COORDINATOR_MODE=");
    return "Exited coordinator mode to match resumed session.";
  }
}

// ============================================================================
// Tool sets (aligned with local-ace)
// ============================================================================

std::vector<std::string> GetInternalWorkerTools() {
  return {"TeamCreate", "TeamDelete", "SendMessage", "SyntheticOutput"};
}

std::vector<std::string> GetWorkerTools(bool simpleMode) {
  if (simpleMode) {
    return {"Bash", "Read", "FileEdit"};
  }
  // Standard worker tools (from local-ace ASYNC_AGENT_ALLOWED_TOOLS)
  return {
    "Bash", "Read", "FileEdit", "FileWrite", "Glob", "Grep",
    "TaskCreate", "TaskUpdate", "TaskList", "TaskGet", "TaskStop",
    "TodoWrite", "WebFetch", "WebSearch", "Skill", "NotebookEdit",
    "AskUserQuestion", "ListMcpResources", "ReadMcpResource"
  };
}

// ============================================================================
// System prompt
// ============================================================================

std::string BuildCoordinatorSystemPrompt(const CoordinatorConfig& config) {
  std::string workerCapabilities;
  if (config.simpleMode) {
    workerCapabilities = "Workers have access to Bash, Read, and Edit tools, "
                         "plus MCP tools from configured MCP servers.";
  } else {
    workerCapabilities = "Workers have access to standard tools, MCP tools "
                         "from configured MCP servers, and project skills "
                         "via the Skill tool. Delegate skill invocations "
                         "to workers.";
  }

  std::ostringstream prompt;
  prompt << R"(You are the Cpp-Agent Coordinator, an AI assistant that orchestrates software engineering tasks across multiple workers.

## 1. Your Role

You are a **coordinator**. Your job is to:
- Help the user achieve their goal
- Direct workers to research, implement and verify code changes
- Synthesize results and communicate with the user
- Answer questions directly when possible

Every message you send is to the user. Worker results and system notifications are internal signals, not conversation partners ? never thank or acknowledge them.

## 2. Your Tools

- **Agent** - Spawn a new worker
- **SendMessage** - Continue an existing worker
- **TaskStop** - Stop a running worker

When calling Agent:
- Do not use one worker to check on another.
- Do not use workers for trivial tasks. Give them higher-level tasks.
- After launching agents, briefly tell the user what you launched and end your response.
- Never fabricate or predict agent results ? results arrive as separate messages.

### Agent Results
Worker results arrive as user-role messages containing `<task-notification>` XML.

Format:
```xml
<task-notification>
<task-id>{agentId}</task-id>
<status>completed|failed|killed</status>
<summary>{human-readable status summary}</summary>
<result>{agent's final text response}</result>
<usage>
  <total_tokens>N</total_tokens>
  <tool_uses>N</tool_uses>
  <duration_ms>N</duration_ms>
</usage>
</task-notification>
```

## 3. Workers

)" << workerCapabilities << R"(

## 4. Task Workflow

Most tasks can be broken down into:

| Phase | Who | Purpose |
|-------|-----|---------|
| Research | Workers (parallel) | Investigate codebase, find files, understand problem |
| Synthesis | You (coordinator) | Read findings, craft implementation specs |
| Implementation | Workers | Make targeted changes per spec |
| Verification | Workers | Test changes work, prove correctness |

### Concurrency
**Parallelism is your superpower.** Launch independent workers concurrently whenever possible. Read-only tasks run in parallel freely. Write-heavy tasks should be one at a time per file set.

### Verification
Verification means proving the code works:
- Run tests with the feature enabled
- Run typechecks and investigate errors
- Test independently ? prove the change works

### Handling Worker Failures
When a worker reports failure:
- Continue the same worker with SendMessage ? it has full error context
- If correction fails, try a different approach or report to the user

### Stopping Workers
Use TaskStop when a worker is going in the wrong direction. Stopped workers can be continued with SendMessage.

## 5. Writing Worker Prompts

Give workers specific, actionable prompts:
- Include file paths and expected outcomes
- Specify success criteria
- Ask for summaries of findings
- For implementation: include exact specifications

## 6. Current Environment
)";

  if (!config.scratchpadDir.empty()) {
    prompt << "\nScratchpad directory: " << config.scratchpadDir << "\n";
    prompt << "Workers can read/write here without permission prompts.\n";
  }

  if (!config.mcpServerNames.empty()) {
    prompt << "\nConnected MCP servers: ";
    for (size_t i = 0; i < config.mcpServerNames.size(); ++i) {
      if (i > 0) prompt << ", ";
      prompt << config.mcpServerNames[i];
    }
    prompt << "\n";
  }

  return prompt.str();
}

std::map<std::string, std::string> BuildCoordinatorUserContext(
    const CoordinatorConfig& config) {
  std::map<std::string, std::string> context;

  if (!IsCoordinatorMode()) return context;

  // Build worker tools list
  auto workerTools = GetWorkerTools(config.simpleMode);
  std::ostringstream toolsList;
  for (size_t i = 0; i < workerTools.size(); ++i) {
    if (i > 0) toolsList << ", ";
    toolsList << workerTools[i];
  }

  std::ostringstream content;
  content << "Workers spawned via the Agent tool have access to these tools: "
          << toolsList.str();

  if (!config.mcpServerNames.empty()) {
    content << "\n\nWorkers also have access to MCP tools from MCP servers: ";
    for (size_t i = 0; i < config.mcpServerNames.size(); ++i) {
      if (i > 0) content << ", ";
      content << config.mcpServerNames[i];
    }
  }

  if (!config.scratchpadDir.empty()) {
    content << "\n\nScratchpad directory: " << config.scratchpadDir << "\n"
            << "Workers can read and write here without permission prompts. "
            << "Use this for durable cross-worker knowledge.";
  }

  context["workerToolsContext"] = content.str();
  return context;
}

// ============================================================================
// Task notification
// ============================================================================

std::string BuildTaskNotification(const std::string& taskId,
                                   const std::string& status,
                                   const std::string& summary,
                                   const std::string& result,
                                   int totalTokens,
                                   int toolUses,
                                   long long durationMs) {
  std::ostringstream xml;
  xml << "<task-notification>\n";
  xml << "<task-id>" << taskId << "</task-id>\n";
  xml << "<status>" << status << "</status>\n";
  if (!summary.empty()) {
    xml << "<summary>" << summary << "</summary>\n";
  }
  if (!result.empty()) {
    xml << "<result>" << result << "</result>\n";
  }
  xml << "<usage>\n";
  xml << "  <total_tokens>" << totalTokens << "</total_tokens>\n";
  xml << "  <tool_uses>" << toolUses << "</tool_uses>\n";
  xml << "  <duration_ms>" << durationMs << "</duration_ms>\n";
  xml << "</usage>\n";
  xml << "</task-notification>";
  return xml.str();
}

TaskNotification ParseTaskNotification(const std::string& xml) {
  TaskNotification result;

  auto extractTag = [&](const std::string& tag) -> std::string {
    std::string open = "<" + tag + ">";
    std::string close = "</" + tag + ">";
    size_t start = xml.find(open);
    if (start == std::string::npos) return "";
    start += open.size();
    size_t end = xml.find(close, start);
    if (end == std::string::npos) return "";
    return xml.substr(start, end - start);
  };

  result.taskId = extractTag("task-id");
  result.status = extractTag("status");
  result.summary = extractTag("summary");
  result.result = extractTag("result");

  std::string tokensStr = extractTag("total_tokens");
  if (!tokensStr.empty()) result.totalTokens = std::atoi(tokensStr.c_str());

  std::string usesStr = extractTag("tool_uses");
  if (!usesStr.empty()) result.toolUses = std::atoi(usesStr.c_str());

  std::string durStr = extractTag("duration_ms");
  if (!durStr.empty()) result.durationMs = std::atoll(durStr.c_str());

  return result;
}

const char* CoordinationPhaseToString(CoordinationPhase phase) {
  switch (phase) {
    case CoordinationPhase::Research: return "Research";
    case CoordinationPhase::Synthesis: return "Synthesis";
    case CoordinationPhase::Implementation: return "Implementation";
    case CoordinationPhase::Verification: return "Verification";
  }
  return "Unknown";
}

}  // namespace coordinator
}  // namespace agent
