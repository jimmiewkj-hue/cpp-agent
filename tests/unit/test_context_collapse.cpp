// P0-03: Context collapse + grouping + cleanup tests

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#define TEST(name) void test_##name()
#define RUN(name) do { std::cout << "  " << #name << "... "; test_##name(); std::cout << "PASSED" << std::endl; } while(0)
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { std::cerr << "FAILED: " << #a << " == " << #b << " at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)

#include "compact/ContextCollapse.h"
using namespace agent::compact;

TEST(should_collapse_when_over_limit) {
    ContextCollapser collapser;
    CHECK(collapser.ShouldCollapse(300000));
}

TEST(should_not_collapse_under_limit) {
    ContextCollapser collapser;
    CHECK(!collapser.ShouldCollapse(1000));
}

TEST(collapse_preserves_recent) {
    ContextCollapser collapser;
    CollapseConfig cfg;
    cfg.keepRecentMessages = 3;
    cfg.maxTokens = 500;
    collapser.SetConfig(cfg);

    std::vector<std::string> msgs = {
        "old message 1", "old message 2", "old message 3",
        "recent 1", "recent 2", "recent 3"
    };
    std::vector<int> tokens = {100, 100, 100, 100, 100, 100};

    auto result = collapser.Collapse(msgs, tokens);
    CHECK(result.wasCollapsed);
    CHECK(result.messagesRemoved > 0);
}

TEST(collapse_no_collapse_needed) {
    ContextCollapser collapser;
    CollapseConfig cfg;
    cfg.maxTokens = 10000;
    collapser.SetConfig(cfg);

    std::vector<std::string> msgs = {"msg1", "msg2"};
    std::vector<int> tokens = {10, 10};
    auto result = collapser.Collapse(msgs, tokens);
    CHECK(!result.wasCollapsed);
}

TEST(make_collapse_boundary) {
    std::string boundary = ContextCollapser::MakeCollapseBoundary(5, 2000);
    CHECK(boundary.find("5") != std::string::npos);
    CHECK(boundary.find("2000") != std::string::npos);
}

TEST(message_grouper_basic) {
    MessageGrouper grouper;
    std::vector<std::string> msgs = {"user msg", "assistant reply", "tool_use Bash", "tool_result ok"};
    std::vector<int> tokens = {10, 20, 30, 40};
    auto groups = grouper.GroupMessages(msgs, tokens);
    CHECK(groups.size() >= 3);
}

TEST(message_grouper_complete_tool_chain) {
    MessageGrouper grouper;
    std::vector<std::string> msgs = {"tool_use Bash pip list", "tool_result packages..."};
    std::vector<int> tokens = {30, 40};
    auto groups = grouper.GroupMessages(msgs, tokens);
    CHECK(groups.size() >= 1);
}

TEST(post_compact_cleanup_empty) {
    PostCompactCleanup cleaner;
    std::vector<std::string> msgs = {"", "  ", "\n", "real message"};
    auto result = cleaner.Cleanup(msgs);
    CHECK(result.emptyMessagesRemoved >= 2);
}

TEST(post_compact_cleanup_duplicates) {
    PostCompactCleanup cleaner;
    std::vector<std::string> msgs = {
        "[Collapse] archived",
        "[Collapse] archived",
        "real message"
    };
    auto result = cleaner.Cleanup(msgs);
    CHECK(result.duplicateBoundariesRemoved >= 1);
}

TEST(merge_compactable_groups) {
    MessageGrouper grouper;
    MessageGroup g1, g2, g3;
    g1.compactable = true; g1.totalTokens = 50; g1.messageIndices = {0};
    g2.compactable = true; g2.totalTokens = 60; g2.messageIndices = {1};
    g3.compactable = false; g3.totalTokens = 30; g3.messageIndices = {2};
    
    auto merged = grouper.MergeCompactableGroups({g1, g2, g3}, 200);
    CHECK(merged.size() == 2);  // g1+g2 merged, g3 separate
}

int main() {
    std::cout << "=== Context Collapse Tests ===" << std::endl;
    RUN(should_collapse_when_over_limit);
    RUN(should_not_collapse_under_limit);
    RUN(collapse_preserves_recent);
    RUN(collapse_no_collapse_needed);
    RUN(make_collapse_boundary);
    RUN(message_grouper_basic);
    RUN(message_grouper_complete_tool_chain);
    RUN(post_compact_cleanup_empty);
    RUN(post_compact_cleanup_duplicates);
    RUN(merge_compactable_groups);
    std::cout << "\nAll context collapse tests PASSED" << std::endl;
    return 0;
}
