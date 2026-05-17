#include "agents/SubAgentManager.h"

#include <iostream>

static int failures = 0;
static void Check(bool condition, const char* label) {
  if (!condition) { std::cerr << "FAIL: " << label << std::endl; ++failures; }
}

namespace {

void TestSubAgentTaskLifecycle() {
  agent::agents::SubAgentManager manager;
  manager.SetWorkerRuntimeRoot("build\\subagent-test-p3");

  agent::agents::SubAgentTask task;
  task.prompt = "P3 test task.";
  task.priority = 5;
  task.isolation = "none";

  std::string taskId = manager.StartSubTask(task);
  Check(!taskId.empty(), "StartSubTask should return taskId");

  bool updated = manager.UpdateTaskState(
      taskId, agent::agents::SubAgentTaskState::Completed, "p3 done");
  Check(updated, "UpdateTaskState should succeed");

  agent::agents::SubAgentTaskLifecycle lifecycle;
  bool got = manager.TryGetTask(taskId, &lifecycle);
  Check(got, "TryGetTask should retrieve task");
  Check(lifecycle.summary == "p3 done", "Task summary should match");
}

void TestSubAgentIsForkCandidate() {
  agent::agents::SubAgentManager manager;
  agent::core::Message nonFork;
  nonFork.role = agent::core::MessageRole::Assistant;
  nonFork.content.push_back(agent::core::ContentBlock::MakeText("hello"));
  Check(!manager.IsForkCandidate(nonFork), "Text-only not fork candidate");
}

void TestSubAgentForkMessages() {
  agent::agents::SubAgentManager manager;
  agent::core::Message assistant;
  assistant.role = agent::core::MessageRole::Assistant;
  assistant.content.push_back(agent::core::ContentBlock::MakeToolUse(
      "tu-001", "FileRead", R"({"path":"README.md"})"));
  const auto forked = manager.BuildForkedMessages(
      "Test directive", assistant, "C:\\repo", "C:\\wt");
  Check(forked.size() >= 2, "Fork should have 2 messages");
}

void TestSubAgentForkWithRenderedPrompt() {
  agent::agents::SubAgentManager manager;
  agent::core::Message assistant;
  assistant.role = agent::core::MessageRole::Assistant;
  assistant.content.push_back(agent::core::ContentBlock::MakeToolUse(
      "tu-001", "FileRead", R"({"path":"test"})"));

  std::string rendered = "<system>You are a helpful agent.</system>";
  const auto forked = manager.BuildForkedMessages(
      "Test", assistant, "C:\\", "C:\\wt", rendered);

  bool foundCacheTag = false;
  for (const auto& msg : forked)
    for (const auto& block : msg.content)
      if (block.type == agent::core::BlockType::Text &&
          block.asText.text.find("<prompt_cache_prefix>") != std::string::npos)
        foundCacheTag = true;
  Check(foundCacheTag, "Fork with renderedSystemPrompt injects cache prefix");
}

void TestSubAgentWorktree() {
  agent::agents::SubAgentManager manager;
  manager.SetWorkerRuntimeRoot("build\\subagent-test-p3");
  std::string path = manager.GetWorktreePath("wt-001");
  Check(!path.empty(), "GetWorktreePath produces path");
  manager.RemoveWorktree(path, true);
}

void TestSubAgentWorkerControl() {
  agent::agents::SubAgentManager manager;
  manager.SetWorkerRuntimeRoot("build\\subagent-test-p3");
  agent::agents::SubAgentTask task;
  task.prompt = "control test";
  task.priority = 5;
  std::string id = manager.StartSubTask(task);
  bool stopped = manager.StopTask(id);
  Check(stopped, "StopTask should succeed");
}

void TestSubAgentSummary() {
  agent::agents::SubAgentManager manager;
  auto summary = manager.SummarizeTasks();
  Check(summary.pending >= 0, "Summary pending >= 0");
}

}  // namespace

int main() {
  TestSubAgentTaskLifecycle();
  TestSubAgentIsForkCandidate();
  TestSubAgentForkMessages();
  TestSubAgentForkWithRenderedPrompt();
  TestSubAgentWorktree();
  TestSubAgentWorkerControl();
  TestSubAgentSummary();

  std::cout << "[test_subagent] Failures: " << failures << std::endl;
  return failures > 0 ? 1 : 0;
}
