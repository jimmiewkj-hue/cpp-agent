#pragma once

#include <string>
#include <vector>
#include <functional>
#include <map>
#include <mutex>

namespace agent {
namespace mcp {

// P0-03: MCP channel notification relay (aligned with local-ace channelNotification.ts).
// Mirrors permission prompts over active channels (Telegram, iMessage, Discord).
// When the agent hits a permission dialog, it sends the prompt via active channels
// and races the reply against local UI/bridge/hooks/classifier. First resolver wins.

// ============================================================================
// Channel notification types
// ============================================================================
enum class ChannelNotificationType {
  PermissionRequest,     // Tool permission prompt
  ElicitationRequest,    // MCP elicitation (user input)
  OAuthCallback,         // OAuth authorization callback
  ServerNotification,    // Generic server notification
};

struct ChannelPermissionResponse {
  enum Behavior { Allow, Deny };
  Behavior behavior = Deny;
  std::string fromServer;  // Which channel server the reply came from
  std::string requestId;
};

// ============================================================================
// Channel notification config
// ============================================================================
struct ChannelNotificationConfig {
  bool enabled = false;
  int responseTimeoutMs = 30000;       // How long to wait for channel response
  int maxRetries = 3;
  std::vector<std::string> activeChannels;  // e.g., {"telegram", "discord"}
  bool requireServerApproval = true;   // Server must emit approval event, not just relay
};

// ============================================================================
// Response resolver
// ============================================================================
using ChannelResponseHandler = std::function<void(const ChannelPermissionResponse&)>;
using UnsubscribeFn = std::function<void()>;

class ChannelNotificationRelay {
 public:
  explicit ChannelNotificationRelay(const ChannelNotificationConfig& config = {});

  // Check if channel permission relay is enabled
  bool IsEnabled() const;

  // Register a response handler for a specific request ID.
  // Returns an unsubscribe function.
  // (aligned with local-ace ChannelPermissionCallbacks.onResponse)
  UnsubscribeFn OnResponse(const std::string& requestId,
                            ChannelResponseHandler handler);

  // Send a permission request through active channels.
  // Returns the request ID for tracking.
  // (aligned with local-ace sendChannelPermissionRequest)
  std::string SendPermissionRequest(
      const std::string& toolName,
      const std::string& serverName,
      const std::string& prompt);

  // Handle an incoming permission response from a channel server.
  // Resolves the pending request and notifies all registered handlers.
  bool HandleIncomingResponse(const ChannelPermissionResponse& response);

  // Send an elicitation request (user input prompt via MCP).
  std::string SendElicitationRequest(
      const std::string& serverName,
      const std::string& prompt,
      const std::string& requestSchema);

  // Send a generic server notification.
  void SendServerNotification(
      const std::string& serverName,
      const std::string& notificationType,
      const std::string& payload);

  // Check if there are pending requests waiting for channel responses.
  bool HasPendingRequests() const;

  // Get the count of pending requests.
  int PendingRequestCount() const;

  // Cancel all pending requests (e.g., on session shutdown).
  void CancelAllPending();

  // Configuration
  void SetConfig(const ChannelNotificationConfig& config);
  const ChannelNotificationConfig& Config() const { return config_; }

 private:
  struct PendingRequest {
    std::string requestId;
    std::string toolName;
    std::string serverName;
    std::string prompt;
    long long sentAtMs;
    std::vector<ChannelResponseHandler> handlers;
  };

  std::string GenerateRequestId() const;
  long long NowMs() const;

  ChannelNotificationConfig config_;
  std::map<std::string, PendingRequest> pendingRequests_;
  mutable std::mutex mutex_;
  mutable int requestCounter_ = 0;
};

}  // namespace mcp
}  // namespace agent
