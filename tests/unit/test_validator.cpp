#include "core/LocalValidator.h"
#include "core/AgentTypes.h"
#include "core/StateTypes.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

static int failures = 0;

static void Check(bool condition, const char* label) {
  if (!condition) {
    std::cerr << "FAIL: " << label << std::endl;
    ++failures;
  }
}

void TestExtractOriginalUserGoal() {
  using agent::core::Message;
  using agent::core::MessageRole;
  using agent::core::ContentBlock;
  
  std::vector<Message> messages;
  
  // Add a user message
  Message userMsg;
  userMsg.role = MessageRole::User;
  userMsg.isMeta = false;
  userMsg.content.push_back(ContentBlock::MakeText("Build a text analysis system"));
  messages.push_back(userMsg);
  
  std::string goal = agent::core::ExtractOriginalUserGoal(messages);
  Check(goal == "Build a text analysis system", "ExtractOriginalUserGoal finds user text");
  
  // Skip meta messages
  Message metaMsg;
  metaMsg.role = MessageRole::User;
  metaMsg.isMeta = true;
  metaMsg.content.push_back(ContentBlock::MakeText("[Validator] retry"));
  messages.push_back(metaMsg);
  
  goal = agent::core::ExtractOriginalUserGoal(messages);
  Check(goal == "Build a text analysis system", "ExtractOriginalUserGoal skips meta messages");
}

void TestRequiresDirectTextResponse() {
  Check(agent::core::RequiresDirectTextResponse("just tell me the answer"), "just tell");
  Check(agent::core::RequiresDirectTextResponse("don't write any files please"), "dont write");
  Check(agent::core::RequiresDirectTextResponse("respond directly in chat"), "directly in chat");
  Check(!agent::core::RequiresDirectTextResponse("build a complete system"), "build system NOT direct");
  Check(!agent::core::RequiresDirectTextResponse(""), "empty string NOT direct");
}

void TestForbidsFileOperations() {
  Check(agent::core::ForbidsFileOperations("don't create files"), "dont create files");
  Check(agent::core::ForbidsFileOperations("read-only analysis"), "read-only");
  Check(agent::core::ForbidsFileOperations("without saving any files"), "without saving");
  Check(!agent::core::ForbidsFileOperations("create a text analysis system"), "create system NOT forbid");
}

void TestForbidsJavaScript() {
  Check(agent::core::ForbidsJavaScript("no javascript please"), "no javascript");
  Check(agent::core::ForbidsJavaScript("pure html only without js"), "without js");
  Check(!agent::core::ForbidsJavaScript("build a web app"), "web app NOT forbid js");
}

void TestForbidsMarkdown() {
  Check(agent::core::ForbidsMarkdown("plain text no markdown"), "no markdown");
  Check(agent::core::ForbidsMarkdown("please don't use markdown"), "dont use markdown");
  Check(!agent::core::ForbidsMarkdown("write a report"), "write report NOT forbid md");
}

void TestBuildCompactConstraints() {
  auto constraints = agent::core::BuildCompactConstraints("just tell me the answer");
  bool hasDirectText = false;
  for (const auto& c : constraints) {
    if (c == "DIRECT_TEXT_ONLY") hasDirectText = true;
  }
  Check(hasDirectText, "direct text request generates DIRECT_TEXT_ONLY constraint");
  
  constraints = agent::core::BuildCompactConstraints("build a text analysis system");
  Check(constraints.empty(), "normal request has no constraints");
}

void TestRunLocalToolValidation() {
  using agent::core::ValidationContext;
  using agent::core::ValidationMode;
  using agent::core::ContentBlock;
  
  // Test: direct text response with Write tool should block
  ValidationContext ctx;
  ctx.mode = ValidationMode::ToolUse;
  ctx.originalUserGoal = "just tell me the answer directly";
  
  ContentBlock writeBlock = ContentBlock::MakeToolUse("write-1", "Write", R"({"file_path":"test.txt","content":"hi"})");
  ctx.toolUseBlocks.push_back(writeBlock);
  
  auto result = agent::core::RunLocalToolValidation(ctx);
  Check(result.immediateResult.has_value(), "direct text + Write blocks tool");
  if (result.immediateResult) {
    Check(!result.immediateResult->toolInterventions.empty(), "has interventions");
    Check(result.immediateResult->finalResponseAction == "retry_from_tools", "retry action set");
  }
  
  // Test: normal request with Write tool should NOT block
  ctx.originalUserGoal = "build a text analysis system";
  ctx.toolUseBlocks.clear();
  ctx.toolUseBlocks.push_back(writeBlock);
  
  result = agent::core::RunLocalToolValidation(ctx);
  Check(!result.immediateResult.has_value() || 
        (result.immediateResult->toolInterventions.empty()),
        "normal request + Write tool does NOT block");
  
  // Test: Read tool should never be blocked (it's read-only)
  ctx.originalUserGoal = "just tell me the answer";
  ctx.toolUseBlocks.clear();
  ContentBlock readBlock = ContentBlock::MakeToolUse("read-1", "Read", R"({"file_path":"test.txt"})");
  ctx.toolUseBlocks.push_back(readBlock);
  
  result = agent::core::RunLocalToolValidation(ctx);
  // Read is not in kMutatingToolNames, so it should NOT be blocked even for direct text
  bool readBlocked = false;
  if (result.immediateResult) {
    for (const auto& ti : result.immediateResult->toolInterventions) {
      if (ti.toolUseId == "read-1") readBlocked = true;
    }
  }
  Check(!readBlocked, "Read tool is NOT blocked for direct text requests");
}

void TestRunLocalFinalTextValidation() {
  using agent::core::ValidationContext;
  using agent::core::ValidationMode;
  
  // Test: JavaScript in HTML-only response
  ValidationContext ctx;
  ctx.mode = ValidationMode::FinalText;
  ctx.originalUserGoal = "create a webpage with no javascript";
  ctx.assistantText = "<html><script>alert('hi')</script></html>";
  
  auto result = agent::core::RunLocalFinalTextValidation(ctx);
  Check(result.immediateResult.has_value(), "JS in no-JS response triggers validation");
  
  // Test: No JS when not forbidden
  ctx.originalUserGoal = "create a webpage";
  result = agent::core::RunLocalFinalTextValidation(ctx);
  Check(!result.immediateResult.has_value(), "JS in normal response does NOT trigger");
  
  // Test: Markdown in plain text response
  ctx.originalUserGoal = "plain text no markdown";
  ctx.assistantText = "**bold** text";
  result = agent::core::RunLocalFinalTextValidation(ctx);
  Check(result.immediateResult.has_value(), "Markdown in plain text response triggers validation");
}

// STRENGTHEN-07: Verify the ValidatorTier comparison logic that gates whether
// the LLM validator runs. This is the core fix for the "same-tier
// net-negative" death-loop (validator-retry-limit-session evidence).
void TestValidatorTierComparison() {
  using namespace agent::core;
  // Cloud validating local -> Stronger (LLM validator should run)
  Check(CompareModelFamilies(ModelFamily::Qwen, ModelFamily::Claude)
        == ValidatorTier::Stronger,
        "Claude validating Qwen => Stronger");
  Check(CompareModelFamilies(ModelFamily::Gemma, ModelFamily::Claude)
        == ValidatorTier::Stronger,
        "Claude validating Gemma => Stronger");
  // Local validating cloud -> Weaker (LLM validator should NOT run)
  Check(CompareModelFamilies(ModelFamily::Claude, ModelFamily::Qwen)
        == ValidatorTier::Weaker,
        "Qwen validating Claude => Weaker");
  // Same-tier local-local -> Peer (the death-loop scenario)
  Check(CompareModelFamilies(ModelFamily::Qwen, ModelFamily::Gemma)
        == ValidatorTier::Peer,
        "Gemma validating Qwen => Peer (no net benefit)");
  Check(CompareModelFamilies(ModelFamily::Gemma, ModelFamily::Qwen)
        == ValidatorTier::Peer,
        "Qwen validating Gemma => Peer (no net benefit)");
  Check(CompareModelFamilies(ModelFamily::Qwen, ModelFamily::Qwen)
        == ValidatorTier::Peer,
        "Qwen validating Qwen => Peer (self-validation)");
  // Cloud-cloud -> Peer (conservative)
  Check(CompareModelFamilies(ModelFamily::Claude, ModelFamily::Claude)
        == ValidatorTier::Peer,
        "Claude validating Claude => Peer");
}

int main() {
  TestExtractOriginalUserGoal();
  TestRequiresDirectTextResponse();
  TestForbidsFileOperations();
  TestForbidsJavaScript();
  TestForbidsMarkdown();
  TestBuildCompactConstraints();
  TestRunLocalToolValidation();
  TestRunLocalFinalTextValidation();
  TestValidatorTierComparison();
  
  if (failures == 0) {
    std::cout << "All validator tests PASSED" << std::endl;
    return 0;
  }
  std::cerr << "[test_validator] Failures: " << failures << std::endl;
  return 1;
}
