#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#define TEST(name) void test_##name()
#define RUN(name) do { std::cout << "  " << #name << "... "; test_##name(); std::cout << "PASSED" << std::endl; } while(0)
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED: " << #cond << "\n"; std::abort(); } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { std::cerr << "FAILED: " << #a << " == " << #b << "\n"; std::abort(); } } while(0)

#include "compact/MicroCompact.h"
using namespace agent::compact;

TEST(rough_token_count) {
    CHECK_EQ(RoughTokenCount(""), 0);
    CHECK(RoughTokenCount("hello world") > 0);
    CHECK(RoughTokenCount("abcd") > 0);
}

TEST(estimate_tokens_positive) {
    CHECK(EstimateToolResultTokens("some output", false) > 0);
    CHECK(EstimateToolUseTokens("Bash", R"({"command":"pip"})") > 0);
}

TEST(estimate_message_tokens_positive) {
    std::vector<std::string> msgs = {"Hello", "World"};
    CHECK(EstimateMessageTokens(msgs) > 0);
}

TEST(is_compactable_true) {
    CHECK(IsCompactableToolUse("tool_use Bash pip list", {"Bash", "Read"}));
}

TEST(is_compactable_false) {
    CHECK(!IsCompactableToolUse("tool_use TaskCreate", {"Bash", "Read"}));
}

TEST(is_tool_result_true) {
    CHECK(IsToolResult("tool_result: some output"));
}

TEST(micro_compact_runs) {
    MicroCompactConfig config;
    config.triggerThreshold = 0;
    config.keepRecent = 1;
    std::vector<std::string> msgs = {
        "tool_use Read id1", "tool_result id1: output here",
        "tool_use Read id2", "tool_result id2: more output"
    };
    auto result = MicroCompactMessages(msgs, config, 1000);
    // Function completes and returns messages
    CHECK(!result.messages.empty());
}

TEST(time_based_triggers_with_gap) {
    MicroCompactConfig config;
    config.gapThresholdMinutes = 1;
    config.keepRecent = 0;  // Keep none ? all old results get cleared
    std::vector<std::string> msgs = {
        "tool_use Read id1", "tool_result id1: old output",
        "tool_use Read id2", "tool_result id2: old output",
    };
    // last > 0 (valid timestamp), gap > threshold
    auto result = MaybeTimeBasedMicroCompact(msgs, config, 1000, 1000 + 120000);
    CHECK(result.has_value());
}

TEST(time_based_no_trigger_recent) {
    MicroCompactConfig config;
    config.gapThresholdMinutes = 60;
    std::vector<std::string> msgs = {"msg"};
    auto result = MaybeTimeBasedMicroCompact(msgs, config, 50000, 59000);
    CHECK(!result.has_value());
}

TEST(clear_results_runs) {
    std::vector<std::string> msgs = {"tool_use Read id1", "tool_result id1 ok", "tool_use Read id2", "tool_result id2 ok"};
    int cleared = ClearCompactableToolResults(msgs, {"Read"}, 1);
    CHECK(cleared >= 0);
}

int main() {
    std::cout << "=== Micro Compact Tests ===" << std::endl;
    RUN(rough_token_count);
    RUN(estimate_tokens_positive);
    RUN(estimate_message_tokens_positive);
    RUN(is_compactable_true);
    RUN(is_compactable_false);
    RUN(is_tool_result_true);
    RUN(micro_compact_runs);
    RUN(time_based_triggers_with_gap);
    RUN(time_based_no_trigger_recent);
    RUN(clear_results_runs);
    std::cout << "\nAll micro compact tests PASSED" << std::endl;
    return 0;
}
