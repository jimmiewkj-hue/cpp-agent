#include "api/ModelClient.h"
#include "core/AgentTypes.h"

#include <windows.h>

#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

void LoadDebugEnvIntoProcess() {
  std::ifstream in(".dbg\\stream-response-stall.env", std::ios::binary);
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (!key.empty()) {
      SetEnvironmentVariableA(key.c_str(), value.c_str());
    }
  }
}

std::string ReadAllText(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::string();
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

std::string GetEnvOrDefault(const char* name, const std::string& fallback) {
  char buffer[512] = {0};
  DWORD len = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
  if (len == 0 || len >= sizeof(buffer)) return fallback;
  return std::string(buffer, len);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: agent_model_stream_smoke_runner <prompt-file>\n";
    return 2;
  }

  LoadDebugEnvIntoProcess();
  const std::string prompt = ReadAllText(argv[1]);
  if (prompt.empty()) {
    std::cerr << "Prompt file is empty: " << argv[1] << "\n";
    return 2;
  }

  agent::core::LlmConfig llmCfg;
  llmCfg.apiEndpoint = GetEnvOrDefault(
      "CPP_AGENT_API_ENDPOINT", "http://127.0.0.1:8080/v1/chat/completions");
  llmCfg.mainModel = GetEnvOrDefault(
      "CPP_AGENT_MAIN_MODEL", "Qwen3.6-35B-A3B-UD-Q6_K");
  llmCfg.validatorModel = GetEnvOrDefault(
      "CPP_AGENT_VALIDATOR_MODEL", "");
  llmCfg.fallbackModel = GetEnvOrDefault(
      "CPP_AGENT_FALLBACK_MODEL", "gemma-4-31B-it-Q8_0");
  llmCfg.connectTimeoutMs = 30000;
  llmCfg.requestTimeoutMs = 180000;

  agent::api::HttpLlmClient client(llmCfg);
  std::vector<agent::core::Message> messages;
  agent::core::Message user;
  user.role = agent::core::MessageRole::User;
  user.uuid = "smoke-user-1";
  user.content.push_back(agent::core::ContentBlock::MakeText(prompt));
  messages.push_back(user);

  std::atomic<bool> completed(false);
  std::thread heartbeat([&]() {
    int beat = 0;
    while (!completed.load()) {
      Sleep(5000);
      if (completed.load()) break;
      std::cout << "smoke_heartbeat=" << (++beat) << std::endl;
    }
  });

  int textEvents = 0;
  int toolEvents = 0;
  int apiErrors = 0;
  int stopEvents = 0;
  std::cout << "smoke_start=true" << std::endl;
  client.StreamResponse(
      messages,
      "You are a helpful coding agent. Use tools when required.",
      llmCfg.mainModel,
      "",
      [&](const std::string& event, const std::string& data) {
        if (event == "text_delta") ++textEvents;
        if (event == "tool_use") ++toolEvents;
        if (event == "api_error") ++apiErrors;
        if (event == "stop_reason") ++stopEvents;
        std::cout << "event=" << event << "\n";
        std::cout << "data_prefix=" << data.substr(0, 200) << "\n";
      });
  completed.store(true);
  if (heartbeat.joinable()) heartbeat.join();

  std::cout << "smoke_done=true" << std::endl;
  std::cout << "text_events=" << textEvents << std::endl;
  std::cout << "tool_events=" << toolEvents << std::endl;
  std::cout << "api_errors=" << apiErrors << std::endl;
  std::cout << "stop_events=" << stopEvents << std::endl;
  return 0;
}
