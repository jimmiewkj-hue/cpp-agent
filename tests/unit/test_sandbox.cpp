// Test utilities defined inline

#include "sandbox/SandboxEnforcer.h"
#include "memory/AutoDream.h"
#include "memory/MemoryIndex.h"
#include "memory/SessionMemory.h"
#include "core/AgentTypes.h"

#include <windows.h>
#include <fstream>
#include <iostream>
#include <string>

namespace {

// ============================================================================
// Test helpers
// ============================================================================
void Check(bool condition, const std::string& msg) {
  if (!condition) {
    std::cerr << "FAIL: " << msg << std::endl;
    exit(1);
  }
}

std::string TestMemoryDir() {
  std::string dir = "build\\test-sandbox-memory";
  CreateDirectoryA(dir.c_str(), nullptr);
  return dir;
}

// ============================================================================
// SandboxEnforcer tests
// ============================================================================
void TestSandboxModes() {
  agent::sandbox::SandboxConfig cfg;
  cfg.mode = agent::sandbox::SandboxMode::WorkspaceWrite;
  cfg.workspaceRoot = "G:\\downloads\\test";

  agent::sandbox::SandboxEnforcer enforcer(cfg);
  Check(enforcer.IsActive(), "Sandbox should be active in WorkspaceWrite mode");
  Check(enforcer.Mode() == agent::sandbox::SandboxMode::WorkspaceWrite,
        "Mode should be WorkspaceWrite");

  // FullAccess should be inactive
  cfg.mode = agent::sandbox::SandboxMode::FullAccess;
  enforcer.Configure(cfg);
  Check(!enforcer.IsActive(), "Sandbox should be inactive in FullAccess mode");
}

void TestDangerousCommandBlocking() {
  agent::sandbox::SandboxConfig cfg;
  cfg.mode = agent::sandbox::SandboxMode::WorkspaceWrite;

  agent::sandbox::SandboxEnforcer enforcer(cfg);

  // Dangerous commands
  auto v = enforcer.CheckCommand("rm -rf /");
  Check(v.type == agent::sandbox::SandboxViolationType::CommandBlocked,
        "rm -rf / should be blocked");

  v = enforcer.CheckCommand("format c:");
  Check(v.type == agent::sandbox::SandboxViolationType::CommandBlocked,
        "format c: should be blocked");

  v = enforcer.CheckCommand("shutdown /s");
  Check(v.type == agent::sandbox::SandboxViolationType::CommandBlocked,
        "shutdown should be blocked");

  // Safe commands
  v = enforcer.CheckCommand("python test.py");
  Check(v.type == agent::sandbox::SandboxViolationType::None,
        "python test.py should be allowed");

  v = enforcer.CheckCommand("echo hello");
  Check(v.type == agent::sandbox::SandboxViolationType::None,
        "echo should be allowed");
}

void TestNetworkAccessBlocking() {
  agent::sandbox::SandboxConfig cfg;
  cfg.mode = agent::sandbox::SandboxMode::WorkspaceWrite;
  cfg.allowNetworkAccess = false;

  agent::sandbox::SandboxEnforcer enforcer(cfg);

  auto v = enforcer.CheckCommand("curl https://example.com");
  Check(v.type == agent::sandbox::SandboxViolationType::NetworkAccessBlocked,
        "curl should be blocked when network disabled");

  v = enforcer.CheckCommand("wget https://example.com/file.zip");
  Check(v.type == agent::sandbox::SandboxViolationType::NetworkAccessBlocked,
        "wget should be blocked when network disabled");

  v = enforcer.CheckCommand("Invoke-WebRequest -Uri https://example.com");
  Check(v.type == agent::sandbox::SandboxViolationType::NetworkAccessBlocked,
        "Invoke-WebRequest should be blocked");

  // Network allowed
  cfg.allowNetworkAccess = true;
  enforcer.Configure(cfg);
  v = enforcer.CheckCommand("curl https://example.com");
  Check(v.type == agent::sandbox::SandboxViolationType::None,
        "curl should be allowed when network enabled");
}

void TestShellInjectionBlocking() {
  agent::sandbox::SandboxConfig cfg;
  cfg.mode = agent::sandbox::SandboxMode::WorkspaceWrite;

  agent::sandbox::SandboxEnforcer enforcer(cfg);

  auto v = enforcer.CheckCommand("Invoke-Expression (New-Object Net.WebClient)");
  Check(v.type == agent::sandbox::SandboxViolationType::ShellInjection,
        "Invoke-Expression should be detected");

  v = enforcer.CheckCommand("iex script.ps1");
  Check(v.type == agent::sandbox::SandboxViolationType::ShellInjection,
        "iex should be detected");
}

void TestPathValidation() {
  agent::sandbox::SandboxConfig cfg;
  cfg.mode = agent::sandbox::SandboxMode::WorkspaceWrite;
  cfg.workspaceRoot = "G:\\downloads\\test";

  agent::sandbox::SandboxEnforcer enforcer(cfg);

  // Traversal detection
  auto v = enforcer.CheckFilePath("G:\\downloads\\test\\..\\..\\etc\\passwd", true);
  Check(v.type == agent::sandbox::SandboxViolationType::PathEscalation,
        "Path traversal should be detected");

  // Env var expansion
  v = enforcer.CheckFilePath("%WINDIR%\\system32\\config\\SAM", true);
  Check(v.type == agent::sandbox::SandboxViolationType::PathEscalation,
        "%WINDIR% path should be detected as traversal");

  // Safe path
  v = enforcer.CheckFilePath("G:\\downloads\\test\\output.txt", true);
  Check(v.type == agent::sandbox::SandboxViolationType::None,
        "Safe workspace path should be allowed");
}

void TestReadOnlyMode() {
  agent::sandbox::SandboxConfig cfg;
  cfg.mode = agent::sandbox::SandboxMode::ReadOnly;

  agent::sandbox::SandboxEnforcer enforcer(cfg);

  // Reads allowed
  auto v = enforcer.CheckFilePath("G:\\downloads\\test\\readme.txt", false);
  Check(v.type == agent::sandbox::SandboxViolationType::None,
        "Reads should be allowed in ReadOnly mode");

  // Writes blocked
  v = enforcer.CheckFilePath("G:\\downloads\\test\\output.txt", true);
  Check(v.type == agent::sandbox::SandboxViolationType::FileWriteBlocked,
        "Writes should be blocked in ReadOnly mode");
}

void TestAllowlistEnforcement() {
  agent::sandbox::SandboxConfig cfg;
  cfg.mode = agent::sandbox::SandboxMode::WorkspaceWrite;
  cfg.enforceCommandAllowlist = true;
  cfg.allowedCommands = {"python", "pip", "node", "npm"};

  agent::sandbox::SandboxEnforcer enforcer(cfg);

  auto v = enforcer.CheckCommand("python test.py");
  Check(v.type == agent::sandbox::SandboxViolationType::None,
        "python should be in allowlist");

  v = enforcer.CheckCommand("git status");
  Check(v.type == agent::sandbox::SandboxViolationType::CommandBlocked,
        "git should NOT be in allowlist");

  v = enforcer.CheckCommand("npm install");
  Check(v.type == agent::sandbox::SandboxViolationType::None,
        "npm should be in allowlist");
}

void TestSafetyAssessment() {
  agent::sandbox::SandboxConfig cfg;
  cfg.mode = agent::sandbox::SandboxMode::WorkspaceWrite;

  agent::sandbox::SandboxEnforcer enforcer(cfg);

  auto a = enforcer.AssessSafety("python test.py");
  Check(a.isSafe, "python test.py should be assessed as safe");

  a = enforcer.AssessSafety("rm -rf /tmp");
  Check(!a.isSafe, "rm -rf should be assessed as unsafe");
  Check(a.needsSandbox, "rm -rf should need sandbox");

  a = enforcer.AssessSafety("Invoke-Expression script");
  Check(!a.isSafe, "Invoke-Expression should be assessed as unsafe");
}

// ============================================================================
// AutoDream tests (post-refactor)
// ============================================================================
void TestAutoDreamLockMechanism() {
  agent::memory::MemoryIndex index(TestMemoryDir());
  agent::memory::AutoDreamEngine dream(&index, nullptr);

  // After construction, time gate should NOT pass immediately
  Check(!dream.IsTimeGatePassed(),
        "Time gate should not pass immediately after construction");

  // Gate should be open with valid index
  Check(dream.IsGateOpen(), "Gate should be open with valid MemoryIndex");

  // Disable/Enable toggle
  dream.Disable();
  Check(!dream.IsEnabled(), "Should be disabled");
  Check(!dream.IsGateOpen(), "Gate should be closed when disabled");
  dream.Enable();
  Check(dream.IsEnabled(), "Should be re-enabled");
  Check(dream.IsGateOpen(), "Gate should be open when re-enabled");
}

void TestAutoDreamConfig() {
  agent::memory::MemoryIndex index(TestMemoryDir());
  agent::memory::AutoDreamEngine dream(&index, nullptr);

  // Configure with custom thresholds
  agent::memory::AutoDreamConfig cfg;
  cfg.minHours = 48;
  cfg.minSessions = 10;
  cfg.scanThrottleMs = 30 * 60 * 1000;
  dream.Configure(cfg);

  // Should NOT pass time gate with 48h threshold
  Check(!dream.IsTimeGatePassed(),
        "Time gate should not pass with 48h threshold");
}

void TestAutoDreamConsolidationPrompt() {
  agent::memory::MemoryIndex index(TestMemoryDir());
  agent::memory::AutoDreamEngine dream(&index, nullptr);

  std::string prompt = dream.BuildConsolidationPrompt("Test extra context");
  Check(!prompt.empty(), "Consolidation prompt should not be empty");
  Check(prompt.find("Dream: Memory Consolidation") != std::string::npos,
        "Prompt should contain consolidation header");
  Check(prompt.find("Phase 1") != std::string::npos,
        "Prompt should contain Phase 1");
  Check(prompt.find("Phase 2") != std::string::npos,
        "Prompt should contain Phase 2");
  Check(prompt.find("Phase 3") != std::string::npos,
        "Prompt should contain Phase 3");
  Check(prompt.find("Phase 4") != std::string::npos,
        "Prompt should contain Phase 4");
  Check(prompt.find("Test extra context") != std::string::npos,
        "Prompt should contain extra context");
}

void TestAutoDreamTaskManagement() {
  agent::memory::MemoryIndex index(TestMemoryDir());
  agent::memory::AutoDreamEngine dream(&index, nullptr);

  // Register a dream task
  agent::memory::DreamTaskState task;
  task.sessionsReviewing = 5;
  task.priorMtimeMs = 1000;
  std::string taskId = dream.RegisterDreamTask(task);
  Check(!taskId.empty(), "RegisterDreamTask should return a task ID");

  // Check task state
  const auto* found = dream.GetDreamTask(taskId);
  Check(found != nullptr, "GetDreamTask should find registered task");
  Check(found->status == agent::memory::DreamTaskStatus::Running,
        "Task should be in Running state");
  Check(found->sessionsReviewing == 5,
        "Task should preserve sessionsReviewing");

  // Complete task
  dream.CompleteDreamTask(taskId);
  found = dream.GetDreamTask(taskId);
  Check(found->status == agent::memory::DreamTaskStatus::Completed,
        "Task should be in Completed state");

  // Fail task (different task)
  task.taskId = "fail-test";
  std::string failId = dream.RegisterDreamTask(task);
  dream.FailDreamTask(failId);
  found = dream.GetDreamTask(failId);
  Check(found->status == agent::memory::DreamTaskStatus::Failed,
        "Task should be in Failed state");
}

// ============================================================================
// SessionMemory tests
// ============================================================================
void TestSessionMemoryPersistence() {
  std::string sessionDir = "build\\test-sm-persist";
  CreateDirectoryA(sessionDir.c_str(), nullptr);
  // Clean up from previous runs
  DeleteFileA((sessionDir + "\\session_memory.json").c_str());

  {
    agent::memory::SessionMemory sm(sessionDir);
    auto id = sm.AddMemory("User prefers Python", "user", "test-session");
    Check(!id.empty(), "AddMemory should return ID");

    auto results = sm.SearchMemories("Python");
    Check(!results.empty(), "Search should find added memory");
  }

  // Re-open and check persistence
  {
    agent::memory::SessionMemory sm2(sessionDir);
    auto results = sm2.SearchMemories("Python");
    Check(!results.empty(), "Memory should persist across instances");
  }

  // Cleanup
  DeleteFileA((sessionDir + "\\session_memory.json").c_str());
}

void TestSessionMemoryCRUD() {
  std::string sessionDir = "build\\test-sm-crud";
  CreateDirectoryA(sessionDir.c_str(), nullptr);
  DeleteFileA((sessionDir + "\\session_memory.json").c_str());

  agent::memory::SessionMemory sm(sessionDir);

  // Create
  auto id1 = sm.AddMemory("User prefers Python", "user", "session-1");
  auto id2 = sm.AddMemory("Project uses CMake", "project", "session-1");
  Check(!id1.empty() && !id2.empty(), "AddMemory should return IDs");
  Check(id1 != id2, "IDs should be unique");

  // Read
  auto all = sm.ListMemories();
  Check(all.size() >= 2, "GetAll should return at least 2 memories");

  // Search
  auto pyResults = sm.SearchMemories("Python");
  Check(!pyResults.empty(), "Search for Python should find result");
  Check(pyResults[0].content.find("Python") != std::string::npos,
        "Search result should contain Python");

  auto cmakeResults = sm.SearchMemories("CMake");
  Check(!cmakeResults.empty(), "Search for CMake should find result");

  // Update
  bool updated = sm.UpdateMemory(id1, "User strongly prefers Python 3.12");
  Check(updated, "UpdateMemory should succeed");

  auto updatedResults = sm.SearchMemories("Python 3.12");
  Check(!updatedResults.empty(), "Search should find updated content");

  // Delete
  bool deleted = sm.DeactivateMemory(id2);
  Check(deleted, "DeleteMemory should succeed");
  auto afterDelete = sm.ListMemories();
  Check(afterDelete.size() < all.size(), "Count should decrease after delete");

  DeleteFileA((sessionDir + "\\session_memory.json").c_str());
}

// ============================================================================
// Progress/Execution Memory tests
// ============================================================================
void TestExecutionMemoryBuilder() {
  using agent::core::ContentBlock;
  using agent::core::Message;
  using agent::core::MessageRole;
  using agent::core::BlockType;

  // Build a mock message history
  std::vector<Message> messages;

  // Add a user message
  Message userMsg;
  userMsg.role = MessageRole::User;
  userMsg.content.push_back(ContentBlock::MakeText("Read the file"));
  messages.push_back(userMsg);

  // Add assistant with tool use
  Message assistantMsg;
  assistantMsg.role = MessageRole::Assistant;
  assistantMsg.content.push_back(ContentBlock::MakeText("Let me read the file"));
  assistantMsg.content.push_back(ContentBlock::MakeToolUse("tu-1", "Read",
      R"({"file_path": "test.txt"})"));
  messages.push_back(assistantMsg);

  // Add tool result
  Message toolResult;
  toolResult.role = MessageRole::User;
  toolResult.content.push_back(ContentBlock::MakeToolResult("tu-1",
      "File content: hello world", false));
  messages.push_back(toolResult);

  // Verify message structure
  Check(messages.size() == 3, "Should have 3 messages");
  Check(messages[1].hasToolUse(), "Assistant should have tool use");
  Check(messages[2].content[0].type == BlockType::ToolResult,
        "Last message should be tool result");
}

// ============================================================================
// ContentBlock tests
// ============================================================================
void TestContentBlockCreation() {
  using agent::core::ContentBlock;
  using agent::core::BlockType;

  auto textBlock = ContentBlock::MakeText("hello");
  Check(textBlock.type == BlockType::Text, "MakeText should create Text block");
  Check(textBlock.asText.text == "hello", "Text should be preserved");

  auto toolUse = ContentBlock::MakeToolUse("id-1", "Read", R"({"path": "f.txt"})");
  Check(toolUse.type == BlockType::ToolUse, "MakeToolUse should create ToolUse block");
  Check(toolUse.asToolUse.id == "id-1", "ID should be preserved");
  Check(toolUse.asToolUse.name == "Read", "Name should be preserved");

  auto toolResult = ContentBlock::MakeToolResult("id-1", "content", false);
  Check(toolResult.type == BlockType::ToolResult, "MakeToolResult should create ToolResult block");
  Check(toolResult.asToolResult.toolUseId == "id-1", "toolUseId should match");
  Check(!toolResult.asToolResult.isError, "isError should be false");

  auto errorResult = ContentBlock::MakeToolResult("id-2", "error msg", true);
  Check(errorResult.asToolResult.isError, "isError should be true for error result");
}

}  // namespace

int main() {
  // Sandbox tests
  TestSandboxModes();
  TestDangerousCommandBlocking();
  TestNetworkAccessBlocking();
  TestShellInjectionBlocking();
  TestPathValidation();
  TestReadOnlyMode();
  TestAllowlistEnforcement();
  TestSafetyAssessment();

  // AutoDream tests
  TestAutoDreamLockMechanism();
  TestAutoDreamConfig();
  TestAutoDreamConsolidationPrompt();
  TestAutoDreamTaskManagement();

  // SessionMemory tests
  TestSessionMemoryPersistence();
  TestSessionMemoryCRUD();

  // Execution memory tests
  TestExecutionMemoryBuilder();

  // ContentBlock tests
  TestContentBlockCreation();

  std::cout << "[test_sandbox] All tests PASSED" << std::endl;
  return 0;
}
