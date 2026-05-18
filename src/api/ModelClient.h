#pragma once

#include "core/AgentTypes.h"
#include "core/StateTypes.h"

#include <functional>
#include <string>
#include <vector>

namespace agent {
namespace api {

using SseEventCallback = std::function<void(
    const std::string& event, const std::string& data)>;

class ModelClient {
 public:
  virtual ~ModelClient() = default;

  virtual std::vector<core::Message> GenerateResponse(
      const std::vector<core::Message>& messages,
      const std::string& systemPrompt,
      const std::string& model) = 0;

  virtual void StreamResponse(
      const std::vector<core::Message>& messages,
      const std::string& systemPrompt,
      const std::string& model,
      const std::string& toolsJson,
      const SseEventCallback& onEvent);

  virtual std::vector<core::Message> SideQuery(
      const std::vector<core::Message>& messages,
      const std::string& systemPrompt,
      const std::string& model) = 0;
};

class SkeletonModelClient : public ModelClient {
 public:
  SkeletonModelClient();

  std::vector<core::Message> GenerateResponse(
      const std::vector<core::Message>& messages,
      const std::string& systemPrompt,
      const std::string& model) override;

  void StreamResponse(
      const std::vector<core::Message>& messages,
      const std::string& systemPrompt,
      const std::string& model,
      const std::string& toolsJson,
      const SseEventCallback& onEvent) override;

  std::vector<core::Message> SideQuery(
      const std::vector<core::Message>& messages,
      const std::string& systemPrompt,
      const std::string& model) override;

 private:
  int callCount_ = 0;
};

class HttpLlmClient : public ModelClient {
 public:
  explicit HttpLlmClient(const core::LlmConfig& config);

  std::vector<core::Message> GenerateResponse(
      const std::vector<core::Message>& messages,
      const std::string& systemPrompt,
      const std::string& model) override;

  void StreamResponse(
      const std::vector<core::Message>& messages,
      const std::string& systemPrompt,
      const std::string& model,
      const std::string& toolsJson,
      const SseEventCallback& onEvent) override;

  std::vector<core::Message> SideQuery(
      const std::vector<core::Message>& messages,
      const std::string& systemPrompt,
      const std::string& model) override;

  static bool IsNativeAnthropicEndpoint(const std::string& endpoint);
  static bool IsOpenAICompatibleEndpoint(const std::string& endpoint);

 private:
  std::string BuildAnthropicBody(
      const std::vector<core::Message>& messages,
      const std::string& systemPrompt,
      const std::string& model,
      int maxTokens, bool stream) const;
  std::string BuildOpenAIBody(
      const std::vector<core::Message>& messages,
      const std::string& systemPrompt,
      const std::string& model,
      int maxTokens, bool stream) const;
  std::string BuildRequestBody(
      const std::vector<core::Message>& messages,
      const std::string& systemPrompt,
      const std::string& model,
      int maxTokens, bool stream,
      const std::string& toolsJson) const;
  std::string SendHttpPost(const std::string& body,
                           const std::string& model,
                           std::string* pathOverride,
                           std::string* error) const;

  core::LlmConfig config_;
  bool isNativeAnthropic_ = false;
};

}  // namespace api
}  // namespace agent
