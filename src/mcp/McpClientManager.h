#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace agent {
namespace mcp {

struct McpServerConfig {
  std::string name;
  std::string transportType;
  std::string endpoint;
};

struct McpServerCapabilities {
  bool tools = false;
  bool prompts = false;
  bool resources = false;
  bool resourcesSubscribe = false;
  bool elicitation = false;
};

struct McpToolSchema {
  std::string serverName;
  std::string toolName;
  std::string fullyQualifiedName;
  std::string description;
  bool readOnlyHint = false;
  bool destructiveHint = false;
  bool openWorldHint = false;
  std::string inputSchemaJson;
};

struct McpPromptSchema {
  std::string name;
  std::string description;
  std::string serverName;
};

struct McpResourceSchema {
  std::string uri;
  std::string name;
  std::string description;
  std::string mimeType;
  std::string serverName;
};

static constexpr int kMaxMcpDescriptionLength = 2048;

struct McpServerConnection {
  enum class Type {
    Pending,
    Connected,
    Failed,
    NeedsAuth,
    Disabled
  };

  Type type = Type::Pending;
  std::string name;
  McpServerConfig config;
  McpServerCapabilities capabilities;
  std::string error;
  std::string instructions;
  std::string serverVersion;
  std::string transportSessionId;
  std::string clientSessionId;
  long long sessionExpiresAtUnixMs = 0;
  long long lastActivityUnixMs = 0;
  int reconnectCount = 0;
  bool streamableHttp = false;
  std::vector<McpToolSchema> tools;
  std::vector<McpPromptSchema> prompts;
  std::vector<McpResourceSchema> resources;
};

struct McpTransportRequest {
  std::string method;
  std::string paramsJson;
};

struct McpTransportResponse {
  bool ok = false;
  std::string bodyJson;
  std::string error;
};

class McpTransport {
 public:
  virtual ~McpTransport() {}
  virtual bool Connect(
      const McpServerConfig& config,
      std::string* error) = 0;
  virtual void PopulateConnectionState(McpServerConnection* connection) const = 0;
  virtual McpTransportResponse Send(
      const McpTransportRequest& request) = 0;
  virtual void Close() = 0;
};

using McpTransportFactory = std::function<std::unique_ptr<McpTransport>(
    const McpServerConfig& config)>;

class McpClientManager {
 public:
  McpClientManager();
  ~McpClientManager();

  void SetTransportFactory(McpTransportFactory factory);
  bool RegisterServer(const McpServerConfig& config);
  std::vector<McpServerConnection> connections() const;
  bool ConnectServer(const std::string& serverName);

  bool MarkConnected(const std::string& serverName);
  bool MarkFailed(const std::string& serverName, const std::string& error);
  bool MarkNeedsAuth(const std::string& serverName);
  bool HandleOAuth401(const std::string& serverName);
  void SetOAuthTokenProvider(
      std::function<std::string()> provider);
  static int GetConnectionBatchLimit();

  bool RefreshToolsFromTransport(const std::string& serverName);
  bool SetFetchedTools(
      const std::string& serverName,
      const std::vector<McpToolSchema>& tools);
  std::vector<McpToolSchema> FetchToolsForClient(
      const std::string& serverName) const;
  static bool ParseToolsListResponse(
      const std::string& serverName,
      const std::string& bodyJson,
      std::vector<McpToolSchema>* tools,
      std::string* error);

  bool RefreshPromptsFromTransport(const std::string& serverName);
  std::vector<McpPromptSchema> FetchPromptsForClient(
      const std::string& serverName) const;

  bool RefreshResourcesFromTransport(const std::string& serverName);
  std::vector<McpResourceSchema> FetchResourcesForClient(
      const std::string& serverName) const;
  bool ReadResourceFromTransport(const std::string& serverName,
                                 const std::string& uri,
                                 std::string* bodyJson,
                                 std::string* error);

  static std::string BuildMcpToolName(
      const std::string& serverName,
      const std::string& toolName);

 private:
  struct ManagedConnection;

  ManagedConnection* FindConnection(const std::string& serverName);
  const ManagedConnection* FindConnection(const std::string& serverName) const;
  static std::string NormalizeNameForMcp(const std::string& name);

  std::vector<std::shared_ptr<ManagedConnection> > connections_;
  McpTransportFactory transportFactory_;
  std::function<std::string()> oAuthTokenProvider_;
};

}  // namespace mcp
}  // namespace agent
