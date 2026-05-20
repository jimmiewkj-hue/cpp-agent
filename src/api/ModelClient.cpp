#include "api/ModelClient.h"
#include "third_party/nlohmann_json.hpp"

#include <fstream>
#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <sstream>

namespace agent {
namespace api {

using json = nlohmann::json;

namespace {

std::wstring ToWide(const std::string& text);
std::string ToUtf8(const std::wstring& text);

// #region debug-point A:runtime-reporting
struct DebugServerConfig {
  std::string url = "http://127.0.0.1:7777/event";
  std::string sessionId = "stream-response-stall";
};

std::string TrimDebugValue(const std::string& value) {
  std::size_t start = 0;
  std::size_t end = value.size();
  while (start < end &&
         (value[start] == ' ' || value[start] == '\r' || value[start] == '\n' ||
          value[start] == '\t')) {
    ++start;
  }
  while (end > start &&
         (value[end - 1] == ' ' || value[end - 1] == '\r' ||
          value[end - 1] == '\n' || value[end - 1] == '\t')) {
    --end;
  }
  return value.substr(start, end - start);
}

DebugServerConfig LoadDebugServerConfig() {
  DebugServerConfig cfg;
  char envUrl[512] = {0};
  DWORD envUrlLen = GetEnvironmentVariableA(
      "DEBUG_SERVER_URL", envUrl, sizeof(envUrl));
  if (envUrlLen > 0 && envUrlLen < sizeof(envUrl)) {
    cfg.url.assign(envUrl, envUrlLen);
  }
  char envSession[256] = {0};
  DWORD envSessionLen = GetEnvironmentVariableA(
      "DEBUG_SESSION_ID", envSession, sizeof(envSession));
  if (envSessionLen > 0 && envSessionLen < sizeof(envSession)) {
    cfg.sessionId.assign(envSession, envSessionLen);
  }

  std::ifstream in(".dbg\\stream-response-stall.env", std::ios::binary);
  if (!in) return cfg;

  std::string line;
  while (std::getline(in, line)) {
    line = TrimDebugValue(line);
    if (line.rfind("DEBUG_SERVER_URL=", 0) == 0) {
      cfg.url = line.substr(17);
    } else if (line.rfind("DEBUG_SESSION_ID=", 0) == 0) {
      cfg.sessionId = line.substr(17);
    }
  }
  return cfg;
}

std::string TruncateDebugText(const std::string& text, std::size_t maxLen = 240) {
  if (text.size() <= maxLen) return text;
  return text.substr(0, maxLen) + "...";
}

std::string MakeDebugTraceId(const std::string& prefix) {
  const long long nowMs = static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  return prefix + "-" + std::to_string(nowMs);
}

void ReportDebugEvent(const std::string& runId,
                      const std::string& hypothesisId,
                      const std::string& location,
                      const std::string& msg,
                      const json& data,
                      const std::string& traceId = std::string()) {
  const DebugServerConfig cfg = LoadDebugServerConfig();
  if (cfg.url.empty() || cfg.sessionId.empty()) return;

  json payload;
  payload["sessionId"] = cfg.sessionId;
  payload["runId"] = runId;
  payload["hypothesisId"] = hypothesisId;
  payload["location"] = location;
  payload["msg"] = msg;
  payload["data"] = data;
  payload["ts"] = static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  if (!traceId.empty()) payload["traceId"] = traceId;
  const std::string body = payload.dump(-1, ' ', false,
                                        json::error_handler_t::replace);

  URL_COMPONENTS components;
  ZeroMemory(&components, sizeof(components));
  components.dwStructSize = sizeof(components);
  wchar_t hostName[256] = {0};
  wchar_t urlPath[1024] = {0};
  std::wstring wideUrl = ToWide(cfg.url);
  components.lpszHostName = hostName;
  components.dwHostNameLength = sizeof(hostName) / sizeof(hostName[0]);
  components.lpszUrlPath = urlPath;
  components.dwUrlPathLength = sizeof(urlPath) / sizeof(urlPath[0]);
  if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components)) return;

  const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
  const std::wstring host(components.lpszHostName, components.dwHostNameLength);
  const std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);

  HINTERNET session = WinHttpOpen(L"cpp-agent-debug/0.1",
      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return;
  WinHttpSetTimeouts(session, 500, 500, 1000, 1000);

  HINTERNET connect =
      WinHttpConnect(session, host.c_str(), components.nPort, 0);
  if (!connect) {
    WinHttpCloseHandle(session);
    return;
  }

  HINTERNET req = WinHttpOpenRequest(
      connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
  if (!req) {
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return;
  }

  const std::wstring headers = L"Content-Type: application/json\r\n";
  WinHttpAddRequestHeaders(req, headers.c_str(),
                           static_cast<DWORD>(headers.size()),
                           WINHTTP_ADDREQ_FLAG_ADD);
  WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                     const_cast<char*>(body.data()),
                     static_cast<DWORD>(body.size()),
                     static_cast<DWORD>(body.size()), 0);
  WinHttpReceiveResponse(req, nullptr);
  WinHttpCloseHandle(req);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
}
// #endregion

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

std::string JoinTextBlocks(const core::Message& msg) {
  std::ostringstream joined;
  bool first = true;
  for (const auto& block : msg.content) {
    if (block.type != core::BlockType::Text) continue;
    if (!first) joined << "\n";
    first = false;
    joined << block.asText.text;
  }
  return joined.str();
}

json ParseJsonOrFallbackObject(const std::string& rawJson) {
  json parsed = json::parse(rawJson, nullptr, false);
  return parsed.is_discarded() ? json::object() : parsed;
}

json ConvertOpenAIToolsToAnthropic(const std::string& toolsJson) {
  if (toolsJson.empty()) return json::array();

  json parsed = json::parse(toolsJson, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_array()) return json::array();

  json converted = json::array();
  for (const auto& tool : parsed) {
    if (!tool.is_object()) continue;
    json anthropicTool;
    if (tool.contains("function") && tool["function"].is_object()) {
      const json& fn = tool["function"];
      anthropicTool["name"] = fn.value("name", "");
      anthropicTool["description"] = fn.value("description", "");
      anthropicTool["input_schema"] =
          fn.contains("parameters") ? fn["parameters"] : json::object();
    } else {
      anthropicTool["name"] = tool.value("name", "");
      anthropicTool["description"] = tool.value("description", "");
      anthropicTool["input_schema"] =
          tool.contains("input_schema") ? tool["input_schema"] : json::object();
    }
    if (!anthropicTool["name"].get<std::string>().empty()) {
      converted.push_back(anthropicTool);
    }
  }
  return converted;
}

json BuildOpenAIChatMessages(const std::vector<core::Message>& msgs,
                             const std::string& systemPrompt) {
  json arr = json::array();

  if (!systemPrompt.empty()) {
    arr.push_back({
        {"role", "system"},
        {"content", systemPrompt},
    });
  }

  for (const auto& msg : msgs) {
    if (msg.role == core::MessageRole::System) {
      const std::string systemText = JoinTextBlocks(msg);
      if (!systemText.empty()) {
        arr.push_back({
            {"role", "system"},
            {"content", systemText},
        });
      }
      continue;
    }

    if (msg.role == core::MessageRole::User) {
      const std::string userText = JoinTextBlocks(msg);
      if (!userText.empty()) {
        arr.push_back({
            {"role", "user"},
            {"content", userText},
        });
      }
      for (const auto& block : msg.content) {
        if (block.type != core::BlockType::ToolResult) continue;
        arr.push_back({
            {"role", "tool"},
            {"tool_call_id", block.asToolResult.toolUseId},
            {"content", block.asToolResult.content},
        });
      }
      continue;
    }

    if (msg.role == core::MessageRole::Assistant) {
      json assistant = {
          {"role", "assistant"},
      };
      const std::string assistantText = JoinTextBlocks(msg);
      assistant["content"] = assistantText.empty() ? json(nullptr)
                                                   : json(assistantText);

      json toolCalls = json::array();
      for (const auto& block : msg.content) {
        if (block.type != core::BlockType::ToolUse) continue;
        toolCalls.push_back({
            {"id", block.asToolUse.id},
            {"type", "function"},
            {"function", {
                 {"name", block.asToolUse.name},
                 {"arguments", block.asToolUse.inputJson.empty()
                                   ? "{}"
                                   : block.asToolUse.inputJson},
             }},
        });
      }
      if (!toolCalls.empty()) {
        assistant["tool_calls"] = toolCalls;
      }

      if (!assistantText.empty() || !toolCalls.empty()) {
        arr.push_back(assistant);
      }
    }
  }

  return arr;
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

std::string QueryHeaderUtf8(HINTERNET req, DWORD queryFlag) {
  DWORD sizeBytes = 0;
  WinHttpQueryHeaders(req, queryFlag, WINHTTP_HEADER_NAME_BY_INDEX,
                      WINHTTP_NO_OUTPUT_BUFFER, &sizeBytes,
                      WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || sizeBytes == 0) {
    return {};
  }
  std::wstring buffer(sizeBytes / sizeof(wchar_t), L'\0');
  if (!WinHttpQueryHeaders(req, queryFlag, WINHTTP_HEADER_NAME_BY_INDEX,
                           &buffer[0], &sizeBytes,
                           WINHTTP_NO_HEADER_INDEX)) {
    return {};
  }
  buffer.resize(sizeBytes / sizeof(wchar_t));
  while (!buffer.empty() && buffer.back() == L'\0') buffer.pop_back();
  return ToUtf8(buffer);
}

std::vector<core::Message> CollectStreamedTextResponse(
    HttpLlmClient* client,
    const std::vector<core::Message>& messages,
    const std::string& systemPrompt,
    const std::string& model,
    int maxTokens) {
  core::Message response;
  response.role = core::MessageRole::Assistant;
  response.uuid = "http-stream-aggregate";
  std::ostringstream text;
  bool sawApiError = false;

  client->StreamResponse(
      messages, systemPrompt, model, "",
      [&](const std::string& event, const std::string& data) {
        if (event == "text_delta") {
          text << data;
        } else if (event == "stop_reason") {
          response.stopReason = data;
        } else if (event == "api_error") {
          sawApiError = true;
          response.isApiErrorMessage = true;
          response.content.clear();
          response.content.push_back(core::ContentBlock::MakeText(
              "LLM API error: " + data));
        }
      },
      maxTokens);

  if (!sawApiError) {
    response.content.push_back(core::ContentBlock::MakeText(text.str()));
  }
  return {response};
}

void ParseSseBody(const std::string& rawBody, const SseEventCallback& onEvent);

void ParseAnthropicSse(const std::string& rawBody,
                       const SseEventCallback& onEvent) {
  std::string textBlock;
  std::string toolId;
  std::string toolName;
  std::string toolInput;
  int contentBlockIndex = -1;
  std::string currentBlockType;

  auto flushTextBlock = [&]() {
    if (!textBlock.empty()) {
      if (onEvent) onEvent("text_delta", textBlock);
      textBlock.clear();
    }
  };

  auto flushToolBlock = [&]() {
    if (!toolName.empty() && !toolInput.empty()) {
      std::ostringstream toolEvent;
      toolEvent << "{\"id\":\"" << toolId << "\",\"name\":\""
                << EscapeJson(toolName) << "\",\"input\":"
                << (toolInput.empty() ? "{}" : toolInput) << "}";
      if (onEvent) onEvent("tool_use", toolEvent.str());
    }
    toolId.clear();
    toolName.clear();
    toolInput.clear();
  };

  SseEventCallback sseWrapper = [&](const std::string& event, const std::string& data) {
    if (data.empty()) return;

    json parsed = json::parse(data, nullptr, false);
    if (parsed.is_discarded()) return;

    if (event == "message_start") {
      return;
    }

    if (event == "content_block_start") {
      flushTextBlock();
      flushToolBlock();

      if (!parsed.contains("type")) return;
      std::string type = parsed["type"].get<std::string>();

      if (type == "thinking") {
        currentBlockType = type;
        return;
      }

      currentBlockType = type;

      if (parsed.contains("index") && parsed["index"].is_number())
        contentBlockIndex = parsed["index"].get<int>();

      if (type == "tool_use") {
        if (parsed.contains("id") && parsed["id"].is_string())
          toolId = parsed["id"].get<std::string>();
        if (parsed.contains("name") && parsed["name"].is_string())
          toolName = parsed["name"].get<std::string>();
        if (parsed.contains("input") && parsed["input"].is_object())
          toolInput = parsed["input"].dump();
      }
      return;
    }

    if (event == "content_block_delta") {
      if (!parsed.contains("type")) return;
      std::string deltaType = parsed["type"].get<std::string>();

      if (deltaType == "thinking_delta") return;

      if (deltaType == "text_delta" &&
          parsed.contains("text") && parsed["text"].is_string()) {
        textBlock.append(parsed["text"].get<std::string>());
      } else if (deltaType == "input_json_delta" &&
                 parsed.contains("partial_json") &&
                 parsed["partial_json"].is_string()) {
        toolInput.append(parsed["partial_json"].get<std::string>());
      }
      return;
    }

    if (event == "content_block_stop") {
      if (currentBlockType == "thinking") {
        currentBlockType.clear();
        return;
      }
      if (currentBlockType == "text") {
        flushTextBlock();
      } else if (currentBlockType == "tool_use") {
        flushToolBlock();
      }
      currentBlockType.clear();
      return;
    }

    if (event == "message_delta") {
      if (parsed.contains("stop_reason") && parsed["stop_reason"].is_string()) {
        flushTextBlock();
        flushToolBlock();
        if (onEvent) onEvent("stop_reason", parsed["stop_reason"].get<std::string>());
      }
      return;
    }

    if (event == "message_stop") {
      flushTextBlock();
      flushToolBlock();
      return;
    }
  };

  ParseSseBody(rawBody, sseWrapper);

  flushTextBlock();
  flushToolBlock();
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

std::string ExtractAnthropicResponseText(const std::string& jsonData) {
  json parsed = json::parse(jsonData, nullptr, false);
  if (parsed.is_discarded()) return std::string();
  if (!parsed.contains("content") || !parsed["content"].is_array()) {
    return std::string();
  }

  std::ostringstream text;
  for (const auto& block : parsed["content"]) {
    if (!block.is_object() || !block.contains("type") || !block["type"].is_string()) {
      continue;
    }
    const std::string type = block["type"].get<std::string>();
    if (type == "text" && block.contains("text") && block["text"].is_string()) {
      text << block["text"].get<std::string>();
    } else if (type == "thinking" &&
               block.contains("thinking") && block["thinking"].is_string()) {
      // Some local Anthropic-compatible servers emit only thinking blocks.
      text << block["thinking"].get<std::string>();
    } else if (type == "tool_use" && block.contains("input")) {
      text << "[tool_use] " << block.value("name", "");
    }
  }
  return text.str();
}

bool LooksLikeJsonPayload(const std::string& rawBody) {
  for (char ch : rawBody) {
    if (ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t') continue;
    return ch == '{' || ch == '[';
  }
  return false;
}

void EmitOpenAIJsonResponseEvents(const std::string& jsonData,
                                  const SseEventCallback& onEvent) {
  json parsed = json::parse(jsonData, nullptr, false);
  if (parsed.is_discarded() || !parsed.contains("choices") ||
      !parsed["choices"].is_array() || parsed["choices"].empty()) {
    return;
  }

  const json& choice = parsed["choices"][0];
  if (choice.contains("message") && choice["message"].is_object()) {
    const json& message = choice["message"];
    if (message.contains("reasoning_content") &&
        message["reasoning_content"].is_string()) {
      const std::string reasoning = message["reasoning_content"].get<std::string>();
      if (!reasoning.empty() && onEvent) onEvent("text_delta", reasoning);
    }
    if (message.contains("content") && message["content"].is_string()) {
      const std::string content = message["content"].get<std::string>();
      if (!content.empty() && onEvent) onEvent("text_delta", content);
    }
    if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
      for (const auto& toolCall : message["tool_calls"]) {
        if (!toolCall.is_object()) continue;
        const std::string id = toolCall.value("id", "");
        if (!toolCall.contains("function") || !toolCall["function"].is_object()) {
          continue;
        }
        const json& fn = toolCall["function"];
        const std::string name = fn.value("name", "");
        std::string args = "{}";
        if (fn.contains("arguments")) {
          if (fn["arguments"].is_string()) {
            args = fn["arguments"].get<std::string>();
          } else {
            args = fn["arguments"].dump();
          }
        }
        std::ostringstream toolEvent;
        toolEvent << "{\"id\":\"" << EscapeJson(id) << "\",\"name\":\""
                  << EscapeJson(name) << "\",\"input\":"
                  << (args.empty() ? "{}" : args) << "}";
        if (onEvent) onEvent("tool_use", toolEvent.str());
      }
    }
  }

  if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
    const std::string finishReason = choice["finish_reason"].get<std::string>();
    if (!finishReason.empty() && onEvent) onEvent("stop_reason", finishReason);
  } else if (onEvent) {
    onEvent("stop_reason", "stop");
  }
}

void EmitAnthropicJsonResponseEvents(const std::string& jsonData,
                                     const SseEventCallback& onEvent) {
  json parsed = json::parse(jsonData, nullptr, false);
  if (parsed.is_discarded() || !parsed.contains("content") ||
      !parsed["content"].is_array()) {
    return;
  }

  for (const auto& block : parsed["content"]) {
    if (!block.is_object() || !block.contains("type") || !block["type"].is_string()) {
      continue;
    }
    const std::string type = block["type"].get<std::string>();
    if (type == "text" && block.contains("text") && block["text"].is_string()) {
      if (onEvent) onEvent("text_delta", block["text"].get<std::string>());
    } else if (type == "thinking" && block.contains("thinking") &&
               block["thinking"].is_string()) {
      if (onEvent) onEvent("text_delta", block["thinking"].get<std::string>());
    } else if (type == "tool_use") {
      const std::string id = block.value("id", "");
      const std::string name = block.value("name", "");
      const std::string input = block.contains("input") ? block["input"].dump() : "{}";
      std::ostringstream toolEvent;
      toolEvent << "{\"id\":\"" << EscapeJson(id) << "\",\"name\":\""
                << EscapeJson(name) << "\",\"input\":"
                << input << "}";
      if (onEvent) onEvent("tool_use", toolEvent.str());
    }
  }

  const std::string stopReason = parsed.value("stop_reason", "stop");
  if (onEvent) onEvent("stop_reason", stopReason);
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
    const SseEventCallback&,
    int) {}

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
    const SseEventCallback& onEvent,
    int) {
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
  return endpoint.find("api.anthropic.com") != std::string::npos ||
         endpoint.find("/v1/messages") != std::string::npos;
}

bool HttpLlmClient::IsOpenAICompatibleEndpoint(const std::string& endpoint) {
  return !IsNativeAnthropicEndpoint(endpoint);
}

std::string HttpLlmClient::BuildAnthropicBody(
    const std::vector<core::Message>& messages,
    const std::string& systemPrompt,
    const std::string& model,
    int maxTokens, bool stream,
    const std::string& toolsJson,
    double temperature) const {
  json body;
  body["model"] = model;
  body["max_tokens"] = maxTokens;
  body["system"] = systemPrompt;
  body["messages"] = json::parse(BuildMessagesJson(messages), nullptr, false);
  if (body["messages"].is_discarded()) {
    body["messages"] = json::array();
  }
  if (stream) body["stream"] = true;

  const json anthropicTools = ConvertOpenAIToolsToAnthropic(toolsJson);
  if (!anthropicTools.empty()) {
    body["tools"] = anthropicTools;
  }
  if (temperature >= 0.0) {
    body["temperature"] = temperature;
  }

  return body.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string HttpLlmClient::BuildOpenAIBody(
    const std::vector<core::Message>& messages,
    const std::string& systemPrompt,
    const std::string& model,
    int maxTokens, bool stream,
    const std::string& toolsJson,
    double temperature) const {
  json body;
  body["model"] = model;
  body["max_tokens"] = maxTokens;
  body["stream"] = stream;
  body["messages"] = BuildOpenAIChatMessages(messages, systemPrompt);
  if (temperature >= 0.0) {
    body["temperature"] = temperature;
  }

  if (!toolsJson.empty()) {
    json tools = json::parse(toolsJson, nullptr, false);
    if (!tools.is_discarded() && tools.is_array() && !tools.empty()) {
      body["tools"] = tools;
      body["tool_choice"] = "auto";
    }
  }

  return body.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string HttpLlmClient::BuildRequestBody(
    const std::vector<core::Message>& messages,
    const std::string& systemPrompt,
    const std::string& model,
    int maxTokens, bool stream,
    const std::string& toolsJson,
    double temperature) const {
  if (isNativeAnthropic_) {
    return BuildAnthropicBody(
        messages, systemPrompt, model, maxTokens, stream, toolsJson,
        temperature);
  }
  return BuildOpenAIBody(
      messages, systemPrompt, model, maxTokens, stream, toolsJson,
      temperature);
}

std::string HttpLlmClient::SendHttpPost(const std::string& body,
                                        const std::string& model,
                                        std::string* pathOverride,
                                        std::string* error) const {
  const auto start = std::chrono::steady_clock::now();
  const std::string traceId = MakeDebugTraceId("http");
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
    // #region debug-point A:crack-url-failed
    ReportDebugEvent("pre-fix", "A", "ModelClient.cpp:send-http:crack-url",
                     "[DEBUG] WinHttpCrackUrl failed",
                     {{"endpoint", ep}, {"model", model}}, traceId);
    // #endregion
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

  // #region debug-point A:http-request-start
  ReportDebugEvent("pre-fix", "A", "ModelClient.cpp:send-http:start",
                   "[DEBUG] SendHttpPost start",
                   {{"endpoint", ep},
                    {"model", model},
                    {"bodySize", static_cast<int>(body.size())},
                    {"secure", secure},
                    {"port", static_cast<int>(port)},
                    {"requestPath", ToUtf8(requestPath)},
                    {"connectTimeoutMs", config_.connectTimeoutMs},
                    {"requestTimeoutMs", config_.requestTimeoutMs}},
                   traceId);
  // #endregion

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
    // #region debug-point A:send-request-failed
    ReportDebugEvent("pre-fix", "A", "ModelClient.cpp:send-http:send-failed",
                     "[DEBUG] WinHttpSendRequest failed",
                     {{"lastError", static_cast<int>(GetLastError())},
                      {"elapsedMs", static_cast<long long>(
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start).count())}},
                     traceId);
    // #endregion
    if (error) *error = "WinHttpSendRequest failed";
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return {};
  }

  if (!WinHttpReceiveResponse(req, nullptr)) {
    // #region debug-point A:receive-response-failed
    ReportDebugEvent("pre-fix", "A", "ModelClient.cpp:send-http:receive-failed",
                     "[DEBUG] WinHttpReceiveResponse failed",
                     {{"lastError", static_cast<int>(GetLastError())},
                      {"elapsedMs", static_cast<long long>(
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start).count())}},
                     traceId);
    // #endregion
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
  const std::string contentType =
      QueryHeaderUtf8(req, WINHTTP_QUERY_CONTENT_TYPE);

  WinHttpCloseHandle(req);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);

  if (!ok) return {};

  // #region debug-point B:http-response-received
  ReportDebugEvent("pre-fix", "B", "ModelClient.cpp:send-http:response",
                   "[DEBUG] SendHttpPost received response body",
                   {{"statusCode", static_cast<int>(statusCode)},
                    {"contentType", contentType},
                    {"responseSize", static_cast<int>(responseBody.size())},
                    {"jsonLike", LooksLikeJsonPayload(responseBody)},
                    {"responsePrefix", TruncateDebugText(responseBody)},
                    {"elapsedMs", static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start).count())}},
                   traceId);
  // #endregion

  if (statusCode >= 400) {
    // #region debug-point C:http-status-error
    ReportDebugEvent("pre-fix", "C", "ModelClient.cpp:send-http:http-error",
                     "[DEBUG] LLM endpoint returned HTTP error",
                     {{"statusCode", static_cast<int>(statusCode)},
                      {"responsePrefix", TruncateDebugText(responseBody)}},
                     traceId);
    // #endregion
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
  if (!isNativeAnthropic_) {
    return CollectStreamedTextResponse(
        this, messages, systemPrompt, actualModel, 4096);
  }
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
  const std::string parsedText = isNativeAnthropic_
      ? ExtractAnthropicResponseText(raw)
      : ExtractOpenAIResponseText(raw);
  response.content.push_back(core::ContentBlock::MakeText(parsedText));
  return {response};
}

void HttpLlmClient::StreamResponse(
    const std::vector<core::Message>& messages,
    const std::string& systemPrompt,
    const std::string& model,
    const std::string& toolsJson,
    const SseEventCallback& onEvent,
    int maxTokensOverride) {
  const auto start = std::chrono::steady_clock::now();
  const std::string traceId = MakeDebugTraceId("stream");
  int textEventCount = 0;
  int toolEventCount = 0;
  int apiErrorCount = 0;
  int stopEventCount = 0;
  int totalEventCount = 0;
  SseEventCallback instrumentedOnEvent =
      [&](const std::string& event, const std::string& data) {
        ++totalEventCount;
        if (event == "text_delta") ++textEventCount;
        if (event == "tool_use") ++toolEventCount;
        if (event == "api_error") ++apiErrorCount;
        if (event == "stop_reason") ++stopEventCount;
        if (totalEventCount <= 6) {
          // #region debug-point D:stream-event
          ReportDebugEvent("pre-fix", "D", "ModelClient.cpp:stream:event",
                           "[DEBUG] StreamResponse emitted event",
                           {{"event", event},
                            {"dataPrefix", TruncateDebugText(data)},
                            {"eventIndex", totalEventCount}},
                           traceId);
          // #endregion
        }
        if (onEvent) onEvent(event, data);
      };
  std::string actualModel = model.empty() ? config_.mainModel : model;
  const int maxTokens = maxTokensOverride > 0 ? maxTokensOverride : 4096;
  // #region debug-point A:stream-enter
  ReportDebugEvent("pre-fix", "A", "ModelClient.cpp:stream:enter",
                   "[DEBUG] StreamResponse enter",
                   {{"model", actualModel},
                    {"messageCount", static_cast<int>(messages.size())},
                    {"systemPromptSize", static_cast<int>(systemPrompt.size())},
                    {"toolsJsonSize", static_cast<int>(toolsJson.size())},
                    {"maxTokens", maxTokens},
                    {"nativeAnthropic", isNativeAnthropic_}},
                   traceId);
  // #endregion
  std::string body = BuildRequestBody(messages, systemPrompt, actualModel,
                                      maxTokens, true, toolsJson);
  std::string error;
  std::string raw = SendHttpPost(body, actualModel, nullptr, &error);
  // #region debug-point B:stream-post-return
  ReportDebugEvent("pre-fix", "B", "ModelClient.cpp:stream:post-return",
                   "[DEBUG] StreamResponse SendHttpPost returned",
                   {{"hasError", !error.empty()},
                    {"error", error},
                    {"rawSize", static_cast<int>(raw.size())},
                    {"jsonLike", LooksLikeJsonPayload(raw)},
                    {"rawPrefix", TruncateDebugText(raw)},
                    {"elapsedMs", static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start).count())}},
                   traceId);
  // #endregion
  if (!error.empty()) {
    if (instrumentedOnEvent) instrumentedOnEvent("api_error", error);
    return;
  }
  if (LooksLikeJsonPayload(raw)) {
    // #region debug-point C:stream-json-branch
    ReportDebugEvent("pre-fix", "C", "ModelClient.cpp:stream:json-branch",
                     "[DEBUG] StreamResponse selected JSON branch",
                     {{"nativeAnthropic", isNativeAnthropic_}},
                     traceId);
    // #endregion
    if (isNativeAnthropic_) {
      EmitAnthropicJsonResponseEvents(raw, instrumentedOnEvent);
    } else {
      EmitOpenAIJsonResponseEvents(raw, instrumentedOnEvent);
    }
    // #region debug-point D:stream-json-complete
    ReportDebugEvent("pre-fix", "D", "ModelClient.cpp:stream:json-complete",
                     "[DEBUG] StreamResponse JSON branch completed",
                     {{"totalEvents", totalEventCount},
                      {"textEvents", textEventCount},
                      {"toolEvents", toolEventCount},
                      {"apiErrors", apiErrorCount},
                      {"stopEvents", stopEventCount}},
                     traceId);
    // #endregion
    return;
  }
  // #region debug-point C:stream-sse-branch
  ReportDebugEvent("pre-fix", "C", "ModelClient.cpp:stream:sse-branch",
                   "[DEBUG] StreamResponse selected SSE branch",
                   {{"nativeAnthropic", isNativeAnthropic_}},
                   traceId);
  // #endregion
  if (isNativeAnthropic_) {
    ParseAnthropicSse(raw, instrumentedOnEvent);
  } else {
    ParseOpenAISseDelta(raw, instrumentedOnEvent);
  }
  // #region debug-point D:stream-sse-complete
  ReportDebugEvent("pre-fix", "D", "ModelClient.cpp:stream:sse-complete",
                   "[DEBUG] StreamResponse SSE branch completed",
                   {{"totalEvents", totalEventCount},
                    {"textEvents", textEventCount},
                    {"toolEvents", toolEventCount},
                    {"apiErrors", apiErrorCount},
                    {"stopEvents", stopEventCount},
                    {"elapsedMs", static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start).count())}},
                   traceId);
  // #endregion
}

std::vector<core::Message> HttpLlmClient::SideQuery(
    const std::vector<core::Message>& messages,
    const std::string& systemPrompt,
    const std::string& model) {
  std::string actualModel = model.empty() ? config_.validatorModel : model;
  if (actualModel.empty()) actualModel = config_.mainModel;
  if (!isNativeAnthropic_) {
    core::Message response;
    response.role = core::MessageRole::Assistant;
    response.uuid = "http-sidequery-stream-aggregate";
    std::ostringstream text;
    bool sawApiError = false;
    std::string body = BuildRequestBody(
        messages, systemPrompt, actualModel, 4096, true, "", 0.0);
    std::string error;
    std::string raw = SendHttpPost(body, actualModel, nullptr, &error);
    if (!error.empty()) {
      response.isApiErrorMessage = true;
      response.content.push_back(core::ContentBlock::MakeText(
          "Side query error: " + error));
      return {response};
    }
    ParseOpenAISseDelta(
        raw,
        [&](const std::string& event, const std::string& data) {
          if (event == "text_delta") {
            text << data;
          } else if (event == "stop_reason") {
            response.stopReason = data;
          } else if (event == "api_error") {
            sawApiError = true;
            response.isApiErrorMessage = true;
            response.content.clear();
            response.content.push_back(core::ContentBlock::MakeText(
                "Side query error: " + data));
          }
        });
    if (!sawApiError) {
      response.content.push_back(core::ContentBlock::MakeText(text.str()));
    }
    return {response};
  }
  std::string body = BuildRequestBody(messages, systemPrompt, actualModel,
                                      4096, false, "", 0.0);
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
  const std::string parsed = isNativeAnthropic_
      ? ExtractAnthropicResponseText(raw)
      : ExtractOpenAIResponseText(raw);
  r.content.push_back(core::ContentBlock::MakeText(
      parsed.empty() ? raw : parsed));
  return {r};
}

}  // namespace api
}  // namespace agent
