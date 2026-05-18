#include "api/ModelClient.h"
#include "third_party/nlohmann_json.hpp"

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <sstream>

namespace agent {
namespace api {

using json = nlohmann::json;

namespace {

std::wstring ToWide(const std::string& text) {
  if (text.empty()) return {};
  int sz = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                               static_cast<int>(text.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(sz), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                      static_cast<int>(text.size()), &w[0], sz);
  return w;
}

std::string ToUtf8(const std::wstring& text) {
  if (text.empty()) return {};
  int sz = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                               static_cast<int>(text.size()),
                               nullptr, 0, nullptr, nullptr);
  std::string u(static_cast<size_t>(sz), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                      static_cast<int>(text.size()),
                      &u[0], sz, nullptr, nullptr);
  return u;
}

std::string EscapeJson(const std::string& s) {
  std::ostringstream o;
  for (char c : s) {
    switch (c) {
      case '"':  o << "\\\""; break;
      case '\\': o << "\\\\"; break;
      case '\n': o << "\\n"; break;
      case '\r': o << "\\r"; break;
      case '\t': o << "\\t"; break;
      default:   o << c; break;
    }
  }
  return o.str();
}

std::string RoleToString(core::MessageRole role) {
  switch (role) {
    case core::MessageRole::User:      return "user";
    case core::MessageRole::Assistant: return "assistant";
    case core::MessageRole::System:    return "system";
  }
  return "user";
}

std::string BuildMessagesJson(const std::vector<core::Message>& msgs) {
  std::ostringstream arr;
  arr << "[";
  bool firstMsg = true;
  for (const auto& msg : msgs) {
    if (!firstMsg) arr << ",";
    firstMsg = false;
    arr << "{\"role\":\"" << RoleToString(msg.role) << "\",\"content\":[";

    bool firstBlock = true;
    for (const auto& block : msg.content) {
      if (!firstBlock) arr << ",";
      firstBlock = false;

      if (block.type == core::BlockType::Text) {
        arr << "{\"type\":\"text\",\"text\":\""
            << EscapeJson(block.asText.text) << "\"}";
      } else if (block.type == core::BlockType::ToolUse) {
        arr << "{\"type\":\"tool_use\",\"id\":\""
            << EscapeJson(block.asToolUse.id)
            << "\",\"name\":\"" << EscapeJson(block.asToolUse.name)
            << "\",\"input\":" << block.asToolUse.inputJson << "}";
      } else if (block.type == core::BlockType::ToolResult) {
        arr << "{\"type\":\"tool_result\",\"tool_use_id\":\""
            << EscapeJson(block.asToolResult.toolUseId)
            << "\",\"content\":\""
            << EscapeJson(block.asToolResult.content) << "\"";
        if (block.asToolResult.isError) arr << ",\"is_error\":true";
        arr << "}";
      }
    }
    arr << "]}";
  }
  arr << "]";
  return arr.str();
}

bool ReadHttpBody(HINTERNET req, std::string* body, std::string* error) {
  DWORD avail = 0;
  do {
    avail = 0;
    if (!WinHttpQueryDataAvailable(req, &avail)) {
      if (error) *error = "WinHttpQueryDataAvailable failed";
      return false;
    }
    if (avail == 0) break;
    std::vector<char> buf(avail);
    DWORD downloaded = 0;
    if (!WinHttpReadData(req, buf.data(), avail, &downloaded)) {
      if (error) *error = "WinHttpReadData failed";
      return false;
    }
    body->append(buf.data(), downloaded);
  } while (avail > 0);
  return true;
}

void ParseSseBody(const std::string& rawBody, const SseEventCallback& onEvent) {
  std::istringstream stream(rawBody);
  std::string line, currentEvent, currentData;
  auto flush = [&]() {
    if (!currentData.empty()) {
      if (onEvent) onEvent(currentEvent.empty() ? "text_delta" : currentEvent, currentData);
      currentEvent.clear();
      currentData.clear();
    }
  };
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) { flush(); continue; }
    if (line[0] == ':') continue;
    auto colon = line.find(':');
    auto field = colon == std::string::npos ? line : line.substr(0, colon);
    auto value = colon == std::string::npos ? "" : line.substr(colon + 1);
    if (!value.empty() && value[0] == ' ') value.erase(0, 1);
    if (field == "event") currentEvent = value;
    else if (field == "data") {
      if (!currentData.empty()) currentData.push_back('\n');
      currentData.append(value);
    }
  }
  flush();
}

std::string ExtractJsonStringValue(const std::string& jsonData,
                                   const std::string& key) {
  auto pos = jsonData.find(key);
  if (pos == std::string::npos) return std::string();
  pos += key.size();
  std::string result;
  while (pos < jsonData.size()) {
    if (jsonData[pos] == '\\' && pos + 1 < jsonData.size()) {
      char next = jsonData[pos + 1];
      if (next == 'n') { result.push_back('\n'); pos += 2; continue; }
      if (next == 'r') { result.push_back('\r'); pos += 2; continue; }
      if (next == 't') { result.push_back('\t'); pos += 2; continue; }
      if (next == '"') { result.push_back('"'); pos += 2; continue; }
      if (next == '\\') { result.push_back('\\'); pos += 2; continue; }
      result.push_back(jsonData[pos]); ++pos; continue;
    }
    if (jsonData[pos] == '"') break;
    if (jsonData[pos] == '\n' || jsonData[pos] == '\r') { ++pos; continue; }
    result.push_back(jsonData[pos]);
    ++pos;
  }
  return result;
}

std::string ExtractOpenAIDeltaContent(const std::string& jsonData) {
  std::string content = ExtractJsonStringValue(
      jsonData, "\"delta\":{\"content\":\"");
  if (!content.empty()) return content;
  return ExtractJsonStringValue(
      jsonData, "\"delta\":{\"reasoning_content\":\"");
}

std::string ExtractOpenAIDeltaToolName(const std::string& jsonData) {
  return ExtractJsonStringValue(jsonData, "\"function\":{\"name\":\"");
}

std::string ExtractOpenAIDeltaToolId(const std::string& jsonData) {
  const std::string key = "\"tool_calls\":[{\"index\":0,\"id\":\"";
  auto pos = jsonData.find(key);
  if (pos == std::string::npos) return std::string();
  pos += key.size();
  auto end = jsonData.find('"', pos);
  if (end == std::string::npos) return std::string();
  return jsonData.substr(pos, end - pos);
}

std::string ExtractOpenAIDeltaToolArgs(const std::string& jsonData) {
  return ExtractJsonStringValue(jsonData, "\"function\":{\"arguments\":\"");
}

std::string ExtractOpenAIDeltaFinishReason(const std::string& jsonData) {
  const std::string key = "\"finish_reason\":\"";
  auto pos = jsonData.find(key);
  if (pos == std::string::npos) return std::string();
  pos += key.size();
  auto end = jsonData.find('"', pos);
  if (end == std::string::npos) return std::string();
  return jsonData.substr(pos, end - pos);
}

std::string ExtractOpenAIResponseText(const std::string& jsonData) {
  json parsed = json::parse(jsonData, nullptr, false);
  if (parsed.is_discarded()) return std::string();
  if (!parsed.contains("choices") || !parsed["choices"].is_array() ||
      parsed["choices"].empty())
    return std::string();
  const json& choice = parsed["choices"][0];
  if (!choice.contains("message") || !choice["message"].is_object())
    return std::string();
  const json& message = choice["message"];
  if (message.contains("content") && message["content"].is_string() &&
      !message["content"].get<std::string>().empty())
    return message["content"].get<std::string>();
  if (message.contains("reasoning_content") &&
      message["reasoning_content"].is_string())
    return message["reasoning_content"].get<std::string>();
  return std::string();
}

void ParseOpenAISseDelta(const std::string& rawBody,
                         const SseEventCallback& onEvent) {
  std::string pendingToolName;
  std::string pendingToolArgs;
  std::string pendingToolId;

  auto emitPendingToolUse = [&]() {
    if (pendingToolName.empty() || pendingToolArgs.empty()) return;
    std::ostringstream toolEvent;
    toolEvent << "{\"id\":\"" << pendingToolId << "\",\"name\":\""
              << EscapeJson(pendingToolName) << "\",\"input\":"
              << pendingToolArgs << "}";
    if (onEvent) onEvent("tool_use", toolEvent.str());
    pendingToolName.clear();
    pendingToolArgs.clear();
    pendingToolId.clear();
  };

  SseEventCallback wrapper = [&](const std::string& /*event*/, const std::string& data) {
    if (data == "[DONE]") {
      emitPendingToolUse();
      if (onEvent) onEvent("stop_reason", "stop");
      return;
    }

    json parsed = json::parse(data, nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("choices") ||
        !parsed["choices"].is_array() || parsed["choices"].empty())
      return;

    const json& choice = parsed["choices"][0];
    if (choice.contains("delta") && choice["delta"].is_object()) {
      const json& delta = choice["delta"];
      std::string textDelta;
      if (delta.contains("content") && delta["content"].is_string())
        textDelta = delta["content"].get<std::string>();
      else if (delta.contains("reasoning_content") &&
               delta["reasoning_content"].is_string())
        textDelta = delta["reasoning_content"].get<std::string>();

      if (!textDelta.empty()) {
        emitPendingToolUse();
        if (onEvent) onEvent("text_delta", textDelta);
      }

      if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
        for (const auto& toolCall : delta["tool_calls"]) {
          if (!toolCall.is_object()) continue;
          if (toolCall.contains("id") && toolCall["id"].is_string())
            pendingToolId = toolCall["id"].get<std::string>();
          else if (pendingToolId.empty())
            pendingToolId = "oaitu-" + std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());

          if (toolCall.contains("function") && toolCall["function"].is_object()) {
            const json& fn = toolCall["function"];
            if (fn.contains("name") && fn["name"].is_string())
              pendingToolName = fn["name"].get<std::string>();
            if (fn.contains("arguments") && fn["arguments"].is_string())
              pendingToolArgs += fn["arguments"].get<std::string>();
          }
        }
      }
    }

    if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
      const std::string finishReason = choice["finish_reason"].get<std::string>();
      if (!finishReason.empty() && finishReason != "null") {
        emitPendingToolUse();
        if (onEvent) onEvent("stop_reason", finishReason);
      }
    }
  };
  ParseSseBody(rawBody, wrapper);

  emitPendingToolUse();
}

}  // namespace

void ModelClient::StreamResponse(
    const std::vector<core::Message>&,
    const std::string&,
    const std::string&,
    const std::string&,
    const SseEventCallback&) {}

SkeletonModelClient::SkeletonModelClient() : callCount_(0) {}

std::vector<core::Message> SkeletonModelClient::GenerateResponse(
    const std::vector<core::Message>&,
    const std::string&,
    const std::string&) {
  ++callCount_;
  if (callCount_ == 1) {
    core::Message a;
    a.role = core::MessageRole::Assistant;
    a.uuid = "skel-asst-001";
    a.usage = {64, 12};
    a.content.push_back(core::ContentBlock::MakeText("Skeleton response."));
    core::Message b;
    b.role = core::MessageRole::Assistant;
    b.uuid = "skel-asst-002";
    b.usage = {64, 8};
    b.content.push_back(core::ContentBlock::MakeToolUse("skel-tu-001", "FileRead",
                                                         R"({"path":"README.md"})"));
    return {a, b};
  }
  core::Message f;
  f.role = core::MessageRole::Assistant;
  f.uuid = "skel-asst-final";
  f.usage = {40, 6};
  f.stopReason = "end_turn";
  f.content.push_back(core::ContentBlock::MakeText("Skeleton final."));
  return {f};
}

void SkeletonModelClient::StreamResponse(
    const std::vector<core::Message>&,
    const std::string&,
    const std::string&,
    const std::string&,
    const SseEventCallback& onEvent) {
  ++callCount_;
  if (!onEvent) return;
  if (callCount_ == 1) {
    onEvent("text_delta", "SkeletonStream response. ");
    onEvent("tool_use", R"({"id":"skel-tu-001","name":"FileRead","input":{"path":"README.md"}})");
    onEvent("text_delta", "Tool dispatched.");
  } else {
    onEvent("text_delta", "SkeletonStream final.");
    onEvent("stop_reason", "end_turn");
  }
}

std::vector<core::Message> SkeletonModelClient::SideQuery(
    const std::vector<core::Message>&,
    const std::string&,
    const std::string&) {
  core::Message r;
  r.role = core::MessageRole::Assistant;
  r.uuid = "skel-side-001";
  r.usage = {16, 4};
  r.content.push_back(core::ContentBlock::MakeText("Skeleton side-query."));
  return {r};
}

HttpLlmClient::HttpLlmClient(const core::LlmConfig& config)
    : config_(config),
      isNativeAnthropic_(IsNativeAnthropicEndpoint(config.apiEndpoint)) {}

bool HttpLlmClient::IsNativeAnthropicEndpoint(const std::string& endpoint) {
  return endpoint.find("api.anthropic.com") != std::string::npos;
}

bool HttpLlmClient::IsOpenAICompatibleEndpoint(const std::string& endpoint) {
  return !IsNativeAnthropicEndpoint(endpoint);
}

std::string HttpLlmClient::BuildAnthropicBody(
    const std::vector<core::Message>& messages,
    const std::string& systemPrompt,
    const std::string& model,
    int maxTokens, bool stream) const {
  std::ostringstream body;
  body << "{"
       << "\"model\":\"" << EscapeJson(model) << "\","
       << "\"max_tokens\":" << maxTokens << ","
       << "\"system\":\"" << EscapeJson(systemPrompt) << "\","
       << "\"messages\":" << BuildMessagesJson(messages);
  if (stream) body << ",\"stream\":true";
  body << "}";
  return body.str();
}

std::string HttpLlmClient::BuildOpenAIBody(
    const std::vector<core::Message>& messages,
    const std::string& systemPrompt,
    const std::string& model,
    int maxTokens, bool stream) const {
  return BuildAnthropicBody(messages, systemPrompt, model, maxTokens, stream);
}

std::string HttpLlmClient::BuildRequestBody(
    const std::vector<core::Message>& messages,
    const std::string& systemPrompt,
    const std::string& model,
    int maxTokens, bool stream,
    const std::string& toolsJson) const {
  std::ostringstream body;
  body << "{"
       << "\"model\":\"" << EscapeJson(model) << "\","
       << "\"max_tokens\":" << maxTokens << ","
       << "\"stream\":" << (stream ? "true" : "false") << ","
       << "\"system\":\"" << EscapeJson(systemPrompt) << "\","
       << "\"messages\":" << BuildMessagesJson(messages);
  if (!toolsJson.empty()) {
    body << ",\"tools\":" << toolsJson;
  }
  body << "}";
  return body.str();
}

std::string HttpLlmClient::SendHttpPost(const std::string& body,
                                        const std::string& /*model*/,
                                        std::string* pathOverride,
                                        std::string* error) const {
  std::wstring host, path;
  INTERNET_PORT port = INTERNET_DEFAULT_HTTP_PORT;
  bool secure = false;

  std::string ep = config_.apiEndpoint;
  std::wstring wideEp = ToWide(ep);

  URL_COMPONENTS components;
  ZeroMemory(&components, sizeof(components));
  components.dwStructSize = sizeof(components);
  wchar_t hn[256] = {0}, up[2048] = {0};
  components.lpszHostName = hn;
  components.dwHostNameLength = 256;
  components.lpszUrlPath = up;
  components.dwUrlPathLength = 2048;

  if (!WinHttpCrackUrl(wideEp.c_str(), 0, 0, &components)) {
    if (error) *error = "WinHttpCrackUrl failed for endpoint: " + ep;
    return {};
  }

  secure = (components.nScheme == INTERNET_SCHEME_HTTPS);
  port = components.nPort;
  host = std::wstring(components.lpszHostName, components.dwHostNameLength);
  path = std::wstring(components.lpszUrlPath, components.dwUrlPathLength);

  HINTERNET session = WinHttpOpen(L"cpp-agent/0.2.0",
      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) { if (error) *error = "WinHttpOpen failed"; return {}; }

  WinHttpSetTimeouts(session, config_.connectTimeoutMs,
                     config_.connectTimeoutMs,
                     config_.requestTimeoutMs,
                     config_.requestTimeoutMs);

  HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
  if (!connect) {
    if (error) *error = "WinHttpConnect failed";
    WinHttpCloseHandle(session);
    return {};
  }

  std::wstring requestPath = path;
  if (pathOverride && !pathOverride->empty()) {
    requestPath = ToWide(*pathOverride);
  } else if (isNativeAnthropic_) {
    requestPath = L"/v1/messages";
  }

  HINTERNET req = WinHttpOpenRequest(connect, L"POST", requestPath.c_str(),
      nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
      secure ? WINHTTP_FLAG_SECURE : 0);
  if (!req) {
    if (error) *error = "WinHttpOpenRequest failed";
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return {};
  }

  std::wstring headers = L"Content-Type: application/json\r\n";
  if (!config_.apiKey.empty()) {
    headers += L"x-api-key: " + ToWide(config_.apiKey) + L"\r\n";
    headers += L"Authorization: Bearer " + ToWide(config_.apiKey) + L"\r\n";
  }
  headers += L"Accept: application/json, text/event-stream\r\n";
  WinHttpAddRequestHeaders(req, headers.c_str(),
                           static_cast<DWORD>(headers.size()),
                           WINHTTP_ADDREQ_FLAG_ADD);

  if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          const_cast<char*>(body.data()),
                          static_cast<DWORD>(body.size()),
                          static_cast<DWORD>(body.size()), 0)) {
    if (error) *error = "WinHttpSendRequest failed";
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return {};
  }

  if (!WinHttpReceiveResponse(req, nullptr)) {
    if (error) *error = "WinHttpReceiveResponse failed";
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return {};
  }

  DWORD statusCode = 0;
  DWORD statusSize = sizeof(statusCode);
  WinHttpQueryHeaders(req,
      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
      WINHTTP_NO_HEADER_INDEX);

  std::string responseBody;
  bool ok = ReadHttpBody(req, &responseBody, error);

  WinHttpCloseHandle(req);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);

  if (!ok) return {};

  if (statusCode >= 400) {
    if (error) {
      *error = "HTTP " + std::to_string(static_cast<int>(statusCode)) +
               " from LLM endpoint";
    }
    return {};
  }

  return responseBody;
}

std::vector<core::Message> HttpLlmClient::GenerateResponse(
    const std::vector<core::Message>& messages,
    const std::string& systemPrompt,
    const std::string& model) {
  std::string actualModel = model.empty() ? config_.mainModel : model;
  std::string body = BuildRequestBody(messages, systemPrompt, actualModel,
                                      4096, false, "");
  std::string error;
  std::string raw = SendHttpPost(body, actualModel, nullptr, &error);
  (void)raw;
  if (!error.empty()) {
    core::Message errMsg;
    errMsg.role = core::MessageRole::Assistant;
    errMsg.uuid = "http-err";
    errMsg.isApiErrorMessage = true;
    errMsg.content.push_back(core::ContentBlock::MakeText("LLM API error: " + error));
    return {errMsg};
  }
  core::Message response;
  response.role = core::MessageRole::Assistant;
  response.uuid = "http-generate";
  response.content.push_back(core::ContentBlock::MakeText(
      ExtractOpenAIResponseText(raw)));
  return {response};
}

void HttpLlmClient::StreamResponse(
    const std::vector<core::Message>& messages,
    const std::string& systemPrompt,
    const std::string& model,
    const std::string& toolsJson,
    const SseEventCallback& onEvent) {
  std::string actualModel = model.empty() ? config_.mainModel : model;
  std::string body = BuildRequestBody(messages, systemPrompt, actualModel,
                                      4096, true, toolsJson);
  std::string error;
  std::string raw = SendHttpPost(body, actualModel, nullptr, &error);
  if (!error.empty()) {
    if (onEvent) onEvent("api_error", error);
    return;
  }
  if (isNativeAnthropic_) {
    ParseSseBody(raw, onEvent);
  } else {
    ParseOpenAISseDelta(raw, onEvent);
  }
}

std::vector<core::Message> HttpLlmClient::SideQuery(
    const std::vector<core::Message>& messages,
    const std::string& systemPrompt,
    const std::string& model) {
  std::string actualModel = model.empty() ? config_.validatorModel : model;
  if (actualModel.empty()) actualModel = config_.mainModel;
  std::string body = BuildRequestBody(messages, systemPrompt, actualModel,
                                      1024, false, "");
  std::string error;
  std::string raw = SendHttpPost(body, actualModel, nullptr, &error);
  if (!error.empty()) {
    core::Message errMsg;
    errMsg.role = core::MessageRole::Assistant;
    errMsg.uuid = "side-err";
    errMsg.isApiErrorMessage = true;
    errMsg.content.push_back(core::ContentBlock::MakeText("Side query error: " + error));
    return {errMsg};
  }
  core::Message r;
  r.role = core::MessageRole::Assistant;
  r.uuid = "side-resp";
  const std::string parsed = ExtractOpenAIResponseText(raw);
  r.content.push_back(core::ContentBlock::MakeText(
      parsed.empty() ? raw : parsed));
  return {r};
}

}  // namespace api
}  // namespace agent
