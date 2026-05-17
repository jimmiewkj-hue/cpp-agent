#pragma once

#include "core/AgentTypes.h"

#include <string>
#include <vector>

namespace agent {
namespace api {

class ModelClient;

struct SideQueryRequest {
  std::string querySource;
  std::string model;
  std::string systemPrompt;
  std::vector<core::Message> messages;
  int maxTokens = 1024;
  int maxRetries = 2;
  bool skipSystemPromptPrefix = false;
  double temperature = 0.0;
  // 工具 Schema JSON 列表。
  // local-ace: tools?: Tool[] | BetaToolUnion[]
  // 分类器调用时携带此字段以启用结构化 tool_use 返回。
  std::vector<std::string> toolSchemas;
  // 强制工具调用名称。对应 local-ace tool_choice: { type: 'tool', name: 'x' }。
  std::string forcedToolChoice;
};

struct SideQueryResponse {
  bool ok = false;
  std::vector<core::Message> messages;
  std::string error;
};

class SideQueryClient {
 public:
  explicit SideQueryClient(ModelClient& modelClient);

  SideQueryResponse Query(const SideQueryRequest& request) const;

 private:
  ModelClient& modelClient_;
};

}  // namespace api
}  // namespace agent
