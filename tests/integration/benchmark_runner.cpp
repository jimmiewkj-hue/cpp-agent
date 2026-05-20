#include "agents/SubAgentManager.h"
#include "api/ModelClient.h"
#include "api/SideQueryClient.h"
#include "core/QueryEngine.h"
#include "core/StateTypes.h"
#include "infra/SessionManager.h"
#include "memory/MemoryIndex.h"
#include "permissions/PermissionEngine.h"
#include "third_party/nlohmann_json.hpp"
#include "tools/ToolOrchestrator.h"
#include "tools/ToolRegistry.h"

#include <windows.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

std::string ParentPath(const std::string& path) {
  const std::size_t pos = path.find_last_of("\\/");
  if (pos == std::string::npos) return std::string();
  if (pos == 0) return path.substr(0, 1);
  if (pos == 2 && path.size() >= 3 && path[1] == ':')
    return path.substr(0, 3);
  return path.substr(0, pos);
}

std::string JoinPath(const std::string& lhs, const std::string& rhs) {
  if (lhs.empty()) return rhs;
  if (rhs.empty()) return lhs;
  const char last = lhs[lhs.size() - 1];
  if (last == '\\' || last == '/') return lhs + rhs;
  return lhs + "\\" + rhs;
}

bool FileExists(const std::string& path) {
  DWORD attrs = GetFileAttributesA(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::string& path) {
  DWORD attrs = GetFileAttributesA(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool EnsureDirectoryRecursive(const std::string& path) {
  if (path.empty()) return false;
  if (DirectoryExists(path)) return true;
  const std::string parent = ParentPath(path);
  if (!parent.empty() && parent != path && !DirectoryExists(parent)) {
    if (!EnsureDirectoryRecursive(parent)) return false;
  }
  if (CreateDirectoryA(path.c_str(), nullptr)) return true;
  return GetLastError() == ERROR_ALREADY_EXISTS;
}

void WriteTextFile(const std::string& path, const std::string& content) {
  EnsureDirectoryRecursive(ParentPath(path));
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
}

std::string ReadTextFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::string();
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

std::string DiscoverProjectRoot() {
  char buffer[MAX_PATH] = {0};
  DWORD length = GetCurrentDirectoryA(MAX_PATH, buffer);
  std::string cursor =
      (length == 0 || length >= MAX_PATH) ? "." : std::string(buffer, length);
  while (!cursor.empty()) {
    if (FileExists(JoinPath(cursor, "CMakeLists.txt")) &&
        FileExists(JoinPath(JoinPath(JoinPath(cursor, "src"), "app"), "main.cpp")))
      return cursor;
    const std::string parent = ParentPath(cursor);
    if (parent.empty() || parent == cursor) break;
    cursor = parent;
  }
  return ".";
}

std::string ExtractText(const std::vector<agent::core::Message>& messages) {
  std::ostringstream out;
  for (const auto& msg : messages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::Text) {
        out << block.asText.text << "\n";
      } else if (block.type == agent::core::BlockType::ToolResult) {
        out << block.asToolResult.content << "\n";
      }
    }
  }
  return out.str();
}

bool MessagesContain(const std::vector<agent::core::Message>& messages,
                     const std::string& needle) {
  return ExtractText(messages).find(needle) != std::string::npos;
}

struct ScopedCurrentDirectory {
  explicit ScopedCurrentDirectory(const std::string& nextDir) {
    char buffer[MAX_PATH] = {0};
    DWORD length = GetCurrentDirectoryA(MAX_PATH, buffer);
    if (length != 0 && length < MAX_PATH) {
      previousDir.assign(buffer, length);
    }
    SetCurrentDirectoryA(nextDir.c_str());
  }

  ~ScopedCurrentDirectory() {
    if (!previousDir.empty()) {
      SetCurrentDirectoryA(previousDir.c_str());
    }
  }

  std::string previousDir;
};

struct BenchmarkResult {
  std::string id;
  std::string category;
  bool passed = false;
  long long elapsedMs = 0;
  std::string evidence;
  std::string artifactPath;
};

struct EngineBundle {
  EngineBundle(const std::string& caseDir,
               const std::string& endpoint,
               const std::string& mainModel,
               const std::string& validatorModel,
               const std::string& fallbackModel)
      : caseDir(caseDir),
        sessionDir(JoinPath(caseDir, "session")),
        memoryDir(JoinPath(caseDir, "memory")),
        llmConfig(BuildConfig(endpoint, mainModel, validatorModel, fallbackModel)),
        modelClient(llmConfig),
        sideQueryClient(modelClient),
        sessionManager(EnsureDir(sessionDir)),
        memoryIndex(EnsureDir(memoryDir)),
        engine(toolOrchestrator, permissionEngine, modelClient, sideQueryClient,
               toolRegistry, sessionManager) {
    EnsureDirectoryRecursive(caseDir);
    const std::vector<agent::tools::ToolSchema> baseTools =
        agent::tools::ToolRegistry::GetAllBaseTools();
    for (const auto& tool : baseTools) {
      toolRegistry.RegisterTool(tool);
      permissionEngine.AddAlwaysAllowRule(tool.name);
      permissionEngine.AddAutoModeAllowlistedTool(tool.name);
    }
    toolOrchestrator.SetToolRegistry(&toolRegistry);
    toolOrchestrator.SetSubAgentManager(&subAgentManager);
    toolOrchestrator.SetWorkspaceRoot(caseDir);
    subAgentManager.SetMemoryIndex(&memoryIndex);

    agent::core::AgentConfig config = agent::core::AgentConfig::FromDefaults();
    config.systemPrompt =
        "You are a helpful coding agent. The trusted workspace root is `" +
        caseDir +
        "`. Treat relative paths as inside this workspace, keep generated "
        "project files in this workspace, and use absolute paths only for "
        "external file references.";
    config.defaultModel = mainModel;
    config.memoryRoot = memoryDir;
    config.sessionDir = sessionDir;
    engine.SetConfig(config);
    engine.SetModel(mainModel);
    engine.SetFallbackModel(fallbackModel);
    engine.SetValidatorModel(validatorModel);
    engine.SetMemoryIndex(&memoryIndex);
    engine.SetSubAgentManager(&subAgentManager);
    engine.SetSessionDir(sessionDir);
    engine.SetMaxTurns(30);
  }

  static std::string EnsureDir(const std::string& path) {
    EnsureDirectoryRecursive(path);
    return path;
  }

  static agent::core::LlmConfig BuildConfig(const std::string& endpoint,
                                            const std::string& mainModel,
                                            const std::string& validatorModel,
                                            const std::string& fallbackModel) {
    agent::core::LlmConfig cfg;
    cfg.apiEndpoint = endpoint;
    cfg.mainModel = mainModel;
    cfg.validatorModel = validatorModel;
    cfg.fallbackModel = fallbackModel;
    cfg.connectTimeoutMs = 30000;
    cfg.requestTimeoutMs = 180000;
    return cfg;
  }

  std::string caseDir;
  std::string sessionDir;
  std::string memoryDir;
  agent::core::LlmConfig llmConfig;
  agent::api::HttpLlmClient modelClient;
  agent::api::SideQueryClient sideQueryClient;
  agent::tools::ToolOrchestrator toolOrchestrator;
  agent::tools::ToolRegistry toolRegistry;
  agent::permissions::PermissionEngine permissionEngine;
  agent::infra::SessionManager sessionManager;
  agent::agents::SubAgentManager subAgentManager;
  agent::memory::MemoryIndex memoryIndex;
  agent::core::QueryEngine engine;
};

BenchmarkResult RunPromptBenchmark(const std::string& id,
                                   const std::string& category,
                                   const std::string& caseDir,
                                   const std::string& endpoint,
                                   const std::string& model,
                                   const std::string& validatorModel,
                                   const std::string& fallbackModel,
                                   const std::string& prompt,
                                   const std::function<bool(EngineBundle&, std::string*)>& verify,
                                   agent::core::PermissionMode mode =
                                       agent::core::PermissionMode::Default) {
  BenchmarkResult result;
  result.id = id;
  result.category = category;
  result.artifactPath = caseDir;

  const auto start = std::chrono::steady_clock::now();
  EngineBundle bundle(caseDir, endpoint, model, validatorModel, fallbackModel);
  bundle.permissionEngine.SetPermissionMode(mode);

  ScopedCurrentDirectory cwd(caseDir);
  bundle.engine.SubmitUserPrompt(prompt);
  bool ok = true;
  std::string runError;
  try {
    bundle.engine.RunTurn();
  } catch (const std::exception& ex) {
    ok = false;
    runError = ex.what();
  } catch (...) {
    ok = false;
    runError = "unknown exception";
  }

  std::string evidence;
  if (!ok) {
    evidence = "RunTurn threw: " + runError;
  } else if (!verify(bundle, &evidence)) {
    if (evidence.empty()) evidence = "verification failed";
  } else {
    result.passed = true;
    evidence = evidence.empty() ? "benchmark passed" : evidence;
  }

  const std::string transcriptPath = bundle.sessionManager.TranscriptJsonlPath();
  WriteTextFile(JoinPath(caseDir, "messages.txt"), ExtractText(bundle.engine.messages()));
  if (FileExists(transcriptPath)) {
    WriteTextFile(JoinPath(caseDir, "transcript-copy.jsonl"),
                  ReadTextFile(transcriptPath));
  }

  const auto end = std::chrono::steady_clock::now();
  result.elapsedMs = static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
  result.evidence = evidence;
  return result;
}

BenchmarkResult RunToolBenchmark(const std::string& id,
                                 const std::string& category,
                                 const std::string& caseDir,
                                 const std::function<bool(std::string*, std::string*)>& action) {
  BenchmarkResult result;
  result.id = id;
  result.category = category;
  result.artifactPath = caseDir;
  EnsureDirectoryRecursive(caseDir);
  const auto start = std::chrono::steady_clock::now();

  std::string output;
  std::string evidence;
  result.passed = action(&output, &evidence);
  WriteTextFile(JoinPath(caseDir, "output.txt"), output);

  const auto end = std::chrono::steady_clock::now();
  result.elapsedMs = static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
  result.evidence = evidence.empty() ? (result.passed ? "benchmark passed" : "benchmark failed")
                                     : evidence;
  return result;
}

agent::tools::ToolOrchestrator MakeToolOrchestrator(
    agent::tools::ToolRegistry* registry,
    const std::string& workspaceRoot) {
  agent::tools::ToolOrchestrator orchestrator;
  orchestrator.SetToolRegistry(registry);
  orchestrator.SetWorkspaceRoot(workspaceRoot);
  return orchestrator;
}

agent::core::CanUseTool AllowAllTools() {
  return [](const agent::core::ContentBlock&,
            const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };
}

BenchmarkResult BenchmarkB01(const std::string& root,
                             const std::string& endpoint,
                             const std::string& model,
                             const std::string& validatorModel,
                             const std::string& fallbackModel) {
  const std::string caseDir = JoinPath(root, "B01");
  return RunPromptBenchmark(
      "B01", "功能完整性", caseDir, endpoint, model, validatorModel,
      fallbackModel,
      u8"请在当前工作目录创建一个名为 clock.html 的单文件网页。要求："
      u8"1. 使用工具真实写文件；2. 页面展示模拟时钟；3. 包含完整 html/head/body；"
      u8"4. 完成后简短说明，不要只描述方案。",
      [](EngineBundle& bundle, std::string* evidence) {
        const std::string path = JoinPath(bundle.caseDir, "clock.html");
        const std::string content = ReadTextFile(path);
        const bool ok = FileExists(path) &&
                        content.find("<html") != std::string::npos &&
                        content.find("clock") != std::string::npos;
        *evidence = ok ? "clock.html created in benchmark root"
                       : "clock.html missing or content incomplete";
        return ok;
      });
}

BenchmarkResult BenchmarkB02(const std::string& root,
                             const std::string& endpoint,
                             const std::string& model,
                             const std::string& validatorModel,
                             const std::string& fallbackModel) {
  const std::string caseDir = JoinPath(root, "B02");
  WriteTextFile(JoinPath(caseDir, "docs\\alpha.txt"),
                "Alpha module handles login.\nTODO: add retry logic.\n");
  WriteTextFile(JoinPath(caseDir, "docs\\beta.txt"),
                "Beta module handles payments.\nTODO: add audit logging.\n");
  return RunToolBenchmark(
      "B02", "功能完整性", caseDir,
      [&](std::string* output, std::string* evidence) {
        agent::tools::ToolRegistry registry;
        for (const auto& tool : agent::tools::ToolRegistry::GetAllBaseTools()) {
          registry.RegisterTool(tool);
        }
        agent::tools::ToolOrchestrator orchestrator =
            MakeToolOrchestrator(&registry, caseDir);

        std::vector<agent::core::ContentBlock> readBlocks;
        json alphaRead;
        alphaRead["file_path"] = JoinPath(caseDir, "docs\\alpha.txt");
        json betaRead;
        betaRead["file_path"] = JoinPath(caseDir, "docs\\beta.txt");
        readBlocks.push_back(agent::core::ContentBlock::MakeToolUse(
            "b02-read-alpha", "FileRead", alphaRead.dump()));
        readBlocks.push_back(agent::core::ContentBlock::MakeToolUse(
            "b02-read-beta", "FileRead", betaRead.dump()));
        const auto readResult = orchestrator.Execute(readBlocks, AllowAllTools(), {});
        *output = ExtractText(readResult.userMessages);

        const std::string summaryPath = JoinPath(caseDir, "summary.md");
        std::ostringstream summary;
        summary << "# TODO Summary\n\n";
        summary << "- Alpha: add retry logic\n";
        summary << "- Beta: add audit logging\n";
        json writeJson;
        writeJson["file_path"] = summaryPath;
        writeJson["content"] = summary.str();
        std::vector<agent::core::ContentBlock> writeBlocks;
        writeBlocks.push_back(agent::core::ContentBlock::MakeToolUse(
            "b02-write-summary", "FileWrite", writeJson.dump()));
        const auto writeResult = orchestrator.Execute(writeBlocks, AllowAllTools(), {});
        *output += "\n" + ExtractText(writeResult.userMessages);

        const std::string summaryText = ReadTextFile(summaryPath);
        const bool ok = readResult.errorCount == 0 &&
                        writeResult.errorCount == 0 &&
                        summaryText.find("retry logic") != std::string::npos &&
                        summaryText.find("audit logging") != std::string::npos;
        *evidence = ok ? "tool chain read both files and wrote summary.md"
                       : "tool chain failed to create expected summary.md";
        return ok;
      });
}

BenchmarkResult BenchmarkB03(const std::string& root,
                             const std::string& endpoint,
                             const std::string& model,
                             const std::string& validatorModel,
                             const std::string& fallbackModel) {
  BenchmarkResult result;
  result.id = "B03";
  result.category = "功能完整性";
  const std::string caseDir = JoinPath(root, "B03");
  result.artifactPath = caseDir;

  const auto start = std::chrono::steady_clock::now();
  EngineBundle bundle(caseDir, endpoint, model, validatorModel, fallbackModel);
  ScopedCurrentDirectory cwd(caseDir);

  bundle.engine.SubmitUserPrompt(
      "Create model_a.txt in the current directory with the single line "
      "\"generated by first model\". Use a tool.");
  bool ok1 = bundle.engine.RunTurnWithRecovery();

  bundle.engine.SetModel(validatorModel);
  bundle.engine.SubmitUserPrompt(
      "Now create model_b.txt in the current directory with the single line "
      "\"generated by second model\". Use a tool.");
  bool ok2 = bundle.engine.RunTurnWithRecovery();

  const bool existsA = FileExists(JoinPath(caseDir, "model_a.txt"));
  const bool existsB = FileExists(JoinPath(caseDir, "model_b.txt"));
  result.passed = ok1 && ok2 && existsA && existsB;
  result.evidence = result.passed
      ? ("dynamic model switch succeeded: " + model + " -> " + validatorModel)
      : "model switch benchmark failed";
  WriteTextFile(JoinPath(caseDir, "messages.txt"), ExtractText(bundle.engine.messages()));
  result.elapsedMs = static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start).count());
  return result;
}

BenchmarkResult BenchmarkB04(const std::string& root,
                             const std::string& endpoint,
                             const std::string& model,
                             const std::string& validatorModel,
                             const std::string& fallbackModel) {
  BenchmarkResult result;
  result.id = "B04";
  result.category = "功能完整性";
  const std::string caseDir = JoinPath(root, "B04");
  result.artifactPath = caseDir;
  EnsureDirectoryRecursive(caseDir);
  EnsureDirectoryRecursive(JoinPath(caseDir, "memory"));

  WriteTextFile(JoinPath(caseDir, "memory\\MEMORY.md"),
                "# Session Memory\nUse topic files for details.\n");
  WriteTextFile(JoinPath(caseDir, "memory\\user_role.md"),
                "The user is the release manager for this project.\n");
  WriteTextFile(JoinPath(caseDir, "memory\\project_stack.md"),
                "The project uses C++17, WinHTTP and CMake.\n");
  WriteTextFile(JoinPath(caseDir, "memory\\travel_notes.md"),
                "Vacation plans and train schedules.\n");

  const auto start = std::chrono::steady_clock::now();
  EngineBundle bundle(caseDir, endpoint, model, validatorModel, fallbackModel);
  std::vector<agent::memory::MemoryIndex::RelevantMemory> relevant =
      bundle.memoryIndex.FindRelevantMemories(
          "What is the user's role in this repository?", {});

  std::ostringstream output;
  bool foundRole = false;
  for (const auto& item : relevant) {
    output << item.fileName << "\n";
    if (item.fileName == "user_role.md") foundRole = true;
  }
  WriteTextFile(JoinPath(caseDir, "selected_memories.txt"), output.str());

  result.passed = foundRole;
  result.evidence = foundRole
      ? "side query selected user_role.md"
      : "user_role.md was not selected";
  result.elapsedMs = static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start).count());
  return result;
}

BenchmarkResult BenchmarkB05(const std::string& root) {
  const std::string caseDir = JoinPath(root, "B05");
  return RunToolBenchmark(
      "B05", "兼容性", caseDir,
      [&](std::string* output, std::string* evidence) {
        agent::tools::ToolRegistry registry;
        const auto base = agent::tools::ToolRegistry::GetAllBaseTools();
        for (const auto& tool : base) registry.RegisterTool(tool);
        agent::tools::ToolOrchestrator orchestrator =
            MakeToolOrchestrator(&registry, caseDir);
        std::vector<agent::core::ContentBlock> blocks;
        json payload;
        payload["url"] = "https://example.com/";
        blocks.push_back(agent::core::ContentBlock::MakeToolUse(
            "webfetch-1", "WebFetch", payload.dump()));
        auto result = orchestrator.Execute(blocks, AllowAllTools(), {});
        *output = ExtractText(result.userMessages);
        const bool ok = result.errorCount == 0 &&
                        output->find("Example Domain") != std::string::npos;
        *evidence = ok ? "WebFetch fetched example.com"
                       : "WebFetch failed or missing Example Domain";
        return ok;
      });
}

BenchmarkResult BenchmarkB06(const std::string& root) {
  const std::string caseDir = JoinPath(root, "B06");
  return RunToolBenchmark(
      "B06", "兼容性", caseDir,
      [&](std::string* output, std::string* evidence) {
        agent::tools::ToolRegistry registry;
        const auto base = agent::tools::ToolRegistry::GetAllBaseTools();
        for (const auto& tool : base) registry.RegisterTool(tool);
        agent::tools::ToolOrchestrator orchestrator =
            MakeToolOrchestrator(&registry, caseDir);
        std::vector<agent::core::ContentBlock> blocks;
        json payload;
        payload["query"] = "Example Domain";
        payload["num"] = 3;
        blocks.push_back(agent::core::ContentBlock::MakeToolUse(
            "websearch-1", "WebSearch", payload.dump()));
        auto result = orchestrator.Execute(blocks, AllowAllTools(), {});
        *output = ExtractText(result.userMessages);
        const bool ok = result.errorCount == 0 &&
                        output->find("http") != std::string::npos;
        *evidence = ok ? "WebSearch returned structured results"
                       : "WebSearch failed or returned no links";
        return ok;
      });
}

BenchmarkResult BenchmarkB07(const std::string& root,
                             const std::string& model,
                             const std::string& validatorModel,
                             const std::string& fallbackModel) {
  BenchmarkResult result;
  result.id = "B07";
  result.category = "兼容性";
  const std::string caseDir = JoinPath(root, "B07");
  result.artifactPath = caseDir;
  EnsureDirectoryRecursive(caseDir);

  const auto start = std::chrono::steady_clock::now();
  const std::string anthropicModel =
      validatorModel.empty() ? model : validatorModel;
  agent::core::LlmConfig cfg = EngineBundle::BuildConfig(
      "http://127.0.0.1:8080/v1/messages", anthropicModel, validatorModel, fallbackModel);
  agent::api::HttpLlmClient client(cfg);
  std::vector<agent::core::Message> messages;
  agent::core::Message userMsg;
  userMsg.role = agent::core::MessageRole::User;
  userMsg.content.push_back(
      agent::core::ContentBlock::MakeText("Reply with ANTHROPIC_STREAM_OK only."));
  messages.push_back(userMsg);

  const std::vector<agent::core::Message> response =
      client.GenerateResponse(messages, "", anthropicModel);
  const std::string output = ExtractText(response);
  WriteTextFile(JoinPath(caseDir, "response.txt"), output);
  result.passed = output.find("ANTHROPIC_STREAM_OK") != std::string::npos;
  result.evidence = result.passed
      ? ("Anthropic native endpoint returned expected response via " +
         anthropicModel)
      : "Anthropic native endpoint did not return expected response";
  result.elapsedMs = static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start).count());
  return result;
}

BenchmarkResult BenchmarkB08(const std::string& root,
                             const std::string& endpoint,
                             const std::string& model,
                             const std::string& validatorModel,
                             const std::string& fallbackModel) {
  const std::string caseDir = JoinPath(root, "B08");
  WriteTextFile(JoinPath(caseDir, "todo\\a.txt"), "TODO: fix parser\n");
  WriteTextFile(JoinPath(caseDir, "todo\\b.txt"), "TODO: improve logs\n");
  WriteTextFile(JoinPath(caseDir, "todo\\c.txt"), "DONE: shipped\n");
  return RunToolBenchmark(
      "B08", "性能稳定性", caseDir,
      [&](std::string* output, std::string* evidence) {
        agent::tools::ToolRegistry registry;
        for (const auto& tool : agent::tools::ToolRegistry::GetAllBaseTools()) {
          registry.RegisterTool(tool);
        }
        agent::tools::ToolOrchestrator orchestrator =
            MakeToolOrchestrator(&registry, caseDir);

        std::vector<agent::core::ContentBlock> blocks;
        json globJson;
        globJson["pattern"] = "todo/*.txt";
        blocks.push_back(agent::core::ContentBlock::MakeToolUse(
            "b08-glob", "Glob", globJson.dump()));

        for (const char* name : {"a.txt", "b.txt", "c.txt"}) {
          json readJson;
          readJson["file_path"] = JoinPath(caseDir, std::string("todo\\") + name);
          blocks.push_back(agent::core::ContentBlock::MakeToolUse(
              std::string("b08-read-") + name, "FileRead", readJson.dump()));
        }

        const auto result = orchestrator.Execute(blocks, AllowAllTools(), {});
        *output = ExtractText(result.userMessages);
        const bool ok = result.errorCount == 0 &&
                        output->find("fix parser") != std::string::npos &&
                        output->find("improve logs") != std::string::npos;
        *evidence = ok ? "read-only tool batch returned both TODO items"
                       : "read-only tool batch missed expected TODO items";
        return ok;
      });
}

BenchmarkResult BenchmarkB09(const std::string& root,
                             const std::string& endpoint,
                             const std::string& model,
                             const std::string& validatorModel,
                             const std::string& fallbackModel) {
  BenchmarkResult result;
  result.id = "B09";
  result.category = "异常场景容错";
  const std::string caseDir = JoinPath(root, "B09");
  result.artifactPath = caseDir;

  const auto start = std::chrono::steady_clock::now();
  EngineBundle bundle(caseDir, endpoint, model, validatorModel, fallbackModel);
  ScopedCurrentDirectory cwd(caseDir);

  for (int i = 0; i < 11; ++i) {
    std::ostringstream large;
    large << "long-context-" << i << ":";
    large << std::string(80000, static_cast<char>('A' + (i % 26)));
    bundle.engine.SubmitUserPrompt(large.str());
  }
  bundle.engine.SubmitUserPrompt(
      "Reply with LONG_CONTEXT_OK only after handling the prior context safely.");
  const bool ok = bundle.engine.RunTurnWithRecovery();
  const std::string text = ExtractText(bundle.engine.messages());
  result.passed = ok &&
                  text.find("[Context Collapse]") != std::string::npos &&
                  text.find("LONG_CONTEXT_OK") != std::string::npos;
  result.evidence = result.passed
      ? "long context completed with collapse boundary"
      : "long context benchmark failed to collapse or complete";
  WriteTextFile(JoinPath(caseDir, "messages.txt"), text);
  result.elapsedMs = static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start).count());
  return result;
}

BenchmarkResult BenchmarkB10(const std::string& root,
                             const std::string& endpoint,
                             const std::string& model,
                             const std::string& validatorModel,
                             const std::string& fallbackModel) {
  const std::string caseDir = JoinPath(root, "B10");
  return RunPromptBenchmark(
      "B10", "异常场景容错", caseDir, endpoint, model, validatorModel,
      fallbackModel,
      "You must attempt to use a write tool to create should_not_exist.txt in "
      "the current directory with the text 'blocked by plan mode'.",
      [](EngineBundle& bundle, std::string* evidence) {
        const std::string path = JoinPath(bundle.caseDir, "should_not_exist.txt");
        const std::string transcript =
            ReadTextFile(bundle.sessionManager.TranscriptJsonlPath());
        const bool ok = !FileExists(path) &&
                        transcript.find("[plan mode] Tool execution skipped") !=
                            std::string::npos;
        *evidence = ok ? "plan mode skipped tool execution and did not write file"
                       : "plan mode benchmark did not produce skip evidence";
        return ok;
      },
      agent::core::PermissionMode::Plan);
}

BenchmarkResult BenchmarkB11(const std::string& root) {
  const std::string caseDir = JoinPath(root, "B11");
  return RunToolBenchmark(
      "B11", "工作区路径语义", caseDir,
      [&](std::string* output, std::string* evidence) {
        agent::tools::ToolRegistry registry;
        for (const auto& tool : agent::tools::ToolRegistry::GetAllBaseTools()) {
          registry.RegisterTool(tool);
        }
        agent::tools::ToolOrchestrator orchestrator =
            MakeToolOrchestrator(&registry, caseDir);

        const std::string relativePath = "workspace-output\\artifact.txt";
        const std::string expectedPath = JoinPath(caseDir, relativePath);
        const std::string memoryPath =
            JoinPath(caseDir, ".cpp-agent\\memory\\artifact.txt");
        EnsureDirectoryRecursive(JoinPath(caseDir, ".cpp-agent\\memory"));

        json payload;
        payload["file_path"] = relativePath;
        payload["content"] =
            "workspace benchmark output\n"
            "must stay in workspace root\n";

        const auto result = orchestrator.Execute(
            {agent::core::ContentBlock::MakeToolUse(
                "b11-write", "Write", payload.dump())},
            AllowAllTools(), {});
        *output = ExtractText(result.userMessages);

        const std::string actual = ReadTextFile(expectedPath);
        const bool ok = result.errorCount == 0 &&
                        FileExists(expectedPath) &&
                        !FileExists(memoryPath) &&
                        actual.find("workspace benchmark output") !=
                            std::string::npos;
        *evidence = ok
                        ? "relative write resolved inside workspace root and not into .cpp-agent memory"
                        : "relative write did not stay inside workspace root";
        return ok;
      });
}

void WriteBenchmarkReport(const std::string& reportPath,
                          const std::vector<BenchmarkResult>& results) {
  std::ostringstream md;
  md << "# Benchmark Results\n\n";
  md << "| ID | Category | Status | Elapsed(ms) | Evidence | Artifact |\n";
  md << "|---|---|---|---:|---|---|\n";
  for (const auto& result : results) {
    md << "|" << result.id
       << "|" << result.category
       << "|" << (result.passed ? "PASS" : "FAIL")
       << "|" << result.elapsedMs
       << "|" << result.evidence
       << "|" << result.artifactPath
       << "|\n";
  }
  WriteTextFile(reportPath, md.str());
}

}  // namespace

bool ShouldRun(int argc, char** argv, const std::string& id) {
  if (argc <= 1) return true;
  for (int i = 1; i < argc; ++i) {
    if (id == argv[i]) return true;
  }
  return false;
}

int main(int argc, char** argv) {
  const std::string projectRoot = DiscoverProjectRoot();
  const std::string buildDir = JoinPath(projectRoot, "build");
  const std::string benchmarkRoot = JoinPath(buildDir, "benchmarks");
  EnsureDirectoryRecursive(benchmarkRoot);

  const std::string endpoint = "http://127.0.0.1:8080/v1/chat/completions";
  const std::string mainModel = "Qwen3.6-35B-A3B-UD-Q6_K";
  const std::string validatorModel = "gemma-4-31B-it-Q8_0";
  const std::string fallbackModel = "gemma-4-31B-it-Q8_0";

  std::vector<BenchmarkResult> results;
  if (ShouldRun(argc, argv, "B01")) {
    std::cout << "Running B01..." << std::endl;
    results.push_back(BenchmarkB01(benchmarkRoot, endpoint, mainModel,
                                   validatorModel, fallbackModel));
  }
  if (ShouldRun(argc, argv, "B02")) {
    std::cout << "Running B02..." << std::endl;
    results.push_back(BenchmarkB02(benchmarkRoot, endpoint, mainModel,
                                   validatorModel, fallbackModel));
  }
  if (ShouldRun(argc, argv, "B03")) {
    std::cout << "Running B03..." << std::endl;
    results.push_back(BenchmarkB03(benchmarkRoot, endpoint, mainModel,
                                   validatorModel, fallbackModel));
  }
  if (ShouldRun(argc, argv, "B04")) {
    std::cout << "Running B04..." << std::endl;
    results.push_back(BenchmarkB04(benchmarkRoot, endpoint, mainModel,
                                   validatorModel, fallbackModel));
  }
  if (ShouldRun(argc, argv, "B05")) {
    std::cout << "Running B05..." << std::endl;
    results.push_back(BenchmarkB05(benchmarkRoot));
  }
  if (ShouldRun(argc, argv, "B06")) {
    std::cout << "Running B06..." << std::endl;
    results.push_back(BenchmarkB06(benchmarkRoot));
  }
  if (ShouldRun(argc, argv, "B07")) {
    std::cout << "Running B07..." << std::endl;
    results.push_back(BenchmarkB07(benchmarkRoot, mainModel,
                                   validatorModel, fallbackModel));
  }
  if (ShouldRun(argc, argv, "B08")) {
    std::cout << "Running B08..." << std::endl;
    results.push_back(BenchmarkB08(benchmarkRoot, endpoint, mainModel,
                                   validatorModel, fallbackModel));
  }
  if (ShouldRun(argc, argv, "B09")) {
    std::cout << "Running B09..." << std::endl;
    results.push_back(BenchmarkB09(benchmarkRoot, endpoint, mainModel,
                                   validatorModel, fallbackModel));
  }
  if (ShouldRun(argc, argv, "B10")) {
    std::cout << "Running B10..." << std::endl;
    results.push_back(BenchmarkB10(benchmarkRoot, endpoint, mainModel,
                                   validatorModel, fallbackModel));
  }
  if (ShouldRun(argc, argv, "B11")) {
    std::cout << "Running B11..." << std::endl;
    results.push_back(BenchmarkB11(benchmarkRoot));
  }

  WriteBenchmarkReport(JoinPath(benchmarkRoot, "benchmark-results.md"), results);

  int failures = 0;
  for (const auto& result : results) {
    std::cout << result.id << " [" << (result.passed ? "PASS" : "FAIL")
              << "] " << result.evidence << " (" << result.elapsedMs
              << " ms)" << std::endl;
    if (!result.passed) ++failures;
  }
  return failures == 0 ? 0 : 1;
}
