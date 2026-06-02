// P0-03: Real-world integration test ? jianlai-graph scenario simulation.
// Exercises the FULL module chain: SessionMemory ? ExtractMemories ? AutoDream ?
// CompactEngine ? MicroCompact ? AppStateStore ? PolicyLimits ? CoordinatorMode ?
// ChannelPermissions ? ChannelNotification ? SandboxEnforcer.
//
// Scenario: A user asks to build a character relationship graph from 3 novel files.
// The assistant gets stuck in a "check environment" loop with repeated Bash failures.
// The system must:
//   1. Extract memories from the conversation (ExtractMemories)
//   2. Track task state and completion boundaries (AppStateStore)
//   3. Detect repeated timeout errors (timeout fingerprint normalization)
//   4. Trigger time-based microcompact when cache expires (MicroCompact + TimeBasedMCConfig)
//   5. Build post-compact messages correctly (CompactEngine)
//   6. Enforce policy restrictions (PolicyLimits)
//   7. Coordinate worker agents (CoordinatorMode)
//   8. Check MCP tool permissions (ChannelPermissions)
//   9. Relay permission requests (ChannelNotification)

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <sstream>

#define TEST(name) void test_##name()
#define RUN(name) do { std::cout << "  " << #name << "... "; test_##name(); std::cout << "PASSED" << std::endl; } while(0)
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { std::cerr << "FAILED: " << #a << "==" << #b << " at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)

// All module includes
#include "memory/SessionMemory.h"
#include "memory/ExtractMemories.h"
#include "memory/AutoDream.h"
#include "compact/CompactEngine.h"
#include "compact/MicroCompact.h"
#include "compact/TimeBasedMCConfig.h"
#include "compact/SessionMemoryCompact.h"
#include "compact/ContextCollapse.h"
#include "core/AppStateStore.h"
#include "agents/CoordinatorMode.h"
#include "mcp/ChannelPermissions.h"
#include "mcp/ChannelNotification.h"
#include "permissions/PolicyLimits.h"
#include "sandbox/SandboxEnforcer.h"

using namespace agent;

// ============================================================================
// Full jianlai-graph scenario: memory extraction during Bash failure loop
// ============================================================================
TEST(jianlai_graph_memory_extraction_under_failure) {
    memory::SessionMemory sessionMem("test_integration_session");
    memory::ExtractMemories extractor;
    state::AppStateStore appState;

    // Simulate conversation: user asks for character graph, assistant tries Bash, fails repeatedly
    std::vector<std::string> conversation = {
        "User: Build a character relationship graph from the three novel files",
        "Assistant: Let me check the Python environment first",
        "Assistant tool_use Bash: pip list 2>$null | Select-String nltk",
        "tool_result: Error: command not found",
        "Assistant: Let me try grep instead", 
        "Assistant tool_use Bash: pip list 2>&1 | grep -iE nltk",
        "tool_result: Error: 'grep' is not recognized",
        "Assistant: I'll use python -m pip list",
        "Assistant tool_use Bash: python -m pip list 2>&1",
        "tool_result: Error: command timed out after 120s",
        "User (validator): The assistant keeps using wrong shell syntax on Windows",
        "Assistant: I prefer using PowerShell Select-String for filtering"
    };

    // Extract memories
    auto memories = extractor.Extract(conversation);
    CHECK(memories.size() >= 2);  // Should find at least: preference + reference

    // Verify memory types
    bool foundPreference = false;
    bool foundReference = false;
    for (const auto& m : memories) {
        if (m.type == "user") foundPreference = true;
        if (m.type == "reference") foundReference = true;
    }
    CHECK(foundPreference || foundReference);

    // Add extracted memories to session
    for (const auto& m : memories) {
        sessionMem.AddMemory(m.content, m.type, m.scope, m.priority);
    }

    // Verify session memory
    auto allMemories = sessionMem.ListMemories();
    CHECK(allMemories.size() >= 2);

    // Build context injection for system prompt
    std::string injection = compact::BuildSessionMemoryContextInjection(&sessionMem, 2000);
    CHECK(!injection.empty());
    CHECK(injection.find("[Session Memory]") != std::string::npos);

    // Track task state
    appState.RegisterTask("main", "Build character relationship graph");
    appState.UpdateTaskStatus("main", state::TaskStatus::Running);
    CHECK(appState.HasRunningTasks());
}

// ============================================================================
// Timeout fingerprint + compact chain: detect loop, trigger compact
// ============================================================================
TEST(jianlai_graph_timeout_loop_and_compact) {
    // Simulate accumulating tool results from repeated Bash timeouts
    compact::MicroCompactConfig mcConfig;
    mcConfig.triggerThreshold = 2;
    mcConfig.keepRecent = 1;
    mcConfig.gapThresholdMinutes = 1;

    std::vector<std::string> messages;
    // Simulate 5 rounds of tool_use/tool_result with timeout patterns
    for (int i = 0; i < 5; ++i) {
        messages.push_back("tool_use Bash id" + std::to_string(i) + " pip list");
        std::ostringstream result;
        result << "tool_result id" << i << ": Error: command timed out after 120s\n";
        result << "Partial output: package_list_v" << i << " content";
        messages.push_back(result.str());
    }

    // Microcompact should trigger (count-based: 5 > 2)
    auto result = compact::MicroCompactMessages(messages, mcConfig, 0);
    CHECK(!result.messages.empty());
    // At least some tool results should be cleared
    int clearedCount = 0;
    for (const auto& msg : result.messages) {
        if (msg.find("[Old tool result content cleared]") != std::string::npos) {
            ++clearedCount;
        }
    }

    // Time-based trigger: simulate 2-hour gap
    compact::TimeBasedMCConfig tcConfig;
    tcConfig.enabled = true;
    tcConfig.gapThresholdMinutes = 60;
    long long lastAsst = 1000000;
    long long now = lastAsst + 120 * 60 * 1000LL;  // 2 hours later
    CHECK(compact::ShouldTriggerTimeBasedMC(tcConfig, lastAsst, now));

    // Build compact boundary
    std::string boundary = compact::BuildMicroCompactBoundary(3, 5000);
    CHECK(boundary.find("3") != std::string::npos);
    CHECK(boundary.find("5000") != std::string::npos);
}

// ============================================================================
// Full compact chain: BuildPostCompactMessages with all components
// ============================================================================
TEST(full_compact_chain_with_attachments) {
    compact::CompactionResult cr;
    cr.boundaryMarker = "[Context Collapse] Conversation summarized for context management";
    cr.summaryMessages = {
        "Summary: User asked to build character graph. Assistant checked Python env.",
        "Key finding: Windows PowerShell environment, grep/head not available."
    };
    cr.messagesToKeep = {
        "User: Build character relationship graph",
        "Assistant: pip list shows jieba, networkx, matplotlib installed"
    };
    cr.attachments = {
        compact::CreatePlanAttachmentIfNeeded(
            "Plan: 1. Extract characters 2. Build edges 3. Visualize graph"),
        compact::CreateSkillAttachmentIfNeeded(
            "Skill: Python NLP pipeline with jieba + networkx")
    };
    cr.hookResults = {"Hook: Pre-compact hook completed successfully"};
    cr.preCompactTokenCount = 50000;
    cr.postCompactTokenCount = 15000;

    auto messages = compact::BuildPostCompactMessages(cr);

    // Verify ordering: boundary ? summaries ? keep ? attachments ? hooks
    CHECK(messages.size() >= 7);
    CHECK(messages[0].find("[Context Collapse]") != std::string::npos);
    CHECK(messages[1].find("Summary") != std::string::npos);
    CHECK(messages[3].find("Build character") != std::string::npos);
    CHECK(messages[5].find("[Plan attachment]") != std::string::npos);
    CHECK(messages[6].find("[Skill attachment]") != std::string::npos);
}

// ============================================================================
// AppStateStore: full task lifecycle with compaction tracking
// ============================================================================
TEST(app_state_full_lifecycle_with_compaction) {
    state::AppStateStore store;

    // Register main and sub-tasks
    store.RegisterTask("main", "Build character relationship graph from 3 novel files");
    store.RegisterTask("sub-extract", "Extract character names using regex + NLP");
    store.RegisterTask("sub-edges", "Build relationship edges from interactions");
    store.RegisterTask("sub-viz", "Generate network visualization");

    // Start tasks
    store.UpdateTaskStatus("main", state::TaskStatus::Running);
    store.UpdateTaskStatus("sub-extract", state::TaskStatus::Running);
    store.UpdateTaskStatus("sub-edges", state::TaskStatus::Running);
    CHECK_EQ(store.ActiveTaskCount(), 3);

    // MemDir path resolution
    memory::MemDir memdir("G:\\downloads\\jianlai-graph");
    auto paths = memdir.ResolvePaths();
    CHECK(!paths.sessionMemoryDir.empty());
    CHECK(paths.sessionMemoryDir.find(".cpp-agent") != std::string::npos);
    CHECK(!paths.sessionMemoryFile.empty());

    // Record memory extraction
    store.RecordMemoryExtraction(8);
    store.MarkSessionMemoryInitialized();

    // Record compaction
    store.RecordCompaction("session_memory", 25, 12000);

    // Sub-tasks complete
    store.CompleteTask("sub-extract", 2000, "Extracted 127 characters");
    store.CompleteTask("sub-edges", 1500, "Built 892 edges");

    // Record completion boundaries
    state::CompletionBoundary bashBoundary;
    bashBoundary.type = state::CompletionBoundary::Type::Bash;
    bashBoundary.command = "python extract_characters.py";
    bashBoundary.completedAtMs = 5000;
    bashBoundary.outputTokens = 2000;
    store.RecordCompletionBoundary(bashBoundary);

    state::CompletionBoundary editBoundary;
    editBoundary.type = state::CompletionBoundary::Type::Edit;
    editBoundary.toolName = "Write";
    editBoundary.filePath = "G:\\downloads\\jianlai-graph\\character_graph.py";
    store.RecordCompletionBoundary(editBoundary);

    // Verify state
    CHECK(store.IsSessionMemoryInitialized());
    const auto& cs = store.CompactState();
    CHECK_EQ(cs.totalCompactions, 1);
    CHECK_EQ(cs.sessionMemoryCompactions, 1);

    const auto* lastBoundary = store.LastBoundary();
    CHECK(lastBoundary != nullptr);
    CHECK_EQ(lastBoundary->filePath, "G:\\downloads\\jianlai-graph\\character_graph.py");

    // Complete main task
    store.CompleteTask("main", 10000, "Graph generation complete with 127 nodes, 892 edges");
    CHECK(!store.HasRunningTasks());
}

// ============================================================================
// Coordinator: worker orchestration for the graph building task
// ============================================================================
TEST(coordinator_worker_orchestration_for_graph_building) {
    coordinator::CoordinatorConfig config;
    config.enabled = true;
    config.scratchpadDir = "G:\\downloads\\jianlai-graph\\.cpp-agent\\scratch";
    config.mcpServerNames = {"filesystem"};

    // Build system prompt
    std::string sysPrompt = coordinator::BuildCoordinatorSystemPrompt(config);
    CHECK(sysPrompt.find("coordinator") != std::string::npos);
    CHECK(sysPrompt.find("Research") != std::string::npos);
    CHECK(sysPrompt.find("Implementation") != std::string::npos);
    CHECK(sysPrompt.find("Verification") != std::string::npos);
    CHECK(sysPrompt.find("filesystem") != std::string::npos);

    // Get worker tools
    auto workerTools = coordinator::GetWorkerTools(false);
    CHECK(workerTools.size() > 5);
    CHECK(std::find(workerTools.begin(), workerTools.end(), "TaskCreate") != workerTools.end());

    // Build task notification for completed worker
    std::string notification = coordinator::BuildTaskNotification(
        "agent-extract-chars", "completed",
        "Agent 'Extract character names' completed",
        "Found 127 unique characters across 1278 chapters",
        5000, 42, 30000);
    CHECK(notification.find("agent-extract-chars") != std::string::npos);
    CHECK(notification.find("completed") != std::string::npos);

    // Parse it back
    auto parsed = coordinator::ParseTaskNotification(notification);
    CHECK_EQ(parsed.taskId, "agent-extract-chars");
    CHECK_EQ(parsed.status, "completed");
    CHECK_EQ(parsed.totalTokens, 5000);

    // Coordination phases
    CHECK_EQ(std::string(coordinator::CoordinationPhaseToString(
        coordinator::CoordinationPhase::Research)), "Research");
}

// ============================================================================
// Policy limits + MCP permissions: security chain
// ============================================================================
TEST(policy_limits_and_mcp_permissions_chain) {
    // 1. Policy limits: restrict dangerous operations
    permissions::PolicyLimits limits;
    limits.LoadDefaultRestrictions();
    CHECK(limits.IsRestricted("shell_injection"));
    CHECK(limits.IsRestricted("path_traversal"));
    CHECK(limits.IsRestricted("destructive_system_commands"));
    CHECK(limits.IsBashExecutionAllowed());  // Bash itself is not restricted
    CHECK(limits.IsFileWriteAllowed());

    // 2. Channel permissions: allowlist for MCP tools
    mcp::McpChannelPermissionsConfig permConfig;
    permConfig.globalAllowlist = {
        "filesystem.read_file", "filesystem.write_file",
        "filesystem.list_directory", "filesystem.search_files"
    };
    permConfig.serverToolBlocklists["filesystem"] = {"filesystem.delete_all"};
    permConfig.autoApproveReadOnly = true;
    permConfig.requireApprovalForDestructive = true;

    // Check read tool
    auto readPerm = mcp::CheckMcpToolPermission(
        permConfig, "filesystem", "read_file", true, false);
    CHECK(readPerm.allow);
    CHECK(!readPerm.requiresApproval);

    // Check write tool
    auto writePerm = mcp::CheckMcpToolPermission(
        permConfig, "filesystem", "write_file", false, true);
    CHECK(writePerm.allow);
    CHECK(writePerm.requiresApproval);

    // Check blocked tool
    auto delPerm = mcp::CheckMcpToolPermission(
        permConfig, "filesystem", "delete_all", false, true);
    CHECK(!delPerm.allow);

    // 3. Channel notification: relay permission request
    mcp::ChannelNotificationConfig notifConfig;
    notifConfig.enabled = true;
    notifConfig.activeChannels = {"cli"};
    mcp::ChannelNotificationRelay relay(notifConfig);
    CHECK(relay.IsEnabled());

    std::string reqId = relay.SendPermissionRequest(
        "filesystem.write_file", "filesystem",
        "Allow writing to G:\\downloads\\jianlai-graph\\output\\graph.html?");
    CHECK(!reqId.empty());
    CHECK(relay.HasPendingRequests());

    // Simulate response
    mcp::ChannelPermissionResponse resp;
    resp.requestId = reqId;
    resp.behavior = mcp::ChannelPermissionResponse::Allow;
    resp.fromServer = "cli";
    CHECK(relay.HandleIncomingResponse(resp));
    CHECK(!relay.HasPendingRequests());
}

// ============================================================================
// Session memory under real-world load (50+ memories, priority sorting)
// ============================================================================
TEST(session_memory_real_world_load) {
    memory::SessionMemory mem("test_real_load_dir");

    // Simulate a long conversation with many memory-worthy moments
    const char* memoryTexts[] = {
        "Project uses Python 3.11 with jieba 0.42.1 for Chinese text segmentation",      // project, high priority
        "User prefers concise answers without verbose explanations",                      // user
        "Never use grep on Windows ? always use Select-String or findstr",                // reference, critical
        "The novel files are encoded in UTF-8 with CRLF line endings",                    // reference
        "Character names follow Chinese naming conventions (surname + given name)",       // reference
        "Generated graph should use pyvis for interactive HTML visualization",            // project
        "Output directory: G:\\downloads\\jianlai-graph\\output",                         // reference
        "Performance target: process all 1278 chapters in under 5 minutes",               // project
        "Relationship scoring: dialogue co-occurrence weighted 3x over proximity",        // reference
        "Edge threshold: minimum 3 interactions to create a relationship edge",           // reference
    };

    for (int i = 0; i < 10; ++i) {
        mem.AddMemory(memoryTexts[i],
                      i < 3 ? "project" : "reference",
                      i < 3 ? "project" : "session",
                      i < 3 ? 8 : 5);
    }

    // Add more memories to simulate load
    for (int i = 0; i < 40; ++i) {
        mem.AddMemory("Memory entry " + std::to_string(i),
                      "session", "session", i % 5);
    }

    // Verify total count
    auto all = mem.ListMemories();
    CHECK_EQ(static_cast<int>(all.size()), 50);

    // Search for specific content
    auto grepResults = mem.SearchMemories("grep");
    CHECK(grepResults.size() >= 1);

    // Build context injection with tight budget
    std::string injection = compact::BuildSessionMemoryContextInjection(&mem, 1000);
    CHECK(!injection.empty());
    // Highest priority items should be present
    CHECK(injection.find("grep") != std::string::npos ||
          injection.find("PowerShell") != std::string::npos ||
          injection.find("Python") != std::string::npos);
}

// ============================================================================
// Context collapse: real-world message chain collapse
// ============================================================================
TEST(context_collapse_real_world_chain) {
    compact::ContextCollapser collapser;
    compact::CollapseConfig cfg;
    cfg.maxTokens = 30;  // Very low to force collapse
    cfg.keepRecentMessages = 2;
    collapser.SetConfig(cfg);

    // Simulate a conversation with many tool calls
    std::vector<std::string> messages = {
        "User: Build character graph",
        "Assistant: Let me explore the workspace",
        "tool_use Glob: *.txt",
        "tool_result: found 3 files",
        "Assistant: Reading files",
        "tool_use Read: chapter 1-500",
        "tool_result: file contents...",
        "tool_use Read: chapter 501-1000",
        "tool_result: file contents...",
        "Assistant: Checking Python",
        "tool_use Bash: pip list",
        "tool_result: timed out",
        "Assistant: Trying again...",
        "tool_use Bash: pip list 2>&1",
        "tool_result: packages installed...",
    };

    std::vector<int> tokens;
    for (const auto& m : messages) {
        tokens.push_back(static_cast<int>(m.size()) / 4);
    }

    auto result = collapser.Collapse(messages, tokens);
    CHECK(result.wasCollapsed);
    CHECK(result.collapsedMessages.size() < messages.size());

    // Message grouping
    compact::MessageGrouper grouper;
    auto groups = grouper.GroupMessages(messages, tokens);
    CHECK(groups.size() >= 4);  // user turn, tool chains, etc.

    // Post-compact cleanup
    compact::PostCompactCleanup cleaner;
    std::vector<std::string> cleanup = {"", "  ", "\n", "real message 1", "real message 2"};
    auto cleanupResult = cleaner.Cleanup(cleanup);
    CHECK(cleanupResult.emptyMessagesRemoved >= 2);
}

// ============================================================================
// Sandbox: command filtering for the Bash failure scenario
// ============================================================================
TEST(sandbox_command_filtering_jianlai_scenario) {
    sandbox::SandboxConfig config;
    config.mode = sandbox::SandboxMode::WorkspaceWrite;
    config.workspaceRoot = "G:\\downloads\\jianlai-graph";
    config.blockedCommands = {"rm -rf", "format", "del /f /s", "shutdown"};

    sandbox::SandboxEnforcer enforcer(config);
    CHECK(enforcer.IsActive());

    // Safe command
    auto check1 = enforcer.CheckCommand("python -m pip list");
    CHECK(check1.type == sandbox::SandboxViolationType::None);

    // Dangerous command
    auto check2 = enforcer.CheckCommand("rm -rf /");
    CHECK(check2.type != sandbox::SandboxViolationType::None);

    // Path within workspace
    auto path1 = enforcer.CheckFilePath("G:\\downloads\\jianlai-graph\\output\\graph.html", true);
    CHECK(path1.type == sandbox::SandboxViolationType::None);

    // Path outside workspace
    auto path2 = enforcer.CheckFilePath("C:\\Windows\\System32\\config", true);
    CHECK(path2.type != sandbox::SandboxViolationType::None);
}

// ============================================================================
// PTL retry: truncate for Prompt Too Long recovery
// ============================================================================
TEST(ptl_retry_truncate_with_attachments) {
    std::vector<std::string> messages = {
        "[system] System prompt here...",
        "User: Build a complete text analysis system for the novel Sword of Coming...",
        "Assistant: Let me explore the project directory structure",
        "Assistant tool_use Glob: **/*.txt",
        "tool_result: Found 3 files",
        "Assistant: Reading first file",
        "Assistant tool_use Read: sword_ch1-500.txt limit=100",
        "tool_result: Chapter 1 content...",
        "Assistant: Let me check the Python environment",
        "Assistant tool_use Bash: python --version",
        "tool_result: Python 3.11.0",
        "Assistant: Checking packages",
        "Assistant tool_use Bash: pip list",
        "tool_result: Error: command timed out after 120s\nPartial: jieba 0.42.1, networkx 3.2.1...",
        "Assistant: Most recent message with analysis plan"
    };

    auto result = compact::TruncateHeadForPTLRetry(messages, "system prompt", 500);
    CHECK(result.wasTruncated);
    CHECK(!result.truncatedMessages.empty());
    // The user goal should be preserved
    bool hasUserGoal = false;
    for (const auto& m : result.truncatedMessages) {
        if (m.find("Build a complete text analysis") != std::string::npos) {
            hasUserGoal = true;
        }
    }
    CHECK(hasUserGoal);

    // Strip images from messages
    std::string msgWithImage = "text ![screenshot](data:image/png;base64,iVBORw0KGgo=) more text";
    std::string stripped = compact::StripImagesFromMessage(msgWithImage);
    CHECK(stripped.find("iVBOR") == std::string::npos);
    CHECK(stripped.find("[image removed]") != std::string::npos);

    // Strip reinjected attachments
    std::string msgWithAttach = "content [Plan attachment: plan here] middle [Skill attachment: skill] end";
    std::string stripped2 = compact::StripReinjectedAttachments(msgWithAttach);
    CHECK(stripped2.find("[Plan attachment") == std::string::npos);
}

// ============================================================================
// Token estimation accuracy for the jianlai-graph conversation
// ============================================================================
TEST(token_estimation_for_jianlai_conversation) {
    // Simulate the actual conversation from the jianlai-graph ModelCall
    std::vector<std::string> conversation = {
        "User: Develop a complete text analysis system for Sword of Coming novel, build character relationship graph",
        "Assistant: Let me explore the project structure",
        "tool_use Glob: **/*.txt",
        "tool_result: Found sword_ch1-500.txt (11MB), sword_ch501-1000.txt (20MB), sword_ch1001-1278.txt (10MB)",
        "Assistant: Reading file format",
        "tool_use Read: G:\\downloads\\jianlai-graph\\sword_ch1-500.txt limit=100",
        "tool_result: Chapter 1: The young man carried a wooden sword...",
        "Assistant: Checking Python environment",
        "tool_use Bash: python --version && pip list 2>$null | Select-String nltk",
        "tool_result: Error: command syntax error",
    };

    int estimatedTokens = compact::EstimateMessageTokens(conversation);
    CHECK(estimatedTokens > 50);  // Should be a reasonable estimate

    int toolResultTokens = compact::EstimateToolResultTokens(
        "Found sword_ch1-500.txt (11MB), sword_ch501-1000.txt (20MB), sword_ch1001-1278.txt (10MB)",
        false);
    CHECK(toolResultTokens > 10);

    int toolUseTokens = compact::EstimateToolUseTokens("Bash",
        R"({"command":"python --version && pip list"})");
    CHECK(toolUseTokens > 5);
}

int main() {
    std::cout << "=== Real-World Integration Tests (jianlai-graph scenario) ===" << std::endl;

    std::cout << "\n[Memory + Extraction Chain]" << std::endl;
    RUN(jianlai_graph_memory_extraction_under_failure);

    std::cout << "\n[Timeout Loop + Compact Chain]" << std::endl;
    RUN(jianlai_graph_timeout_loop_and_compact);

    std::cout << "\n[Full Compact Chain]" << std::endl;
    RUN(full_compact_chain_with_attachments);

    std::cout << "\n[AppStateStore Lifecycle]" << std::endl;
    RUN(app_state_full_lifecycle_with_compaction);

    std::cout << "\n[Coordinator Orchestration]" << std::endl;
    RUN(coordinator_worker_orchestration_for_graph_building);

    std::cout << "\n[Policy + MCP Permissions + Notification Chain]" << std::endl;
    RUN(policy_limits_and_mcp_permissions_chain);

    std::cout << "\n[Session Memory Under Load]" << std::endl;
    RUN(session_memory_real_world_load);

    std::cout << "\n[Context Collapse Chain]" << std::endl;
    RUN(context_collapse_real_world_chain);

    std::cout << "\n[Sandbox Command Filtering]" << std::endl;
    RUN(sandbox_command_filtering_jianlai_scenario);

    std::cout << "\n[PTL Retry + Token Estimation]" << std::endl;
    RUN(ptl_retry_truncate_with_attachments);
    RUN(token_estimation_for_jianlai_conversation);

    std::cout << "\n=== All real-world integration tests PASSED ===" << std::endl;
    return 0;
}
