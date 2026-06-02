#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#define TEST(name) void test_##name()
#define RUN(name) do { std::cout << "  " << #name << "... "; test_##name(); std::cout << "PASSED" << std::endl; } while(0)
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED: " << #cond << "\n"; std::abort(); } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { std::cerr << "FAILED: " << #a << " == " << #b << "\n"; std::abort(); } } while(0)

#include "compact/CompactEngine.h"
using namespace agent::compact;

TEST(build_post_compact_messages_order) {
    CompactionResult cr;
    cr.boundaryMarker = "BOUNDARY";
    cr.summaryMessages = {"SUMMARY1", "SUMMARY2"};
    cr.messagesToKeep = {"KEEP1"};
    cr.attachments = {"ATTACH1"};
    cr.hookResults = {"HOOK1"};
    auto msgs = BuildPostCompactMessages(cr);
    CHECK_EQ(static_cast<int>(msgs.size()), 6);
    CHECK_EQ(msgs[0], "BOUNDARY");
    CHECK_EQ(msgs[1], "SUMMARY1");
    CHECK_EQ(msgs[2], "SUMMARY2");
    CHECK_EQ(msgs[3], "KEEP1");
    CHECK_EQ(msgs[4], "ATTACH1");
    CHECK_EQ(msgs[5], "HOOK1");
}

TEST(build_post_compact_empty) {
    CompactionResult cr;
    auto msgs = BuildPostCompactMessages(cr);
    CHECK(msgs.empty());
}

TEST(annotate_boundary_empty_keep) {
    std::string boundary = "[Compact]";
    std::string result = AnnotateBoundaryWithPreservedSegment(boundary, "anchor-1", {});
    CHECK_EQ(result, boundary);
}

TEST(annotate_boundary_with_keep) {
    std::string boundary = "[Compact]";
    std::vector<std::string> keep = {"head-msg", "tail-msg"};
    std::string result = AnnotateBoundaryWithPreservedSegment(boundary, "anchor-1", keep);
    CHECK(result.find("head") != std::string::npos);
    CHECK(result.find("anchor") != std::string::npos);
    CHECK(result.find("tail") != std::string::npos);
}

TEST(merge_hook_instructions_both) {
    std::string result = MergeHookInstructions("user instruction", "hook instruction");
    CHECK(result.find("user instruction") != std::string::npos);
    CHECK(result.find("hook instruction") != std::string::npos);
    CHECK(result.find("\n\n") != std::string::npos);
}

TEST(merge_hook_instructions_user_only) {
    CHECK_EQ(MergeHookInstructions("user only", ""), "user only");
}

TEST(merge_hook_instructions_hook_only) {
    CHECK_EQ(MergeHookInstructions("", "hook only"), "hook only");
}

TEST(strip_images) {
    std::string msg = "text ![img](data:image/png;base64,ABC123==) more text";
    std::string result = StripImagesFromMessage(msg);
    CHECK(result.find("ABC123") == std::string::npos);
    CHECK(result.find("text") != std::string::npos);
    CHECK(result.find("[image removed]") != std::string::npos);
}

TEST(strip_reinjected_attachments) {
    std::string msg = "text [Plan attachment: my plan] middle [Skill attachment: skill] end";
    std::string result = StripReinjectedAttachments(msg);
    CHECK(result.find("[Plan attachment") == std::string::npos);
    CHECK(result.find("[Skill attachment") == std::string::npos);
    CHECK(result.find("text") != std::string::npos);
    CHECK(result.find("end") != std::string::npos);
}

TEST(truncate_head_for_ptl) {
    std::vector<std::string> msgs = {"sys", "user goal", "msg2", "msg3", "msg4", "msg5", "msg6", "recent"};
    auto result = TruncateHeadForPTLRetry(msgs, "system prompt", 500);
    CHECK(result.wasTruncated);
    CHECK(result.truncatedMessages.size() < msgs.size());
    // User goal should be preserved somewhere in the truncated messages
    bool foundUserGoal = false;
    for (const auto& m : result.truncatedMessages) {
        if (m == "user goal") { foundUserGoal = true; break; }
    }
    CHECK(foundUserGoal);
}

TEST(plan_attachment) {
    std::string plan = "This is a plan content";
    std::string result = CreatePlanAttachmentIfNeeded(plan);
    CHECK(result.find("[Plan attachment]") != std::string::npos);
    CHECK(result.find("plan content") != std::string::npos);
    CHECK(result.find("[/Plan attachment]") != std::string::npos);
}

TEST(plan_attachment_empty) {
    CHECK(CreatePlanAttachmentIfNeeded("").empty());
}

TEST(skill_attachment) {
    std::string skill = "Skill content here";
    std::string result = CreateSkillAttachmentIfNeeded(skill);
    CHECK(result.find("[Skill attachment]") != std::string::npos);
    CHECK(result.find("Skill content") != std::string::npos);
}

TEST(error_messages) {
    CHECK(!BuildNotEnoughMessagesError().empty());
    CHECK(!BuildPromptTooLongError().empty());
    CHECK(!BuildUserAbortError().empty());
    CHECK(!BuildIncompleteResponseError().empty());
}

int main() {
    std::cout << "=== Compact Engine Tests ===" << std::endl;
    RUN(build_post_compact_messages_order);
    RUN(build_post_compact_empty);
    RUN(annotate_boundary_empty_keep);
    RUN(annotate_boundary_with_keep);
    RUN(merge_hook_instructions_both);
    RUN(merge_hook_instructions_user_only);
    RUN(merge_hook_instructions_hook_only);
    RUN(strip_images);
    RUN(strip_reinjected_attachments);
    RUN(truncate_head_for_ptl);
    RUN(plan_attachment);
    RUN(plan_attachment_empty);
    RUN(skill_attachment);
    RUN(error_messages);
    std::cout << "\nAll compact engine tests PASSED" << std::endl;
    return 0;
}
