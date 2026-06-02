// P0-03: Coordinator mode tests (aligned with local-ace coordinatorMode)

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

#define TEST(name) void test_##name()
#define RUN(name) do { std::cout << "  " << #name << "... "; test_##name(); std::cout << "PASSED" << std::endl; } while(0)
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { std::cerr << "FAILED: " << #a << " == " << #b << " at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)

#include "agents/CoordinatorMode.h"
using namespace agent::coordinator;

TEST(is_coordinator_disabled_by_default) {
    _putenv("CLAUDE_CODE_COORDINATOR_MODE=");
    CHECK(!IsCoordinatorMode());
}

TEST(is_coordinator_enabled_with_env) {
    _putenv("CLAUDE_CODE_COORDINATOR_MODE=1");
    CHECK(IsCoordinatorMode());
    _putenv("CLAUDE_CODE_COORDINATOR_MODE=");
}

TEST(match_session_mode_no_mismatch) {
    _putenv("CLAUDE_CODE_COORDINATOR_MODE=1");
    std::string warn = MatchSessionMode("coordinator");
    CHECK(warn.empty());
    _putenv("CLAUDE_CODE_COORDINATOR_MODE=");
}

TEST(match_session_mode_empty_input) {
    std::string warn = MatchSessionMode("");
    CHECK(warn.empty());
}

TEST(build_system_prompt_contains_key_sections) {
    CoordinatorConfig config;
    std::string prompt = BuildCoordinatorSystemPrompt(config);
    CHECK(prompt.find("coordinator") != std::string::npos);
    CHECK(prompt.find("Workers") != std::string::npos);
    CHECK(prompt.find("Research") != std::string::npos);
    CHECK(prompt.find("Verification") != std::string::npos);
}

TEST(build_system_prompt_with_mcp_servers) {
    CoordinatorConfig config;
    config.mcpServerNames = {"github", "filesystem"};
    std::string prompt = BuildCoordinatorSystemPrompt(config);
    CHECK(prompt.find("github") != std::string::npos);
}

TEST(get_worker_tools_simple) {
    auto tools = GetWorkerTools(true);
    CHECK(tools.size() >= 3);
    CHECK(std::find(tools.begin(), tools.end(), "Bash") != tools.end());
}

TEST(get_worker_tools_standard) {
    auto tools = GetWorkerTools(false);
    CHECK(tools.size() > 3);
    CHECK(std::find(tools.begin(), tools.end(), "TaskCreate") != tools.end());
}

TEST(internal_worker_tools) {
    auto tools = GetInternalWorkerTools();
    CHECK(std::find(tools.begin(), tools.end(), "TeamCreate") != tools.end());
    CHECK(std::find(tools.begin(), tools.end(), "SendMessage") != tools.end());
}

TEST(build_task_notification_completed) {
    std::string xml = BuildTaskNotification("agent-123", "completed", "Done", "Result text", 500, 10, 3000);
    CHECK(xml.find("<task-id>agent-123</task-id>") != std::string::npos);
    CHECK(xml.find("<status>completed</status>") != std::string::npos);
    CHECK(xml.find("<total_tokens>500</total_tokens>") != std::string::npos);
}

TEST(parse_task_notification) {
    std::string xml = BuildTaskNotification("agent-456", "failed", "Error occurred", "Stack trace", 200, 5, 1500);
    auto parsed = ParseTaskNotification(xml);
    CHECK_EQ(parsed.taskId, "agent-456");
    CHECK_EQ(parsed.status, "failed");
    CHECK_EQ(parsed.totalTokens, 200);
    CHECK_EQ(parsed.toolUses, 5);
}

TEST(build_user_context_with_scratchpad) {
    CoordinatorConfig config;
    _putenv("CLAUDE_CODE_COORDINATOR_MODE=1");
    config.scratchpadDir = "/tmp/scratch";
    auto ctx = BuildCoordinatorUserContext(config);
    CHECK(ctx.find("workerToolsContext") != ctx.end());
    CHECK(ctx["workerToolsContext"].find("/tmp/scratch") != std::string::npos);
    _putenv("CLAUDE_CODE_COORDINATOR_MODE=");
}

TEST(coordination_phase_to_string) {
    CHECK_EQ(std::string(CoordinationPhaseToString(CoordinationPhase::Research)), "Research");
    CHECK_EQ(std::string(CoordinationPhaseToString(CoordinationPhase::Implementation)), "Implementation");
}

int main() {
    std::cout << "=== Coordinator Mode Tests ===" << std::endl;
    RUN(is_coordinator_disabled_by_default);
    RUN(is_coordinator_enabled_with_env);
    RUN(match_session_mode_no_mismatch);
    RUN(match_session_mode_empty_input);
    RUN(build_system_prompt_contains_key_sections);
    RUN(build_system_prompt_with_mcp_servers);
    RUN(get_worker_tools_simple);
    RUN(get_worker_tools_standard);
    RUN(internal_worker_tools);
    RUN(build_task_notification_completed);
    RUN(parse_task_notification);
    RUN(build_user_context_with_scratchpad);
    RUN(coordination_phase_to_string);
    std::cout << "\nAll coordinator mode tests PASSED" << std::endl;
    return 0;
}
