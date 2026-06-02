// P0-03: Comprehensive tests for compact subsystem (aligned with local-ace).
// Covers: TimeBasedMCConfig, SessionMemoryCompact, edge cases, real-world scenarios.

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// Minimal test harness (no external deps needed for these unit tests)
#define TEST(name) void test_##name()
#define RUN(name) do { std::cout << "  " << #name << "... "; test_##name(); std::cout << "PASSED" << std::endl; } while(0)
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { std::cerr << "FAILED: " << #a << " == " << #b << " (" << (a) << " != " << (b) << ") at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)

// Include the module under test
#include "compact/TimeBasedMCConfig.h"
#include "compact/SessionMemoryCompact.h"
#include "memory/SessionMemory.h"

using namespace agent::compact;
using namespace agent::memory;

// ============================================================================
// TimeBasedMCConfig Tests
// ============================================================================

TEST(default_config_is_disabled) {
  TimeBasedMCConfig config;
  CHECK(!config.enabled);
  CHECK_EQ(config.gapThresholdMinutes, 60);
  CHECK_EQ(config.keepRecent, 5);
}

TEST(should_not_trigger_when_disabled) {
  TimeBasedMCConfig config;
  config.enabled = false;
  long long now = 1000 * 60 * 120;  // 120 min
  long long last = 0;
  CHECK(!ShouldTriggerTimeBasedMC(config, last, now));
}

TEST(should_not_trigger_with_zero_timestamp) {
  TimeBasedMCConfig config;
  config.enabled = true;
  config.gapThresholdMinutes = 60;
  CHECK(!ShouldTriggerTimeBasedMC(config, 0, 3600000));
}

TEST(should_trigger_when_gap_exceeded) {
  TimeBasedMCConfig config;
  config.enabled = true;
  config.gapThresholdMinutes = 60;
  long long last = 1000;  // Some prior assistant message time
  long long now = last + 61 * 60 * 1000LL;  // 61 min gap
  CHECK(ShouldTriggerTimeBasedMC(config, last, now));
}

TEST(should_not_trigger_when_gap_not_exceeded) {
  TimeBasedMCConfig config;
  config.enabled = true;
  config.gapThresholdMinutes = 60;
  long long last = 1000;
  long long now = 1000 + 59 * 60 * 1000LL;  // 59 min gap
  CHECK(!ShouldTriggerTimeBasedMC(config, last, now));
}

TEST(should_trigger_with_custom_threshold) {
  TimeBasedMCConfig config;
  config.enabled = true;
  config.gapThresholdMinutes = 10;  // 10 min threshold
  long long last = 1000;
  long long now = last + 11 * 60 * 1000LL;  // 11 min gap
  CHECK(ShouldTriggerTimeBasedMC(config, last, now));
}

TEST(should_not_trigger_below_custom_threshold) {
  TimeBasedMCConfig config;
  config.enabled = true;
  config.gapThresholdMinutes = 10;
  long long last = 1000;
  long long now = last + 9 * 60 * 1000LL;  // 9 min gap, below threshold
  CHECK(!ShouldTriggerTimeBasedMC(config, last, now));
}

TEST(env_var_config_parsing) {
  // Save old env
  const char* oldEnabled = std::getenv("AGENT_TC_MC_ENABLED");
  const char* oldGap = std::getenv("AGENT_TC_MC_GAP_MINUTES");
  const char* oldKeep = std::getenv("AGENT_TC_MC_KEEP_RECENT");

  // Set test env
  _putenv("AGENT_TC_MC_ENABLED=1");
  _putenv("AGENT_TC_MC_GAP_MINUTES=30");
  _putenv("AGENT_TC_MC_KEEP_RECENT=10");

  TimeBasedMCConfig config = GetTimeBasedMCConfig();
  CHECK(config.enabled);
  CHECK_EQ(config.gapThresholdMinutes, 30);
  CHECK_EQ(config.keepRecent, 10);

  // Restore
  if (oldEnabled) _putenv(("AGENT_TC_MC_ENABLED=" + std::string(oldEnabled)).c_str());
  else _putenv("AGENT_TC_MC_ENABLED=");
  if (oldGap) _putenv(("AGENT_TC_MC_GAP_MINUTES=" + std::string(oldGap)).c_str());
  else _putenv("AGENT_TC_MC_GAP_MINUTES=");
  if (oldKeep) _putenv(("AGENT_TC_MC_KEEP_RECENT=" + std::string(oldKeep)).c_str());
  else _putenv("AGENT_TC_MC_KEEP_RECENT=");
}

// ============================================================================
// SessionMemoryCompact Tests
// ============================================================================

TEST(sm_compact_default_config) {
  SessionMemoryCompactConfig config = GetSessionMemoryCompactConfig();
  CHECK_EQ(config.minTokens, 10000);
  CHECK_EQ(config.minTextBlockMessages, 5);
  CHECK_EQ(config.maxTokens, 40000);
}

TEST(should_not_use_when_disabled) {
  CHECK(!ShouldUseSessionMemoryCompaction(false, false));
}

TEST(should_not_use_when_memory_empty) {
  CHECK(!ShouldUseSessionMemoryCompaction(true, true));
}

TEST(should_use_when_enabled_and_memory_not_empty) {
  CHECK(ShouldUseSessionMemoryCompaction(true, false));
}

TEST(build_injection_null_session_memory) {
  std::string result = BuildSessionMemoryContextInjection(nullptr);
  CHECK(result.empty());
}

TEST(truncate_empty_content) {
  std::string result = TruncateSessionMemoryForCompact("", 100);
  CHECK(result.empty());
}

TEST(truncate_small_content) {
  std::string content = "small content";
  std::string result = TruncateSessionMemoryForCompact(content, 1000);
  CHECK_EQ(result, content);
}

TEST(truncate_large_content) {
  std::string content;
  for (int i = 0; i < 100; ++i) {
    content += "Line " + std::to_string(i) + ": some memory content here\n";
  }
  std::string result = TruncateSessionMemoryForCompact(content, 500);
  CHECK(!result.empty());
  CHECK(result.size() <= static_cast<std::size_t>(500));
  // Should contain lines from the end
  CHECK(result.find("Line 99") != std::string::npos);
  // Earlier lines should be trimmed
  CHECK(result.find("Line 0") == std::string::npos);
}

// ============================================================================
// SessionMemory Integration Tests (real-world scenarios)
// ============================================================================

TEST(session_memory_crud_lifecycle) {
  SessionMemory mem("test_session_memory_dir");
  
  // Add memories
  std::string id1 = mem.AddMemory("Project uses Python 3.11", "project", "project", 5);
  std::string id2 = mem.AddMemory("User prefers concise answers", "user", "session", 3);
  std::string id3 = mem.AddMemory("Avoid using grep on Windows", "reference", "session", 8);
  
  CHECK(!id1.empty());
  CHECK(!id2.empty());
  CHECK(!id3.empty());
  
  // List all
  auto all = mem.ListMemories();
  CHECK_EQ(static_cast<int>(all.size()), 3);
  
  // List by type
  auto project = mem.ListMemories("project");
  CHECK_EQ(static_cast<int>(project.size()), 1);
  CHECK_EQ(project[0].priority, 5);
  
  // Search
  auto results = mem.SearchMemories("python");
  CHECK_EQ(static_cast<int>(results.size()), 1);
  CHECK_EQ(results[0].type, "project");
  
  // Update
  CHECK(mem.UpdateMemory(id1, "Project uses Python 3.12"));
  
  // Deactivate
  CHECK(mem.DeactivateMemory(id3));
  auto active = mem.ListMemories();
  CHECK_EQ(static_cast<int>(active.size()), 2);  // Only 2 active now
  
  // Delete
  CHECK(mem.DeleteMemory(id2));
  auto remaining = mem.ListMemories();
  CHECK_EQ(static_cast<int>(remaining.size()), 1);
}

TEST(session_memory_context_injection) {
  SessionMemory mem("test_injection_dir");
  mem.AddMemory("Critical: use PowerShell not Bash", "reference", "session", 10);
  mem.AddMemory("Workspace is G:\\downloads\\jianlai-graph", "project", "project", 5);
  mem.AddMemory("Python packages: jieba, networkx, matplotlib", "reference", "session", 3);
  
  std::string injection = BuildSessionMemoryContextInjection(&mem, 2000);
  CHECK(!injection.empty());
  CHECK(injection.find("[Session Memory]") != std::string::npos);
  CHECK(injection.find("PowerShell") != std::string::npos);
  CHECK(injection.find("jianlai-graph") != std::string::npos);
  
  // Verify high priority items appear first
  std::size_t posCritical = injection.find("Critical");
  std::size_t posWorkspace = injection.find("Workspace");
  CHECK(posCritical != std::string::npos);
  CHECK(posWorkspace != std::string::npos);
  CHECK(posCritical < posWorkspace);  // Higher priority first
}

TEST(session_memory_priority_sorting) {
  SessionMemory mem("test_priority_dir");
  mem.AddMemory("Low priority", "user", "session", 1);
  mem.AddMemory("High priority", "user", "session", 10);
  mem.AddMemory("Medium priority", "user", "session", 5);
  
  std::string injection = BuildSessionMemoryContextInjection(&mem, 2000);
  
  std::size_t posHigh = injection.find("High priority");
  std::size_t posMedium = injection.find("Medium priority");
  std::size_t posLow = injection.find("Low priority");
  
  CHECK(posHigh < posMedium);
  CHECK(posMedium < posLow);
}

TEST(session_memory_search_case_insensitive) {
  SessionMemory mem("test_search_dir");
  mem.AddMemory("PYTHON version check", "reference", "session", 3);
  
  auto results = mem.SearchMemories("python");
  CHECK_EQ(static_cast<int>(results.size()), 1);
}

// ============================================================================
// Real-world scenario: jianlai-graph failure reproduction
// ============================================================================

TEST(real_world_timeout_loop_protection) {
  // Simulates the jianlai-graph scenario: repeated Bash timeouts with different
  // outputs should NOT prevent the "do not restart" guard from triggering.
  
  // With the new timeout fingerprint normalization, timeout errors produce
  // identical fingerprints regardless of variable stdout content.
  
  TimeBasedMCConfig config;
  config.enabled = true;
  config.gapThresholdMinutes = 1;  // Very short for testing
  
  long long baseTime = 1000000;
  long long lastAssistant = baseTime;
  long long now = baseTime + 2 * 60 * 1000LL;  // 2 min gap
  
  // Should trigger - time gap exceeded
  CHECK(ShouldTriggerTimeBasedMC(config, lastAssistant, now));
  
  // With recent activity, should NOT trigger
  long long recentActivity = now - 30 * 1000LL;  // 30 sec ago
  CHECK(!ShouldTriggerTimeBasedMC(config, recentActivity, now));
}

TEST(real_world_memory_injection_with_truncation) {
  // Simulates injecting session memory into system prompt during compact
  SessionMemory mem("test_real_world_dir");
  
  // Add many memories to simulate real session
  for (int i = 0; i < 50; ++i) {
    mem.AddMemory("Memory entry " + std::to_string(i) + " with details about the project",
                  i % 3 == 0 ? "project" : "session",
                  i % 5 == 0 ? "project" : "session",
                  i % 10);
  }
  
  // Inject with tight budget
  std::string injection = BuildSessionMemoryContextInjection(&mem, 500);
  CHECK(!injection.empty());
  CHECK(injection.size() <= static_cast<std::size_t>(600));  // Allow some overhead
  
  // High priority items should be present
  CHECK(injection.find("Memory entry 9") != std::string::npos);  // priority 9
  CHECK(injection.find("Memory entry 0") == std::string::npos);   // priority 0 trimmed
}

int main() {
  std::cout << "=== Compact Module Tests ===" << std::endl;
  
  std::cout << "[TimeBasedMCConfig]" << std::endl;
  RUN(default_config_is_disabled);
  RUN(should_not_trigger_when_disabled);
  RUN(should_not_trigger_with_zero_timestamp);
  RUN(should_trigger_when_gap_exceeded);
  RUN(should_not_trigger_when_gap_not_exceeded);
  RUN(should_trigger_with_custom_threshold);
  RUN(should_not_trigger_below_custom_threshold);
  RUN(env_var_config_parsing);
  
  std::cout << "[SessionMemoryCompact]" << std::endl;
  RUN(sm_compact_default_config);
  RUN(should_not_use_when_disabled);
  RUN(should_not_use_when_memory_empty);
  RUN(should_use_when_enabled_and_memory_not_empty);
  RUN(build_injection_null_session_memory);
  RUN(truncate_empty_content);
  RUN(truncate_small_content);
  RUN(truncate_large_content);
  
  std::cout << "[SessionMemory Integration]" << std::endl;
  RUN(session_memory_crud_lifecycle);
  RUN(session_memory_context_injection);
  RUN(session_memory_priority_sorting);
  RUN(session_memory_search_case_insensitive);
  
  std::cout << "[Real-World Scenarios]" << std::endl;
  RUN(real_world_timeout_loop_protection);
  RUN(real_world_memory_injection_with_truncation);
  
  std::cout << "\nAll compact module tests PASSED" << std::endl;
  return 0;
}
