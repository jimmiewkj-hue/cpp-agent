#include "mcp/ChannelNotification.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <random>

namespace agent {
namespace mcp {

ChannelNotificationRelay::ChannelNotificationRelay(
    const ChannelNotificationConfig& config)
    : config_(config) {}

bool ChannelNotificationRelay::IsEnabled() const {
  return config_.enabled && !config_.activeChannels.empty();
}

void ChannelNotificationRelay::SetConfig(const ChannelNotificationConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_ = config;
}

UnsubscribeFn ChannelNotificationRelay::OnResponse(
    const std::string& requestId,
    ChannelResponseHandler handler) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = pendingRequests_.find(requestId);
  if (it == pendingRequests_.end()) {
    PendingRequest pr;
    pr.requestId = requestId;
    pr.sentAtMs = NowMs();
    it = pendingRequests_.emplace(requestId, pr).first;
  }

  it->second.handlers.push_back(std::move(handler));

  // Return unsubscribe function
  return [this, requestId]() {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingRequests_.erase(requestId);
  };
}

std::string ChannelNotificationRelay::SendPermissionRequest(
    const std::string& toolName,
    const std::string& serverName,
    const std::string& prompt) {
  if (!IsEnabled()) return std::string();

  std::string requestId = GenerateRequestId();

  std::lock_guard<std::mutex> lock(mutex_);
  PendingRequest pr;
  pr.requestId = requestId;
  pr.toolName = toolName;
  pr.serverName = serverName;
  pr.prompt = prompt;
  pr.sentAtMs = NowMs();
  pendingRequests_[requestId] = pr;

  return requestId;
}

bool ChannelNotificationRelay::HandleIncomingResponse(
    const ChannelPermissionResponse& response) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = pendingRequests_.find(response.requestId);
  if (it == pendingRequests_.end()) return false;

  // Notify all handlers
  auto handlers = std::move(it->second.handlers);
  pendingRequests_.erase(it);

  // Release lock before calling handlers to avoid deadlocks
  // (but in this single-threaded context it's fine)

  for (const auto& handler : handlers) {
    if (handler) handler(response);
  }

  return true;
}

std::string ChannelNotificationRelay::SendElicitationRequest(
    const std::string& serverName,
    const std::string& prompt,
    const std::string& requestSchema) {
  if (!IsEnabled()) return std::string();

  std::string requestId = GenerateRequestId();

  std::lock_guard<std::mutex> lock(mutex_);
  PendingRequest pr;
  pr.requestId = requestId;
  pr.serverName = serverName;
  pr.prompt = prompt;
  pr.sentAtMs = NowMs();
  pendingRequests_[requestId] = pr;

  (void)requestSchema;
  return requestId;
}

void ChannelNotificationRelay::SendServerNotification(
    const std::string& serverName,
    const std::string& notificationType,
    const std::string& payload) {
  (void)serverName;
  (void)notificationType;
  (void)payload;
  // In a full implementation, this would broadcast to all active channels
}

bool ChannelNotificationRelay::HasPendingRequests() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !pendingRequests_.empty();
}

int ChannelNotificationRelay::PendingRequestCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<int>(pendingRequests_.size());
}

void ChannelNotificationRelay::CancelAllPending() {
  std::lock_guard<std::mutex> lock(mutex_);
  pendingRequests_.clear();
}

std::string ChannelNotificationRelay::GenerateRequestId() const {
  ++requestCounter_;
  std::ostringstream id;
  id << "chperm-" << requestCounter_ << "-"
     << std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
  return id.str();
}

long long ChannelNotificationRelay::NowMs() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace mcp
}  // namespace agent
