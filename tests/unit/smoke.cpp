#include "agents/SubAgentManager.h"
#include "mcp/McpClientManager.h"
#include "memory/MemoryIndex.h"
#include "core/QueryEngine.h"
#include "core/StateTypes.h"
#include "api/ModelClient.h"
#include "api/SideQueryClient.h"
#include "infra/ProcessRunner.h"
#include "infra/SessionManager.h"
#include "infra/StabilityWatchdog.h"
#include "permissions/PermissionEngine.h"
#include "tools/ToolOrchestrator.h"
#include "tools/ToolRegistry.h"
#include "third_party/nlohmann_json.hpp"

#include <windows.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

static int failures = 0;

static void Check(bool condition, const char* label) {
  if (!condition) {
    std::cerr << "FAIL: " << label << std::endl;
    ++failures;
  }
}

namespace {

std::string CurrentExecutableDir() {
  char buffer[MAX_PATH] = {0};
  const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return std::string();
  }
  std::string path(buffer, buffer + length);
  const std::size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

struct SpawnedProcess {
  HANDLE process = nullptr;
  HANDLE thread = nullptr;
};

SpawnedProcess StartBackgroundProcess(
    const std::string& executable,
    const std::vector<std::string>& arguments) {
  SpawnedProcess spawned;
  std::ostringstream command;
  command << "\"" << executable << "\"";
  for (const auto& arg : arguments) {
    command << " \"" << arg << "\"";
  }
  std::string utf8 = command.str();
  const int size = MultiByteToWideChar(
      CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(
      CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &wide[0], size);
  std::vector<wchar_t> mutableCommand(wide.begin(), wide.end());
  mutableCommand.push_back(L'\0');

  STARTUPINFOW startupInfo;
  ZeroMemory(&startupInfo, sizeof(startupInfo));
  startupInfo.cb = sizeof(startupInfo);

  PROCESS_INFORMATION processInfo;
  ZeroMemory(&processInfo, sizeof(processInfo));

  if (CreateProcessW(
          nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
          CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo)) {
    spawned.process = processInfo.hProcess;
    spawned.thread = processInfo.hThread;
  }
  return spawned;
}

void StopBackgroundProcess(SpawnedProcess* process) {
  if (process == nullptr) {
    return;
  }
  if (process->process != nullptr) {
    TerminateProcess(process->process, 0);
    WaitForSingleObject(process->process, 500);
    CloseHandle(process->process);
    process->process = nullptr;
  }
  if (process->thread != nullptr) {
    CloseHandle(process->thread);
    process->thread = nullptr;
  }
}

class FakeMcpTransport : public agent::mcp::McpTransport {
 public:
  bool Connect(
      const agent::mcp::McpServerConfig&,
      std::string* error) override {
    if (error) {
      error->clear();
    }
    connected_ = true;
    return true;
  }

  void PopulateConnectionState(
      agent::mcp::McpServerConnection* connection) const override {
    if (connection == nullptr) {
      return;
    }
    connection->capabilities.tools = true;
    connection->serverVersion = "fake-1.0";
  }

  agent::mcp::McpTransportResponse Send(
      const agent::mcp::McpTransportRequest& request) override {
    if (!connected_) {
      return {false, std::string(), "not connected"};
    }
    if (request.method != "tools/list") {
      return {false, std::string(), "unsupported method"};
    }
    return {
        true,
        R"({"tools":[{"name":"Create Issue","description":"Create issue","inputSchema":{"type":"object","properties":{"title":{"type":"string"}}},"annotations":{"readOnlyHint":false,"destructiveHint":true,"openWorldHint":true}},{"name":"List Issues","description":"List issues","inputSchema":{"type":"object"},"annotations":{"readOnlyHint":true,"destructiveHint":false,"openWorldHint":false}}]})",
        std::string()};
  }

  void Close() override { connected_ = false; }

 private:
  bool connected_ = false;
};

class SessionAwareFakeHttpTransport : public agent::mcp::McpTransport {
 public:
  bool Connect(
      const agent::mcp::McpServerConfig& config,
      std::string* error) override {
    if (error) {
      error->clear();
    }
    connected_ = true;
    serverName_ = config.name;
    return true;
  }

  void PopulateConnectionState(
      agent::mcp::McpServerConnection* connection) const override {
    if (connection == nullptr) {
      return;
    }
    connection->capabilities.tools = true;
    connection->streamableHttp = true;
    connection->clientSessionId = "client-session-1";
    connection->transportSessionId = currentSessionId_;
    connection->lastActivityUnixMs = lastActivityUnixMs_;
    connection->sessionExpiresAtUnixMs = expiresAtUnixMs_;
    connection->reconnectCount = reconnectCount_;
    connection->serverVersion = "fake-http-1.0";
  }

  agent::mcp::McpTransportResponse Send(
      const agent::mcp::McpTransportRequest& request) override {
    if (!connected_) {
      return {false, std::string(), "not connected"};
    }
    ++sendCount_;
    lastActivityUnixMs_ = 1715932800000LL + sendCount_;
    expiresAtUnixMs_ = lastActivityUnixMs_ + 600000LL;

    if (request.method == "tools/list" && sendCount_ == 1) {
      currentSessionId_ = "srv-session-1";
      return {true,
              R"({"result":{"tools":[{"name":"Http Echo","description":"Echo over http","inputSchema":{"type":"object"},"annotations":{"readOnlyHint":true,"destructiveHint":false,"openWorldHint":false}}]}})",
              std::string()};
    }
    if (request.method == "tools/list") {
      ++reconnectCount_;
      currentSessionId_ = "srv-session-reconnected";
      return {true,
              R"({"result":{"tools":[{"name":"Http Echo","description":"Echo over http","inputSchema":{"type":"object"},"annotations":{"readOnlyHint":true,"destructiveHint":false,"openWorldHint":false}}]}})",
              std::string()};
    }
    return {false, std::string(), "unsupported method"};
  }

  void Close() override { connected_ = false; }

 private:
  bool connected_ = false;
  int sendCount_ = 0;
  int reconnectCount_ = 0;
  std::string serverName_;
  std::string currentSessionId_ = "srv-session-0";
  long long lastActivityUnixMs_ = 0;
  long long expiresAtUnixMs_ = 0;
};

class RecordingModelClient : public agent::api::ModelClient {
 public:
  std::vector<agent::core::Message> GenerateResponse(
      const std::vector<agent::core::Message>&,
      const std::string& systemPrompt,
      const std::string&) override {
    lastSystemPrompt = systemPrompt;
    agent::core::Message message;
    message.role = agent::core::MessageRole::Assistant;
    message.uuid = "recording-final";
    message.stopReason = "end_turn";
    message.content.push_back(
        agent::core::ContentBlock::MakeText("recorded prompt"));
    return {message};
  }

  void StreamResponse(
      const std::vector<agent::core::Message>&,
      const std::string& systemPrompt,
      const std::string&,
      const std::string&,
      const agent::api::SseEventCallback& onEvent,
      int) override {
    lastSystemPrompt = systemPrompt;
    if (onEvent) {
      onEvent("text_delta", streamResponseText);
      if (emitToolUse) {
        onEvent("tool_use",
                "{\"id\":\"recording-tool-1\",\"name\":\"" + streamToolName +
                    "\",\"input\":" + streamToolInputJson + "}");
      }
      onEvent("stop_reason", "end_turn");
    }
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string& systemPrompt,
      const std::string& model) override {
    lastSideQueryPrompt = systemPrompt;
    lastSideQueryModel = model;
    agent::core::Message message;
    message.role = agent::core::MessageRole::Assistant;
    message.uuid = "recording-side";
    message.content.push_back(
        agent::core::ContentBlock::MakeText(sideQueryResponseText));
    return {message};
  }

  std::string lastSystemPrompt;
  std::string lastSideQueryPrompt;
  std::string lastSideQueryModel;
  std::string streamResponseText = "recorded streaming prompt";
  std::string sideQueryResponseText = "side query";
  std::string streamToolName = "FileRead";
  std::string streamToolInputJson = R"({"path":"README.md"})";
  bool emitToolUse = false;
};

class SuccessfulToolThenFinalModelClient : public agent::api::ModelClient {
 public:
  explicit SuccessfulToolThenFinalModelClient(std::string outputPath)
      : outputPath_(outputPath) {}

  std::vector<agent::core::Message> GenerateResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  void StreamResponse(
      const std::vector<agent::core::Message>& messages,
      const std::string&,
      const std::string&,
      const std::string&,
      const agent::api::SseEventCallback& onEvent,
      int) override {
    ++streamCallCount;
    if (streamCallCount == 2) {
      sawToolResultOnFollowup = HasToolResult(messages, "success-tool-1");
    }
    if (!onEvent) return;

    if (streamCallCount == 1) {
      nlohmann::json toolEvent;
      toolEvent["id"] = "success-tool-1";
      toolEvent["name"] = "FileWrite";
      toolEvent["input"] = {
          {"file_path", outputPath_},
          {"content", "<html><body>ok</body></html>"},
      };
      onEvent("text_delta", "Creating HTML file.");
      onEvent("tool_use", toolEvent.dump());
      onEvent("stop_reason", "tool_use");
      return;
    }

    onEvent("text_delta", "File is ready.");
    onEvent("stop_reason", "end_turn");
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  static bool HasToolResult(const std::vector<agent::core::Message>& messages,
                            const std::string& toolUseId) {
    for (const auto& msg : messages) {
      if (msg.role != agent::core::MessageRole::User) continue;
      for (const auto& block : msg.content) {
        if (block.type != agent::core::BlockType::ToolResult) continue;
        if (block.asToolResult.toolUseId == toolUseId) return true;
      }
    }
    return false;
  }

  int streamCallCount = 0;
  bool sawToolResultOnFollowup = false;

 private:
  std::string outputPath_;
};

}  // namespace

int main() {
  agent::agents::SubAgentManager subAgentManager;
  const std::string executableDir = CurrentExecutableDir();
  subAgentManager.SetWorkerExecutablePath(
      executableDir + "\\agent_subagent_worker.exe");
  subAgentManager.SetWorkerRuntimeRoot(
      executableDir + "\\subagent-runtime-tests");
  agent::api::SkeletonModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::mcp::McpClientManager mcpClientManager;
  agent::memory::MemoryIndex memoryIndex(
      "G:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent\\build\\test-memory");
  agent::tools::ToolRegistry toolRegistry;
  agent::tools::ToolOrchestrator toolOrchestrator;
  agent::permissions::PermissionEngine permissionEngine;
  agent::infra::ProcessRunner processRunner;
  agent::infra::SessionManager sessionManager(
      "G:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent\\build\\test-session");
  agent::infra::StabilityWatchdog watchdog(
      agent::infra::StabilityConfig{});

  toolRegistry.RegisterTool(
      {"FileRead", "Read files", R"({"type":"object"})",
       agent::tools::ToolExecCategory::ReadOnly, true, false, 100000});
  toolRegistry.RegisterTool(
      {"Grep", "Search files", R"({"type":"object"})",
       agent::tools::ToolExecCategory::ReadOnly, true, false, 100000});
  toolRegistry.RegisterTool(
      {"Glob", "Find files", R"({"type":"object"})",
       agent::tools::ToolExecCategory::ReadOnly, true, false, 100000});
  toolRegistry.RegisterTool(
      {"FileWrite", "Write content to a file",
       R"({"type":"object","properties":{"file_path":{"type":"string"},"content":{"type":"string"}},"required":["file_path","content"]})",
       agent::tools::ToolExecCategory::FileWrite, false, true, 100000});
  toolRegistry.RegisterTool(
      {"Bash", "Execute a shell command",
       R"({"type":"object","properties":{"command":{"type":"string"}},"required":["command"]})",
       agent::tools::ToolExecCategory::ShellCommand, false, true, 400000});

  // Test 1: allow rule matches
  permissionEngine.AddAlwaysAllowRule("FileRead");
  {
    auto block = agent::core::ContentBlock::MakeToolUse(
        "tu-1", "FileRead", R"({"path":"test"})");
    auto d = permissionEngine.Evaluate(block, {});
    Check(d.behavior == agent::core::PermissionBehavior::Allow,
          "FileRead should be allowed");
  }

  // Test 2: deny rule overrides
  permissionEngine.AddAlwaysDenyRule("Bash");
  {
    auto block = agent::core::ContentBlock::MakeToolUse(
        "tu-2", "Bash", R"({"command":"rm -rf /"})");
    auto d = permissionEngine.Evaluate(block, {});
    Check(d.behavior == agent::core::PermissionBehavior::Deny,
          "Bash should be denied");
  }

  // Test 3: unknown tool returns Ask
  {
    auto block = agent::core::ContentBlock::MakeToolUse(
        "tu-3", "UnknownTool", "{}");
    auto d = permissionEngine.Evaluate(block, {});
    Check(d.behavior == agent::core::PermissionBehavior::Ask,
          "Unknown tool should ask");
  }

  // Test 4: autoModeAllowlistedTools bypass classifier
  permissionEngine.AddAutoModeAllowlistedTool("SafeLs");
  {
    auto block = agent::core::ContentBlock::MakeToolUse(
        "tu-4", "SafeLs", R"({"path":"."})");
    auto d = permissionEngine.Evaluate(block, {});
    Check(d.behavior == agent::core::PermissionBehavior::Allow,
          "SafeAllowlist tool should be allowed");
  }

  // Test 5: DenialTrackingState circuit breaker
  {
    agent::core::DenialTrackingState denial;
    Check(!denial.IsCircuitBroken(), "Fresh denial state should not be broken");
    denial.RecordDenial();
    denial.RecordDenial();
    denial.RecordDenial();
    Check(denial.IsCircuitBroken(),
          "3 consecutive denials should break circuit");
  }

  // Test 6: FailClosed gate denies on classifier exception
  {
    permissionEngine.SetFailClosed(true);
    permissionEngine.SetClassifierCallback(
        [](const agent::core::ContentBlock&,
           const std::vector<agent::core::Message>&) {
          throw std::runtime_error("simulated classifier failure");
          return agent::core::PermissionDecision{};
        });
    auto block = agent::core::ContentBlock::MakeToolUse(
        "tu-5", "RiskyTool", R"({"cmd":"unsafe"})");
    auto d = permissionEngine.Evaluate(block, {});
    Check(d.behavior == agent::core::PermissionBehavior::Deny,
          "FailClosed gate should deny on classifier exception");
    permissionEngine.SetClassifierCallback(nullptr);
    permissionEngine.ResetDenialState();
  }

  // Test 7: ToolRegistry concurrency detection
  Check(toolRegistry.IsConcurrencySafe("FileRead"),
        "FileRead should be concurrency-safe");
  Check(!toolRegistry.IsConcurrencySafe("UnknownTool"),
        "Unknown tools should not be concurrency-safe");

  // Test 8: End-to-end QueryEngine run with all new infrastructure
  agent::core::AgentConfig config = agent::core::AgentConfig::FromDefaults();
  config.systemPrompt = "You are a helpful coding agent.";
  config.defaultModel = "default-model";

  agent::core::QueryEngine engine(
      toolOrchestrator, permissionEngine, modelClient, sideQueryClient,
      toolRegistry, sessionManager);
  engine.SetConfig(config);
  engine.SetMemoryIndex(&memoryIndex);
  engine.SetSubAgentManager(&subAgentManager);

  toolOrchestrator.SetToolRegistry(&toolRegistry);
  permissionEngine.AddAlwaysAllowRule("FileRead");
  watchdog.Start();
  engine.SetStabilityWatchdog(&watchdog);

  engine.SubmitUserPrompt("e2e smoke test with stability");
  engine.RunTurn();

  const auto& msgs = engine.messages();
  Check(msgs.size() >= 4, "E2E should produce >=4 messages");
  Check(msgs.back().stopReason == "end_turn",
        "Final assistant message should carry stopReason");
  Check(watchdog.IsHealthy(), "Watchdog should be healthy after turn");
  Check(sessionManager.messages().size() == msgs.size(),
        "SessionManager should stay in sync with QueryEngine messages");

  // Test 8b: successful tool execution should continue into a final assistant turn
  {
    permissionEngine.AddAlwaysAllowRule("FileWrite");
    const std::string queryLoopSuccessPath =
        "G:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent\\build\\test-session\\query-loop-success.html";
    SuccessfulToolThenFinalModelClient successfulToolModel(
        queryLoopSuccessPath);
    agent::api::SideQueryClient successfulToolSideClient(successfulToolModel);
    agent::core::QueryEngine successEngine(
        toolOrchestrator, permissionEngine, successfulToolModel,
        successfulToolSideClient, toolRegistry, sessionManager);
    successEngine.SetConfig(config);
    successEngine.SubmitUserPrompt("Create a file and then confirm completion.");
    successEngine.RunTurn();

    const auto& successMessages = successEngine.messages();
    Check(successfulToolModel.streamCallCount >= 2,
          "Successful tool path should call the model again after tool_result");
    Check(successfulToolModel.sawToolResultOnFollowup,
          "Follow-up model call should receive tool_result context");
    Check(!successMessages.empty() &&
              successMessages.back().role ==
                  agent::core::MessageRole::Assistant &&
              !successMessages.back().content.empty() &&
              successMessages.back().content[0].type ==
                  agent::core::BlockType::Text &&
              successMessages.back().content[0].asText.text.find("File is ready.") !=
                  std::string::npos,
          "Successful tool path should end with final assistant text");

    std::ifstream successFile(queryLoopSuccessPath.c_str(), std::ios::binary);
    std::ostringstream successContent;
    successContent << successFile.rdbuf();
    Check(!successContent.str().empty(),
          "Successful tool path should create the requested file");
  }

  // Test 9: SessionManager persists and restores
  sessionManager.PersistSnapshot();
  {
    agent::infra::SessionManager restoreTest(
        "G:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent\\build\\test-session");
    Check(restoreTest.RestoreFromDisk(),
          "SessionManager should restore from disk");
  }

  // Test 9b: QueryEngine memory injection reaches the model loop context
  {
    Check(memoryIndex.WriteEntrypoint("- [User role](user_role.md) - coding style"),
          "MemoryIndex should write entrypoint for QueryEngine injection");
    Check(memoryIndex.WriteTopicFile(
              "user_role.md",
              "# User role\nRemember terse review output."),
          "MemoryIndex should write topic for QueryEngine injection");
    RecordingModelClient recordingModelClient;
    agent::api::SideQueryClient recordingSideQueryClient(recordingModelClient);
    agent::core::QueryEngine memoryEngine(
        toolOrchestrator, permissionEngine, recordingModelClient,
        recordingSideQueryClient,
        toolRegistry, sessionManager);
    memoryEngine.SetConfig(config);
    memoryEngine.SetMemoryIndex(&memoryIndex);
    memoryEngine.SubmitUserPrompt("memory prompt propagation test");
    memoryEngine.RunTurn();
    Check(recordingModelClient.lastSystemPrompt.find(
              "Remember terse review output.") != std::string::npos,
          "QueryEngine should pass injected memory content into model prompt");
    Check(recordingModelClient.lastSystemPrompt.find("Memory types") !=
              std::string::npos,
          "QueryEngine should append structured memory instructions");
  }

  // Test 10: SideQueryClient delegates to ModelClient::SideQuery
  {
    agent::api::SideQueryRequest request;
    request.querySource = "unit-test";
    request.model = "test-model";
    request.systemPrompt = "validate";
    request.messages = msgs;

    const auto response = sideQueryClient.Query(request);
    Check(response.ok, "SideQuery should succeed");
    Check(!response.messages.empty(), "SideQuery should return messages");
  }

  // Test 10b: Validator runs with configured validatorModel even without env gate
  {
    SetEnvironmentVariableA("LOCALMODEL_VALIDATION_MODEL", nullptr);
    RecordingModelClient validationModelClient;
    validationModelClient.streamResponseText = "draft answer";
    validationModelClient.sideQueryResponseText =
        "<validation_json>{\"corrected_text\":\"validated answer\","
        "\"final_response_action\":\"approve\"}</validation_json>";
    agent::api::SideQueryClient validationSideClient(validationModelClient);
    agent::core::QueryEngine validationEngine(
        toolOrchestrator, permissionEngine, validationModelClient,
        validationSideClient, toolRegistry, sessionManager);
    validationEngine.SetConfig(config);
    validationEngine.SetValidatorModel("gemma-test");
    validationEngine.SubmitUserPrompt("validator no-tool path");
    validationEngine.RunTurn();
    const auto& validationMsgs = validationEngine.messages();
    Check(validationModelClient.lastSideQueryModel == "gemma-test",
          "Validator should use configured validator model");
    Check(!validationMsgs.empty(), "Validator run should produce messages");
    if (!(validationMsgs.back().content.size() == 1 &&
          validationMsgs.back().content[0].type == agent::core::BlockType::Text &&
          validationMsgs.back().content[0].asText.text == "validated answer")) {
      std::cerr << "DEBUG_VALIDATION_LAST_MESSAGE: size="
                << validationMsgs.back().content.size()
                << " firstType="
                << (validationMsgs.back().content.empty()
                        ? -1
                        : static_cast<int>(validationMsgs.back().content[0].type))
                << " text=[";
      for (const auto& block : validationMsgs.back().content) {
        if (block.type == agent::core::BlockType::Text)
          std::cerr << block.asText.text << '|';
      }
      std::cerr << "]" << std::endl;
    }
    Check(validationMsgs.back().content.size() == 1,
          "Validated assistant should contain one text block");
    Check(validationMsgs.back().content[0].type == agent::core::BlockType::Text,
          "Validated assistant should end with text");
    Check(validationMsgs.back().content[0].asText.text == "validated answer",
          "Validator should correct final no-tool response");
  }

  // Test 11: ProcessRunner can launch a small Windows process
  {
    agent::infra::ProcessRunOptions options;
    options.executable = "powershell.exe";
    options.arguments = {"-NoProfile", "-Command", "Write-Output process-ok"};
    options.timeoutMs = 5000;

    const auto result = processRunner.Run(options);
    Check(!result.spawnFailed, "ProcessRunner should spawn powershell");
    Check(!result.timedOut, "ProcessRunner should not time out");
    Check(result.exitCode == 0, "ProcessRunner exit code should be 0");
    Check(result.stdoutText.find("process-ok") != std::string::npos,
          "ProcessRunner should capture stdout");
  }

  // Test 12: MemoryIndex truncates and upserts pointers
  {
    std::string oversized;
    for (int i = 0; i < 220; ++i) {
      oversized += "- [Entry" + std::to_string(i) + "](topic.md) - hook\n";
    }
    const auto truncation = memoryIndex.TruncateEntrypointContent(oversized);
    Check(truncation.wasLineTruncated, "MemoryIndex should truncate line count");
    Check(memoryIndex.WriteEntrypoint(oversized),
          "MemoryIndex should write entrypoint");
    agent::memory::MemoryPointer pointer;
    pointer.title = "User role";
    pointer.fileName = "user_role.md";
    pointer.hook = "One-line hook";
    Check(memoryIndex.UpsertPointer(pointer),
          "MemoryIndex should upsert pointer");
    const auto pointers =
        memoryIndex.ParsePointers(memoryIndex.ReadEntrypoint());
    Check(!pointers.empty(), "MemoryIndex should parse pointers");

    Check(memoryIndex.WriteTopicFile(
              "user_role.md",
              "# User role\nBuild a production-grade local agent."),
          "MemoryIndex should write topic files");
    const std::string prompt =
        memoryIndex.BuildSystemPromptInjection({"user_role.md"});
    Check(prompt.find("MEMORY.md") != std::string::npos,
          "MemoryIndex should inject entrypoint into prompt");
    Check(prompt.find("production-grade local agent") != std::string::npos,
          "MemoryIndex should inject topic file content into prompt");
  }

  // Test 13: MemoryIndex honors shared memory override
  {
    const std::string overrideDir =
        "G:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent\\build\\shared-memory";
    _putenv_s("AGENT_SHARED_MEMORY_PATH_OVERRIDE", overrideDir.c_str());

    agent::memory::MemoryIndex sharedMemoryIndex(
        "G:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent\\build\\ignored-memory");
    Check(sharedMemoryIndex.WriteEntrypoint(
              "- [Shared note](shared.md) - from override"),
          "MemoryIndex should write entrypoint through override path");
    Check(sharedMemoryIndex.WriteTopicFile(
              "shared.md",
              "# Shared\nShared memory override content"),
          "MemoryIndex should write topic through override path");
    Check(sharedMemoryIndex.ResolveActiveMemoryDir() == overrideDir,
          "MemoryIndex should expose active override path");
    const std::string prompt = sharedMemoryIndex.BuildSystemPromptInjection();
    Check(prompt.find("AGENT_SHARED_MEMORY_PATH_OVERRIDE") != std::string::npos,
          "MemoryIndex prompt should mention override");
    Check(prompt.find("Shared memory override content") != std::string::npos,
          "MemoryIndex prompt should read shared topic content");
    _putenv_s("AGENT_SHARED_MEMORY_PATH_OVERRIDE", "");
  }

  // Test 14: McpClientManager registers, connects, parses tools/list, and maps hints
  {
    agent::mcp::McpServerConfig mcpConfig;
    mcpConfig.name = "GitHub Server";
    mcpConfig.transportType = "stdio";
    mcpConfig.endpoint = "node github-mcp.js";
    mcpClientManager.SetTransportFactory(
        [](const agent::mcp::McpServerConfig&) {
          return std::unique_ptr<agent::mcp::McpTransport>(
              new FakeMcpTransport());
        });
    Check(mcpClientManager.RegisterServer(mcpConfig),
          "McpClientManager should register server");
    Check(mcpClientManager.ConnectServer("GitHub Server"),
          "McpClientManager should connect through transport");
    Check(mcpClientManager.RefreshToolsFromTransport("GitHub Server"),
          "McpClientManager should refresh tools from transport");
    const auto tools = mcpClientManager.FetchToolsForClient("GitHub Server");
    Check(tools.size() == 2, "McpClientManager should fetch parsed tools");
    Check(tools[0].fullyQualifiedName == "mcp__github_server__create_issue",
          "McpClientManager should normalize tool names");
    Check(tools[0].destructiveHint,
          "McpClientManager should map destructiveHint");
    Check(tools[0].openWorldHint,
          "McpClientManager should map openWorldHint");
    Check(tools[1].readOnlyHint,
          "McpClientManager should map readOnlyHint");

    const auto connsAfter = mcpClientManager.connections();
    Check(connsAfter[0].type == agent::mcp::McpServerConnection::Type::Connected,
          "McpClientManager should remain connected after successful refresh");
  }

  // Test 15: McpClientManager parse failure does not kill transport
  {
    agent::mcp::McpServerConfig mcpConfig2;
    mcpConfig2.name = "Broken Server";
    mcpConfig2.transportType = "stdio";
    mcpConfig2.endpoint = "broken";

    class GarbageTransport : public agent::mcp::McpTransport {
     public:
      bool Connect(const agent::mcp::McpServerConfig&, std::string* e) override {
        if (e) e->clear();
        connected_ = true;
        return true;
      }
      void PopulateConnectionState(
          agent::mcp::McpServerConnection* connection) const override {
        if (connection != nullptr) {
          connection->capabilities.tools = true;
        }
      }
      agent::mcp::McpTransportResponse Send(const agent::mcp::McpTransportRequest&) override {
        if (!connected_) return {false, "", "not connected"};
        return {true, R"({"tools":"not_an_array"})", ""};
      }
      void Close() override { closed_ = true; connected_ = false; }
      bool closed() const { return closed_; }
     private:
      bool connected_ = false;
      bool closed_ = false;
    };

    auto* garbageTransport = new GarbageTransport();
    std::unique_ptr<agent::mcp::McpTransport> transportPtr(garbageTransport);

    agent::mcp::McpClientManager mcpMgr2;
    mcpMgr2.SetTransportFactory(
        [&transportPtr](const agent::mcp::McpServerConfig&) {
          return std::move(transportPtr);
        });
    mcpMgr2.RegisterServer(mcpConfig2);
    Check(mcpMgr2.ConnectServer("Broken Server"),
          "McpClientManager should connect broken server");
    Check(!mcpMgr2.RefreshToolsFromTransport("Broken Server"),
          "McpClientManager should return false on parse failure");
    Check(!garbageTransport->closed(),
          "McpClientManager should NOT close transport on parse failure");

    const auto conns2 = mcpMgr2.connections();
    Check(conns2[0].type != agent::mcp::McpServerConnection::Type::Failed,
          "McpClientManager should NOT mark Failed on parse failure");
  }

  // Test 15b: McpClientManager default stdio transport can speak minimal MCP
  {
    const std::string scriptPath =
        "G:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent\\build\\fake_stdio_mcp.ps1";
    std::ofstream script(scriptPath.c_str(), std::ios::binary | std::ios::trunc);
    script <<
        "[Console]::InputEncoding = [System.Text.UTF8Encoding]::new($false)\n"
        "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n"
        "$reader = [Console]::In\n"
        "function Read-McpMessage {\n"
        "  $contentLength = 0\n"
        "  while ($true) {\n"
        "    $line = $reader.ReadLine()\n"
        "    if ($null -eq $line) { return $null }\n"
        "    if ($line -like 'Content-Length:*') { $contentLength = [int]$line.Substring(15).Trim() }\n"
        "    if ($line -eq '') { break }\n"
        "  }\n"
        "  $buffer = New-Object char[] $contentLength\n"
        "  $read = 0\n"
        "  while ($read -lt $contentLength) {\n"
        "    $chunk = $reader.Read($buffer, $read, $contentLength - $read)\n"
        "    if ($chunk -le 0) { break }\n"
        "    $read += $chunk\n"
        "  }\n"
        "  return (-join $buffer)\n"
        "}\n"
        "function Send-McpMessage([string]$json) {\n"
        "  $len = [Text.Encoding]::UTF8.GetByteCount($json)\n"
        "  [Console]::Out.Write(\"Content-Length: $len`r`n`r`n$json\")\n"
        "  [Console]::Out.Flush()\n"
        "}\n"
        "$null = Read-McpMessage\n"
        "Send-McpMessage '{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":{\"tools\":{}},\"instructions\":\"stdio test\",\"serverInfo\":{\"version\":\"1.0.0\"}}}'\n"
        "$null = Read-McpMessage\n"
        "$null = Read-McpMessage\n"
        "Send-McpMessage '{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{\"name\":\"Echo Tool\",\"description\":\"Echo\",\"inputSchema\":{\"type\":\"object\"},\"annotations\":{\"readOnlyHint\":true,\"destructiveHint\":false,\"openWorldHint\":false}}]}}'\n";
    script.close();

    agent::mcp::McpClientManager realStdioManager;
    agent::mcp::McpServerConfig stdioConfig;
    stdioConfig.name = "Real Stdio";
    stdioConfig.transportType = "stdio";
    stdioConfig.endpoint =
        "powershell.exe -NoProfile -ExecutionPolicy Bypass -File " + scriptPath;
    Check(realStdioManager.RegisterServer(stdioConfig),
          "Real stdio MCP manager should register server");
    Check(realStdioManager.ConnectServer("Real Stdio"),
          "Real stdio MCP manager should connect through default transport");
    const auto connections = realStdioManager.connections();
    Check(connections[0].type == agent::mcp::McpServerConnection::Type::Connected,
          "Real stdio MCP transport should stay connected after initialize");
    Check(connections[0].capabilities.tools,
          "Real stdio MCP transport should populate tools capability");
    Check(connections[0].instructions == "stdio test",
          "Real stdio MCP transport should populate instructions");
  }

  // Test 15c: McpClientManager refreshes HTTP session lifecycle state
  {
    agent::mcp::McpClientManager httpManager;
    agent::mcp::McpServerConfig httpConfig;
    httpConfig.name = "HTTP Session Server";
    httpConfig.transportType = "http";
    httpConfig.endpoint = "http://127.0.0.1:8787/mcp";
    httpManager.SetTransportFactory(
        [](const agent::mcp::McpServerConfig&) {
          return std::unique_ptr<agent::mcp::McpTransport>(
              new SessionAwareFakeHttpTransport());
        });
    Check(httpManager.RegisterServer(httpConfig),
          "HTTP session manager should register server");
    Check(httpManager.ConnectServer("HTTP Session Server"),
          "HTTP session manager should connect");
    Check(httpManager.RefreshToolsFromTransport("HTTP Session Server"),
          "HTTP session manager should refresh tools");
    const auto httpConnections = httpManager.connections();
    Check(httpConnections[0].streamableHttp,
          "HTTP session manager should mark streamable HTTP support");
    Check(httpConnections[0].clientSessionId == "client-session-1",
          "HTTP session manager should expose client session id");
    Check(httpConnections[0].transportSessionId == "srv-session-1",
          "HTTP session manager should expose transport session id");
    Check(httpConnections[0].lastActivityUnixMs > 0,
          "HTTP session manager should expose last activity timestamp");
    Check(httpConnections[0].sessionExpiresAtUnixMs >
              httpConnections[0].lastActivityUnixMs,
          "HTTP session manager should expose session expiry");
  }

  // Test 15d: real HTTP transport resumes SSE stream with Last-Event-ID
  {
    const std::string httpScriptPath =
        "G:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent\\build\\fake_http_mcp.ps1";
    std::ofstream script(httpScriptPath.c_str(), std::ios::binary | std::ios::trunc);
    script <<
        "$listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Parse('127.0.0.1'), 8788)\n"
        "$listener.Start()\n"
        "$sessionId = 'srv-session-sse'\n"
        "$servedResume = $false\n"
        "function Read-Request($client) {\n"
        "  $stream = $client.GetStream()\n"
        "  $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::ASCII, $false, 1024, $true)\n"
        "  $requestLine = $reader.ReadLine()\n"
        "  if ($null -eq $requestLine) { return $null }\n"
        "  $parts = $requestLine.Split(' ')\n"
        "  $headers = @{}\n"
        "  while ($true) {\n"
        "    $line = $reader.ReadLine()\n"
        "    if ($null -eq $line -or $line -eq '') { break }\n"
        "    $colon = $line.IndexOf(':')\n"
        "    if ($colon -gt 0) {\n"
        "      $name = $line.Substring(0, $colon).Trim().ToLowerInvariant()\n"
        "      $value = $line.Substring($colon + 1).Trim()\n"
        "      $headers[$name] = $value\n"
        "    }\n"
        "  }\n"
        "  $body = ''\n"
        "  if ($headers.ContainsKey('content-length')) {\n"
        "    $length = [int]$headers['content-length']\n"
        "    if ($length -gt 0) {\n"
        "      $chars = New-Object char[] $length\n"
        "      $offset = 0\n"
        "      while ($offset -lt $length) {\n"
        "        $read = $reader.Read($chars, $offset, $length - $offset)\n"
        "        if ($read -le 0) { break }\n"
        "        $offset += $read\n"
        "      }\n"
        "      $body = New-Object string($chars, 0, $offset)\n"
        "    }\n"
        "  }\n"
        "  return @{ Method = $parts[0]; Path = $parts[1]; Headers = $headers; Body = $body; Stream = $stream }\n"
        "}\n"
        "function Write-Response($stream, [int]$status, [string]$reason, [string]$contentType, [string]$body, [hashtable]$headers) {\n"
        "  $payload = [System.Text.Encoding]::UTF8.GetBytes($body)\n"
        "  $builder = New-Object System.Text.StringBuilder\n"
        "  [void]$builder.Append(\"HTTP/1.1 $status $reason`r`n\")\n"
        "  if ($contentType -ne '') { [void]$builder.Append(\"Content-Type: $contentType`r`n\") }\n"
        "  foreach ($key in $headers.Keys) { [void]$builder.Append((\"{0}: {1}`r`n\" -f $key, $headers[$key])) }\n"
        "  [void]$builder.Append(\"Content-Length: $($payload.Length)`r`n\")\n"
        "  [void]$builder.Append(\"Connection: close`r`n`r`n\")\n"
        "  $headerBytes = [System.Text.Encoding]::ASCII.GetBytes($builder.ToString())\n"
        "  $stream.Write($headerBytes, 0, $headerBytes.Length)\n"
        "  if ($payload.Length -gt 0) { $stream.Write($payload, 0, $payload.Length) }\n"
        "  $stream.Flush()\n"
        "}\n"
        "for ($i = 0; $i -lt 8; $i++) {\n"
        "  $client = $listener.AcceptTcpClient()\n"
        "  try {\n"
        "    $req = Read-Request $client\n"
        "    if ($null -eq $req) { continue }\n"
        "    if ($req.Path -ne '/mcp') { Write-Response $req.Stream 404 'Not Found' 'application/json' '' @{}; continue }\n"
        "    if ($req.Method -eq 'POST') {\n"
        "      if ($req.Body -like '*\"method\":\"initialize\"*') {\n"
        "        Write-Response $req.Stream 200 'OK' 'application/json' '{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":{\"tools\":{}},\"instructions\":\"http sse\",\"serverInfo\":{\"version\":\"2.0.0\"}}}' @{ 'MCP-Session-Id' = $sessionId }\n"
        "        continue\n"
        "      }\n"
        "      if ($req.Body -like '*\"notifications/initialized\"*') {\n"
        "        Write-Response $req.Stream 202 'Accepted' '' '' @{ 'MCP-Session-Id' = $sessionId }\n"
        "        continue\n"
        "      }\n"
        "      if ($req.Body -like '*\"tools/list\"*') {\n"
        "        Write-Response $req.Stream 200 'OK' 'text/event-stream' ('id: evt-1' + \"`nretry: 10`ndata:`n`n\") @{ 'MCP-Session-Id' = $sessionId }\n"
        "        continue\n"
        "      }\n"
        "    }\n"
        "    if ($req.Method -eq 'GET') {\n"
        "      if ($req.Headers['mcp-session-id'] -ne $sessionId) { Write-Response $req.Stream 404 'Not Found' 'application/json' '' @{}; continue }\n"
        "      if ($req.Headers['last-event-id'] -eq 'evt-1' -and -not $servedResume) {\n"
        "        $servedResume = $true\n"
        "        Write-Response $req.Stream 200 'OK' 'text/event-stream' ('id: evt-2' + \"`ndata: {\"\"jsonrpc\"\":\"\"2.0\"\",\"\"id\"\":2,\"\"result\"\":{\"\"tools\"\":[{\"\"name\"\":\"\"Sse Echo\"\",\"\"description\"\":\"\"Echo over SSE\"\",\"\"inputSchema\"\":{\"\"type\"\":\"\"object\"\"},\"\"annotations\"\":{\"\"readOnlyHint\"\":true,\"\"destructiveHint\"\":false,\"\"openWorldHint\"\":false}}]}}`n`n\") @{ 'MCP-Session-Id' = $sessionId }\n"
        "        break\n"
        "      }\n"
        "      Write-Response $req.Stream 200 'OK' 'text/event-stream' ('id: evt-idle' + \"`ndata:`n`n\") @{ 'MCP-Session-Id' = $sessionId }\n"
        "      continue\n"
        "    }\n"
        "    Write-Response $req.Stream 405 'Method Not Allowed' 'application/json' '' @{}\n"
        "  } finally {\n"
        "    $client.Close()\n"
        "  }\n"
        "}\n"
        "$listener.Stop()\n";
    script.close();

    SpawnedProcess httpServer = StartBackgroundProcess(
        "powershell.exe",
        {"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", httpScriptPath});
    Check(httpServer.process != nullptr,
          "Real HTTP SSE test server should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    agent::mcp::McpClientManager realHttpManager;
    agent::mcp::McpServerConfig realHttpConfig;
    realHttpConfig.name = "Real HTTP SSE";
    realHttpConfig.transportType = "http";
    realHttpConfig.endpoint = "http://127.0.0.1:8788/mcp";
    Check(realHttpManager.RegisterServer(realHttpConfig),
          "Real HTTP SSE manager should register server");
    Check(realHttpManager.ConnectServer("Real HTTP SSE"),
          "Real HTTP SSE manager should connect");
    Check(realHttpManager.RefreshToolsFromTransport("Real HTTP SSE"),
          "Real HTTP SSE manager should resume POST stream via GET");
    const auto realHttpConnections = realHttpManager.connections();
    Check(realHttpConnections[0].transportSessionId == "srv-session-sse",
          "Real HTTP SSE manager should persist MCP session id");
    Check(realHttpConnections[0].reconnectCount >= 1,
          "Real HTTP SSE manager should record SSE reconnect attempts");
    const auto realHttpTools =
        realHttpManager.FetchToolsForClient("Real HTTP SSE");
    Check(realHttpTools.size() == 1 &&
              realHttpTools[0].toolName == "Sse Echo",
          "Real HTTP SSE manager should decode resumed SSE tool payload");
    StopBackgroundProcess(&httpServer);
  }

  // Test 16: SubAgentManager builds fork messages, worktree notice, and lifecycle
  {
    agent::core::Message assistant;
    assistant.role = agent::core::MessageRole::Assistant;
    assistant.content.push_back(
        agent::core::ContentBlock::MakeText("working"));
    assistant.content.push_back(agent::core::ContentBlock::MakeToolUse(
        "fork-tu-1", "FileRead", R"({"path":"README.md"})"));

    Check(subAgentManager.IsForkCandidate(assistant),
          "SubAgentManager should detect fork candidate");
    const auto forked = subAgentManager.BuildForkedMessages(
        "Inspect the read-only file path flow.", assistant,
        "C:\\repo", "C:\\repo\\.worktrees\\child");
    Check(forked.size() == 2, "SubAgentManager should emit 2 forked messages");
    Check(forked[1].content.size() >= 2,
          "Forked user message should contain tool_result and directive");
    Check(forked[1].content[0].asToolResult.content ==
              agent::agents::SubAgentManager::kForkPlaceholderResult,
          "SubAgentManager should use stable placeholder result");
    Check(forked[1].content.back().asText.text.find("<worktree_notice>") !=
              std::string::npos,
          "SubAgentManager should inject worktree notice");

    const auto exactTools = subAgentManager.ApplyExactToolWhitelist(
        {"FileRead", "Grep", "Bash"}, {"FileRead", "Bash"});
    Check(exactTools.size() == 2,
          "SubAgentManager should preserve only allowed tools");

    agent::agents::SubAgentTask task;
    task.prompt = "Analyze current module health.";
    task.parentCwd = "C:\\repo";
    task.worktreeCwd = "C:\\repo\\.worktrees\\child";
    task.runInBackground = true;
    const std::string taskId = subAgentManager.StartSubTask(task);
    Check(!taskId.empty(), "SubAgentManager should create lifecycle record");
    Check(subAgentManager.UpdateTaskState(
              taskId, agent::agents::SubAgentTaskState::Completed, "done"),
          "SubAgentManager should update task state");
    agent::agents::SubAgentTaskLifecycle lifecycle;
    Check(subAgentManager.TryGetTask(taskId, &lifecycle),
          "SubAgentManager should retrieve lifecycle");
    Check(lifecycle.summary == "done",
          "SubAgentManager should store task summary");
    Check(!lifecycle.worktreeNotice.empty(),
          "SubAgentManager should persist worktree notice");
  }

  // Test 17: SubAgentManager IsInForkChild detects recursive fork
  {
    agent::core::Message forkUserMsg;
    forkUserMsg.role = agent::core::MessageRole::User;
    forkUserMsg.content.push_back(agent::core::ContentBlock::MakeText(
        "Some text before <fork_boilerplate> directive here."));

    std::vector<agent::core::Message> history = {forkUserMsg};
    Check(subAgentManager.IsInForkChild(history),
          "SubAgentManager should detect fork boilerplate in history");

    Check(!subAgentManager.IsInForkChild({}),
          "SubAgentManager should return false for empty history");
  }

  // Test 18: SubAgentManager enhanced directive has output format
  {
    agent::core::Message assistant;
    assistant.role = agent::core::MessageRole::Assistant;
    assistant.content.push_back(agent::core::ContentBlock::MakeToolUse(
        "fork-tu-2", "FileRead", R"({"path":"main.cpp"})"));

    const auto forked = subAgentManager.BuildForkedMessages(
        "Analyze the module structure.", assistant);
    const std::string& directive =
        forked[1].content.back().asText.text;
    Check(directive.find("RULES (non-negotiable):") != std::string::npos,
          "SubAgentManager directive should contain detailed rules header");
    Check(directive.find("Output format") != std::string::npos,
          "SubAgentManager directive should contain output format section");
    Check(directive.find("Key files:") != std::string::npos,
          "SubAgentManager directive should specify Key files field");
    Check(directive.find("Files changed:") != std::string::npos,
          "SubAgentManager directive should specify Files changed field");
  }

  // Test 19: MemoryIndex enhanced prompt has memory types and how-to-save
  {
    Check(memoryIndex.WriteTopicFile(
              "user_role.md",
              "# User role\nPreferred collaboration style: concise feedback."),
          "MemoryIndex should write topic for prompt test");
    const std::string prompt = memoryIndex.BuildSystemPromptInjection(
        {"user_role.md"}, {"Extra custom guideline."});
    Check(prompt.find("Memory types") != std::string::npos,
          "Enhanced prompt should contain memory types section");
    Check(prompt.find("How to save memories") != std::string::npos,
          "Enhanced prompt should contain how-to-save section");
    Check(prompt.find("Step 1") != std::string::npos,
          "Enhanced prompt should describe two-step save process");
    Check(prompt.find("When to access memory") != std::string::npos,
          "Enhanced prompt should contain when-to-access section");
    Check(prompt.find("Searching past context") != std::string::npos,
          "Enhanced prompt should contain searching section");
    Check(prompt.find("Extra custom guideline.") != std::string::npos,
          "Enhanced prompt should include extra guidelines");
    Check(prompt.find("Preferred collaboration style") != std::string::npos,
          "Enhanced prompt should inject topic file content");
  }

  // Test 20: SessionManager persists and restores subtask lifecycle
  {
    agent::agents::SubAgentTask task;
    task.prompt = "Persist me.";
    task.runInBackground = true;
    const std::string taskId = subAgentManager.StartSubTask(task);
    Check(subAgentManager.UpdateTaskState(
              taskId, agent::agents::SubAgentTaskState::Running, "active"),
          "Subtask should move into running state");
    sessionManager.SetSubAgentTasks(subAgentManager.ListTasks());
    sessionManager.PersistSnapshot();

    agent::infra::SessionManager restoreTasks(
        "G:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent\\build\\test-session");
    Check(restoreTasks.RestoreFromDisk(),
          "SessionManager should restore snapshot with tasks");
    const auto restoredTasks = restoreTasks.subAgentTasks();
    Check(!restoredTasks.empty(),
          "SessionManager should restore persisted subtask lifecycle");
    Check(restoredTasks.back().taskId == taskId,
          "Restored subtask lifecycle should keep task id");
  }

  // Test 20b: SessionManager writes binary snapshot and restores executors
  {
    agent::agents::SubAgentExecutorSlot executor;
    executor.executorId = "exec-a";
    executor.hostName = "local-a";
    executor.maxParallelTasks = 2;
    executor.runningTasks = 1;
    executor.weight = 3;
    executor.healthy = true;
    executor.supportsCheckpointResume = true;
    executor.lastHeartbeatUnixMs = 1715932800100LL;
    sessionManager.SetSubAgentExecutors({executor});
    sessionManager.PersistSnapshot();

    std::ifstream binary(sessionManager.SnapshotPath().c_str(),
                         std::ios::binary);
    std::ostringstream snapshotBytes;
    snapshotBytes << binary.rdbuf();
    Check(sessionManager.SnapshotPath().find("snapshot.pb") != std::string::npos,
          "SessionManager should switch primary snapshot to protobuf file");
    Check(!snapshotBytes.str().empty(),
          "SessionManager should write protobuf snapshot bytes");

    agent::infra::SessionManager executorRestore(
        "G:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent\\build\\test-session");
    Check(executorRestore.RestoreFromDisk(),
          "SessionManager should restore binary snapshot");
    const auto restoredExecutors = executorRestore.subAgentExecutors();
    Check(restoredExecutors.size() == 1,
          "SessionManager should restore persisted executors");
    Check(restoredExecutors[0].executorId == "exec-a",
          "SessionManager should restore executor id");
    Check(restoredExecutors[0].weight == 3,
          "SessionManager should restore executor weight");
  }

  // Test 20c: SessionManager keeps backward compatibility with legacy text snapshot
  {
    agent::infra::SessionManager legacySession(
        "G:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent\\build\\legacy-session");
    legacySession.PersistSnapshot();
    std::ofstream legacyBin(legacySession.SnapshotPath().c_str(),
                            std::ios::binary | std::ios::trunc);
    legacyBin.close();
    std::ofstream legacyOut(legacySession.LegacySnapshotPath().c_str(),
                            std::ios::binary | std::ios::trunc);
    legacyOut << "=== SESSION SNAPSHOT ===\n";
    legacyOut << "session_id=legacy-id\n";
    legacyOut << "timestamp=20260517T120000\n";
    legacyOut << "turn_count=7\n";
    legacyOut << "message_count=1\n";
    legacyOut << "subtask_count=0\n";
    legacyOut << "message.0.role=user\n";
    legacyOut << "message.0.uuid=legacy-msg\n";
    legacyOut << "message.0.is_meta=0\n";
    legacyOut << "message.0.stop_reason=\n";
    legacyOut << "message.0.is_api_error=0\n";
    legacyOut << "message.0.usage.input_tokens=0\n";
    legacyOut << "message.0.usage.output_tokens=0\n";
    legacyOut << "message.0.usage.cache_read=0\n";
    legacyOut << "message.0.usage.cache_create=0\n";
    legacyOut << "message.0.block_count=1\n";
    legacyOut << "message.0.block.0.type=text\n";
    legacyOut << "message.0.block.0.text=legacy restore path\n";
    legacyOut << "=== END ===\n";
    legacyOut.close();
    Check(legacySession.RestoreFromDisk(),
          "SessionManager should restore legacy text snapshot");
    Check(legacySession.metadata().turnCount == 7,
          "SessionManager should keep legacy turn count");
    Check(!legacySession.messages().empty() &&
              legacySession.messages()[0].content[0].asText.text ==
                  "legacy restore path",
          "SessionManager should keep legacy message payload");
  }

  // Test 20d: SubAgentManager executor-aware reschedule restores checkpointed work
  {
    agent::agents::SubAgentManager executorManager;
    agent::agents::SubAgentExecutorSlot execA;
    execA.executorId = "exec-a";
    execA.hostName = "node-a";
    execA.maxParallelTasks = 1;
    execA.weight = 1;
    execA.healthy = true;
    execA.supportsCheckpointResume = true;
    execA.lastHeartbeatUnixMs = 1715932801000LL;
    agent::agents::SubAgentExecutorSlot execB = execA;
    execB.executorId = "exec-b";
    execB.hostName = "node-b";
    execB.weight = 5;
    execB.lastHeartbeatUnixMs = 1715932802000LL;
    executorManager.SetExecutors({execA, execB});

    agent::agents::SubAgentTask highTask;
    highTask.prompt = "Recover me with checkpoint.";
    highTask.runInBackground = true;
    highTask.priority = 90;
    const std::string highTaskId = executorManager.StartSubTask(highTask);
    agent::agents::SubAgentTaskCheckpoint checkpoint;
    checkpoint.checkpointId = "cp-1";
    checkpoint.resumeCursor = "cursor-42";
    checkpoint.savedAtUnixMs = 1715932803000LL;
    checkpoint.resumable = true;
    Check(executorManager.SaveCheckpoint(highTaskId, checkpoint),
          "SubAgentManager should persist checkpoint");
    agent::agents::SubAgentTaskLifecycle assignedBeforeFailure;
    Check(executorManager.TryGetTask(highTaskId, &assignedBeforeFailure),
          "SubAgentManager should expose initial executor assignment");
    Check(executorManager.RecordExecutorFailure(
              assignedBeforeFailure.assignedExecutorId,
              "worker lost network heartbeat"),
          "SubAgentManager should record executor failure");
    agent::agents::SubAgentTaskLifecycle recoveredLifecycle;
    Check(executorManager.TryGetTask(highTaskId, &recoveredLifecycle),
          "SubAgentManager should find rescheduled task");
    Check(recoveredLifecycle.state == agent::agents::SubAgentTaskState::Running,
          "SubAgentManager should reschedule task to another executor");
    Check(recoveredLifecycle.assignedExecutorId !=
              assignedBeforeFailure.assignedExecutorId,
          "SubAgentManager should rebalance onto healthy executor");
    Check(recoveredLifecycle.summary.find("checkpoint") != std::string::npos,
          "SubAgentManager should preserve checkpoint-aware summary");
  }

  // Test 20e: real worker pool preempts low priority work and later resumes it
  {
    agent::agents::SubAgentManager workerPoolManager;
    workerPoolManager.SetWorkerExecutablePath(
        executableDir + "\\agent_subagent_worker.exe");
    workerPoolManager.SetWorkerRuntimeRoot(
        executableDir + "\\subagent-runtime-preempt");
    agent::agents::SubAgentExecutorSlot executor;
    executor.executorId = "solo-exec";
    executor.hostName = "solo";
    executor.maxParallelTasks = 1;
    executor.healthy = true;
    executor.supportsCheckpointResume = true;
    workerPoolManager.SetExecutors({executor});

    agent::agents::SubAgentTask slowTask;
    slowTask.prompt = std::string(200, 'x');
    slowTask.priority = 10;
    slowTask.runInBackground = true;
    const std::string slowTaskId = workerPoolManager.StartSubTask(slowTask);
    std::this_thread::sleep_for(std::chrono::milliseconds(160));

    agent::agents::SubAgentTask urgentTask;
    urgentTask.prompt = "urgent-preempt";
    urgentTask.priority = 99;
    urgentTask.runInBackground = true;
    const std::string urgentTaskId = workerPoolManager.StartSubTask(urgentTask);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    agent::agents::SubAgentTaskLifecycle slowLifecycle;
    agent::agents::SubAgentTaskLifecycle urgentLifecycle;
    Check(workerPoolManager.TryGetTask(slowTaskId, &slowLifecycle),
          "Worker pool should expose slow task lifecycle");
    Check(workerPoolManager.TryGetTask(urgentTaskId, &urgentLifecycle),
          "Worker pool should expose urgent task lifecycle");
    Check(urgentLifecycle.state == agent::agents::SubAgentTaskState::Running,
          "Worker pool should run urgent task after preemption");
    Check(slowLifecycle.state == agent::agents::SubAgentTaskState::Pending,
          "Worker pool should preempt low priority task");
    Check(slowLifecycle.checkpoint.resumable &&
              !slowLifecycle.checkpoint.resumeCursor.empty(),
          "Worker pool should preserve checkpoint on preemption");

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    Check(workerPoolManager.TryGetTask(slowTaskId, &slowLifecycle),
          "Worker pool should keep slow task after urgent completion");
    Check(slowLifecycle.state == agent::agents::SubAgentTaskState::Running ||
              slowLifecycle.state == agent::agents::SubAgentTaskState::Completed,
          "Worker pool should reschedule low priority task after capacity returns");
  }

  // Test 21: Watchdog recovery rehydrates subtask lifecycle through QueryEngine
  {
    agent::agents::SubAgentManager recoverySubAgentManager;
    agent::infra::SessionManager recoverySessionManager(
        "G:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent\\build\\recovery-session");
    agent::infra::StabilityWatchdog recoveryWatchdog(
        agent::infra::StabilityConfig{25, 50, 10, 1024 * 1024, 2000, 5, 100, true});
    agent::memory::MemoryIndex recoveryMemoryIndex(
        "G:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent\\build\\recovery-memory");

    agent::agents::SubAgentExecutorSlot recoveryExecutor;
    recoveryExecutor.executorId = "recovery-exec";
    recoveryExecutor.hostName = "node-recovery";
    recoveryExecutor.maxParallelTasks = 1;
    recoveryExecutor.healthy = true;
    recoveryExecutor.supportsCheckpointResume = true;
    recoveryExecutor.lastHeartbeatUnixMs = 1715932804000LL;
    recoverySubAgentManager.SetExecutors({recoveryExecutor});

    agent::agents::SubAgentTask task;
    task.prompt = "Recover me.";
    task.runInBackground = true;
    const std::string taskId = recoverySubAgentManager.StartSubTask(task);
    Check(recoverySubAgentManager.UpdateTaskState(
              taskId, agent::agents::SubAgentTaskState::Running, "before-crash"),
          "Recovery subtask should enter running state before recovery");

    agent::core::QueryEngine recoveryEngine(
        toolOrchestrator, permissionEngine, modelClient, sideQueryClient,
        toolRegistry, recoverySessionManager);
    recoveryEngine.SetConfig(config);
    recoveryEngine.SetMemoryIndex(&recoveryMemoryIndex);
    recoveryEngine.SetSubAgentManager(&recoverySubAgentManager);
    recoveryWatchdog.Start();
    recoveryEngine.SetStabilityWatchdog(&recoveryWatchdog);
    recoverySessionManager.SetSubAgentTasks(recoverySubAgentManager.ListTasks());
    recoverySessionManager.SetSubAgentExecutors(
        recoverySubAgentManager.ListExecutors());
    recoverySessionManager.PersistSnapshot();

    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    const auto recoveredTasks = recoverySubAgentManager.ListTasks();
    Check(!recoveredTasks.empty(),
          "Recovery watchdog should preserve subtask list");
    Check(recoveredTasks[0].state == agent::agents::SubAgentTaskState::Running,
          "Recovery watchdog should reschedule recovered task through executor layer");
    Check(recoveredTasks[0].assignedExecutorId == "recovery-exec",
          "Recovery watchdog should keep executor-aware reassignment");
    Check(recoveryWatchdog.metrics().runningSubTasks >= 1,
          "Recovery watchdog should report running subtask metrics after reschedule");
    recoveryWatchdog.Stop();
  }

  // Test 22: FileWrite end-to-end through ToolOrchestrator with permission engine
  {
    permissionEngine.ResetDenialState();
    permissionEngine.AddAlwaysAllowRule("FileWrite");

    std::string testDir = "build\\smoke-filewrite";
    CreateDirectoryA(testDir.c_str(), nullptr);

    std::string filePath = testDir + "\\hello.cpp";
    std::string content =
        "#include <iostream>\n"
        "\n"
        "int main() {\n"
        "    std::cout << \"Hello from FileWrite smoke test!\" << std::endl;\n"
        "    return 0;\n"
        "}\n";

    nlohmann::json fwJson;
    fwJson["file_path"] = filePath;
    fwJson["content"] = content;

    agent::core::ContentBlock fwBlock =
        agent::core::ContentBlock::MakeToolUse(
            "fw-smoke-1", "FileWrite", fwJson.dump());
    std::vector<agent::core::ContentBlock> fwBlocks = {fwBlock};

    auto canUseTool = permissionEngine.BuildCanUseTool();
    auto result = toolOrchestrator.Execute(
        fwBlocks, canUseTool, {});

    Check(result.errorCount == 0,
          "FileWrite smoke: should not error");
    Check(!result.userMessages.empty(),
          "FileWrite smoke: should produce result messages");

    bool foundCreateMsg = false;
    for (const auto& msg : result.userMessages) {
      for (const auto& block : msg.content) {
        if (block.type == agent::core::BlockType::ToolResult &&
            block.asToolResult.content.find("Created file") != std::string::npos) {
          foundCreateMsg = true;
        }
      }
    }
    Check(foundCreateMsg,
          "FileWrite smoke: result should contain 'Created file'");

    DWORD attrs = GetFileAttributesA(filePath.c_str());
    Check(attrs != INVALID_FILE_ATTRIBUTES,
          "FileWrite smoke: output file should exist on disk");
    Check(!(attrs & FILE_ATTRIBUTE_DIRECTORY),
          "FileWrite smoke: output should be a file, not directory");

    std::ifstream verify(filePath, std::ios::binary);
    std::string actualContent((std::istreambuf_iterator<char>(verify)),
                               std::istreambuf_iterator<char>());
    verify.close();
    Check(actualContent == content,
          "FileWrite smoke: disk content should match written content");

    DeleteFileA(filePath.c_str());
    RemoveDirectoryA(testDir.c_str());
  }

  // Test 23: FileWrite permissions — deny rule blocks write (without polluting global rules)
  {
    permissionEngine.ResetDenialState();

    agent::core::ContentBlock fwBlock =
        agent::core::ContentBlock::MakeToolUse(
            "fw-smoke-deny", "FileWrite",
            R"({"file_path":"build\should-not-exist.txt","content":"blocked"})");
    std::vector<agent::core::ContentBlock> fwBlocks = {fwBlock};

    auto explicitDeny = [](const agent::core::ContentBlock&,
                           const std::vector<agent::core::Message>&) {
      agent::core::PermissionDecision d;
      d.behavior = agent::core::PermissionBehavior::Deny;
      d.reason = "test deny rule";
      return d;
    };

    auto result = toolOrchestrator.Execute(
        fwBlocks, explicitDeny, {});

    Check(result.deniedCount > 0,
          "FileWrite smoke deny: should be denied by custom canUseTool");
    DWORD attrs = GetFileAttributesA("build\\should-not-exist.txt");
    Check(attrs == INVALID_FILE_ATTRIBUTES,
          "FileWrite smoke deny: file should NOT exist on disk");
  }

  // Test 24: FileWrite multi-file project through orchestrator (complex benchmark)
  {
    permissionEngine.ResetDenialState();
    permissionEngine.AddAlwaysAllowRule("FileWrite");
    permissionEngine.AddAlwaysAllowRule("FileRead");
    permissionEngine.AddAlwaysAllowRule("Bash");

    std::string projDir = "build\\smoke-fw-project";
    CreateDirectoryA(projDir.c_str(), nullptr);

    struct FileSpec {
      std::string path;
      std::string content;
    };

    std::vector<FileSpec> projectFiles = {
      {projDir + "\\CMakeLists.txt",
       "cmake_minimum_required(VERSION 3.20)\n"
       "project(smoke_bench LANGUAGES CXX)\n"
       "add_executable(smoke_bench main.cpp utils.cpp)\n"},
      {projDir + "\\main.cpp",
       "#include \"utils.h\"\n"
       "#include <iostream>\n"
       "int main() { std::cout << add(2, 3) << std::endl; return 0; }\n"},
      {projDir + "\\utils.h",
       "#pragma once\n"
       "int add(int a, int b);\n"},
      {projDir + "\\utils.cpp",
       "#include \"utils.h\"\n"
       "int add(int a, int b) { return a + b; }\n"},
      {projDir + "\\README.md",
       "# Smoke Bench\n"
       "Auto-generated project via FileWrite tool.\n"
       "Build: mkdir build && cd build && cmake .. && cmake --build .\n"},
    };

    auto canUseTool = permissionEngine.BuildCanUseTool();
    int writtenCount = 0;

    for (const auto& spec : projectFiles) {
      nlohmann::json fwJson;
      fwJson["file_path"] = spec.path;
      fwJson["content"] = spec.content;

      std::vector<agent::core::ContentBlock> blocks;
      blocks.push_back(agent::core::ContentBlock::MakeToolUse(
          "fw-proj-" + std::to_string(writtenCount),
          "FileWrite", fwJson.dump()));

      auto result = toolOrchestrator.Execute(blocks, canUseTool, {});
      if (result.errorCount == 0) {
        ++writtenCount;
      }
    }

    Check(writtenCount == static_cast<int>(projectFiles.size()),
          "FileWrite project: all files should be written without error");

    for (const auto& spec : projectFiles) {
      DWORD attrs = GetFileAttributesA(spec.path.c_str());
      Check(attrs != INVALID_FILE_ATTRIBUTES,
            ("FileWrite project: file should exist: " + spec.path).c_str());

      std::ifstream verify(spec.path, std::ios::binary);
      std::string actualContent((std::istreambuf_iterator<char>(verify)),
                                 std::istreambuf_iterator<char>());
      verify.close();
      Check(actualContent == spec.content,
            ("FileWrite project: content match: " + spec.path).c_str());
    }

    for (const auto& spec : projectFiles) {
      DeleteFileA(spec.path.c_str());
    }
    RemoveDirectoryA(projDir.c_str());
  }

  // Test 25: FileWrite edge-case — relative path, deep directory creation via Bash+FileWrite
  {
    permissionEngine.ResetDenialState();
    permissionEngine.AddAlwaysAllowRule("FileWrite");
    permissionEngine.AddAlwaysAllowRule("Bash");

    std::string deepDir = "build\\smoke-fw-deep\\sub\\nested";
    std::string deepFilePath = deepDir + "\\deep_file.txt";
    std::string deepContent = "Deep nested content.\n";

    std::vector<agent::core::ContentBlock> bashBlocks;
    bashBlocks.push_back(agent::core::ContentBlock::MakeToolUse(
        "fw-deep-mkdir", "Bash",
        R"({"command":"mkdir build\\smoke-fw-deep\\sub\\nested 2>nul"})"));
    auto canUseTool = permissionEngine.BuildCanUseTool();
    toolOrchestrator.Execute(bashBlocks, canUseTool, {});

    CreateDirectoryA("build\\smoke-fw-deep", nullptr);
    CreateDirectoryA("build\\smoke-fw-deep\\sub", nullptr);
    CreateDirectoryA(deepDir.c_str(), nullptr);

    nlohmann::json fwDeepJson;
    fwDeepJson["file_path"] = deepFilePath;
    fwDeepJson["content"] = deepContent;

    std::vector<agent::core::ContentBlock> fwBlocks;
    fwBlocks.push_back(agent::core::ContentBlock::MakeToolUse(
        "fw-deep-1", "FileWrite", fwDeepJson.dump()));

    auto fwResult = toolOrchestrator.Execute(fwBlocks, canUseTool, {});
    Check(fwResult.errorCount == 0,
          "FileWrite deep path: should write file in nested directory");

    DWORD attrs = GetFileAttributesA(deepFilePath.c_str());
    Check(attrs != INVALID_FILE_ATTRIBUTES,
          "FileWrite deep path: file should exist in nested directory");

    DeleteFileA(deepFilePath.c_str());
    RemoveDirectoryA(deepDir.c_str());
    RemoveDirectoryA("build\\smoke-fw-deep\\sub");
    RemoveDirectoryA("build\\smoke-fw-deep");
  }

  watchdog.Stop();

  if (failures > 0) {
    std::cerr << failures << " test(s) failed." << std::endl;
    return 1;
  }
  std::cout << "All tests passed." << std::endl;
  return 0;
}
