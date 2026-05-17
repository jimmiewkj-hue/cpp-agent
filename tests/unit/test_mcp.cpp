#include "mcp/McpClientManager.h"

#include <iostream>

static int failures = 0;
static void Check(bool condition, const char* label) {
  if (!condition) { std::cerr << "FAIL: " << label << std::endl; ++failures; }
}

namespace {

void TestMcpToolNaming() {
  std::string name = agent::mcp::McpClientManager::BuildMcpToolName(
      "my-server", "read_file");
  Check(name.find("mcp__") == 0, "MCP tool name should have mcp__ prefix");
  Check(name.find("my_server") != std::string::npos,
        "MCP tool name should have normalized server name");
  Check(name.find("read_file") != std::string::npos,
        "MCP tool name should have tool name");
}

void TestMcpServerConnectionStates() {
  Check(static_cast<int>(agent::mcp::McpServerConnection::Type::Pending) == 0,
        "Pending state");
  Check(static_cast<int>(agent::mcp::McpServerConnection::Type::Connected) == 1,
        "Connected state");
  Check(static_cast<int>(agent::mcp::McpServerConnection::Type::Failed) == 2,
        "Failed state");
  Check(static_cast<int>(agent::mcp::McpServerConnection::Type::NeedsAuth) == 3,
        "NeedsAuth state");
  Check(static_cast<int>(agent::mcp::McpServerConnection::Type::Disabled) == 4,
        "Disabled state");
}

void TestMcpCapabilities() {
  agent::mcp::McpServerCapabilities caps;
  Check(!caps.tools, "capabilities.tools default false");
  Check(!caps.prompts, "capabilities.prompts default false");
  Check(!caps.resources, "capabilities.resources default false");
  Check(!caps.elicitation, "capabilities.elicitation default false");

  caps.tools = true;
  caps.prompts = true;
  caps.resources = true;
  Check(caps.tools, "capabilities.tools set true");
  Check(caps.prompts, "capabilities.prompts set true");
  Check(caps.resources, "capabilities.resources set true");
}

void TestMcpBatchLimit() {
  int limit = agent::mcp::McpClientManager::GetConnectionBatchLimit();
  Check(limit > 0 && limit <= 20, "MCP batch limit should be 1-20");
}

void TestMcpSchemaStructs() {
  agent::mcp::McpToolSchema tool;
  tool.serverName = "test-srv";
  tool.toolName = "echo";
  tool.fullyQualifiedName = "mcp__test_srv__echo";
  Check(!tool.serverName.empty(), "McpToolSchema stores serverName");

  agent::mcp::McpPromptSchema prompt;
  prompt.name = "greet";
  prompt.serverName = "test-srv";
  prompt.description = "A friendly prompt";
  Check(prompt.name == "greet", "McpPromptSchema stores name");

  agent::mcp::McpResourceSchema resource;
  resource.uri = "file:///test/data.json";
  resource.serverName = "test-srv";
  resource.mimeType = "application/json";
  Check(resource.uri.find("file://") == 0, "McpResourceSchema stores uri");
}

void TestMcpClientManagerLifetime() {
  agent::mcp::McpClientManager manager;

  agent::mcp::McpServerConfig cfg;
  cfg.name = "test-server";
  cfg.transportType = "stdio";
  bool registered = manager.RegisterServer(cfg);
  Check(registered, "RegisterServer should succeed");

  auto conns = manager.connections();
  Check(conns.size() >= 1, "Manager should have at least one connection");

  manager.MarkConnected("test-server");
  auto conns2 = manager.connections();
  bool foundConnected = false;
  for (const auto& c : conns2) {
    if (c.name == "test-server" &&
        c.type == agent::mcp::McpServerConnection::Type::Connected) {
      foundConnected = true;
    }
  }
  Check(foundConnected, "MarkConnected should update state");

  manager.MarkNeedsAuth("test-server");
  auto conns3 = manager.connections();
  bool foundNeedsAuth = false;
  for (const auto& c : conns3) {
    if (c.name == "test-server" &&
        c.type == agent::mcp::McpServerConnection::Type::NeedsAuth) {
      foundNeedsAuth = true;
    }
  }
  Check(foundNeedsAuth, "MarkNeedsAuth should update state");
}

}  // namespace

int main() {
  TestMcpToolNaming();
  TestMcpServerConnectionStates();
  TestMcpCapabilities();
  TestMcpBatchLimit();
  TestMcpSchemaStructs();
  TestMcpClientManagerLifetime();

  std::cout << "[test_mcp] Failures: " << failures << std::endl;
  return failures > 0 ? 1 : 0;
}
