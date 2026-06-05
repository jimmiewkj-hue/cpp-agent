#include "core/QueryLoop.h"

#include "api/ModelClient.h"
#include "api/SideQueryClient.h"
#include "core/StreamingToolExecutor.h"
#include "hooks/HookExecutor.h"
#include "infra/SessionManager.h"
#include "permissions/PermissionEngine.h"
#include "tools/ToolOrchestrator.h"
#include "tools/ToolRegistry.h"
#include "third_party/nlohmann_json.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <chrono>
#include <sstream>

using json = nlohmann::json;

namespace agent {
namespace core {

static const int kAutoCompactMaxFailures = 3;
static const int kMaxOutputTokensRecoveryLimit = 3;
static const int kContextWindow = 200000;
static const int kMaxOutputTokensForSummary = 20000;
static const int kAutoCompactBufferTokens = 13000;
static const int kPerMessageBudgetLimit = 600000;
static const int kMicroCompactOldMarkerBytes = 64;
static const int kEscalatedMaxTokens = 65536;
static const int kMaxOutputTokensDefault = 8192;  // aligned with local-ace CAPPED_DEFAULT_MAX_TOKENS
static const int kMicroCompactAgeMs = 5 * 60 * 1000;

namespace {

struct QueryLoopDebugConfig {
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

std::wstring DebugToWide(const std::string& text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                      wide.data(), size);
  return wide;
}

QueryLoopDebugConfig LoadQueryLoopDebugConfig() {
  QueryLoopDebugConfig cfg;
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

std::string MakeQueryLoopTraceId(const std::string& prefix) {
  const long long nowMs = static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  return prefix + "-" + std::to_string(nowMs);
}

std::string TruncateDebugText(const std::string& text,
                              std::size_t maxLen = 240) {
  if (text.size() <= maxLen) return text;
  return text.substr(0, maxLen) + "...";
}

void ReportQueryLoopDebugEvent(const std::string& hypothesisId,
                               const std::string& location,
                               const std::string& msg,
                               const json& data,
                               const std::string& traceId = std::string()) {
  const QueryLoopDebugConfig cfg = LoadQueryLoopDebugConfig();
  if (cfg.url.empty() || cfg.sessionId.empty()) return;

  json payload;
  payload["sessionId"] = cfg.sessionId;
  payload["runId"] = "post-fix";
  payload["hypothesisId"] = hypothesisId;
  payload["location"] = location;
  payload["msg"] = msg;
  payload["data"] = data;
  payload["ts"] = static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  if (!traceId.empty()) payload["traceId"] = traceId;
  const std::string body = payload.dump(
      -1, ' ', false, json::error_handler_t::replace);

  URL_COMPONENTS components;
  ZeroMemory(&components, sizeof(components));
  components.dwStructSize = sizeof(components);
  wchar_t hostName[256] = {0};
  wchar_t urlPath[1024] = {0};
  std::wstring wideUrl = DebugToWide(cfg.url);
  components.lpszHostName = hostName;
  components.dwHostNameLength = sizeof(hostName) / sizeof(hostName[0]);
  components.lpszUrlPath = urlPath;
  components.dwUrlPathLength = sizeof(urlPath) / sizeof(urlPath[0]);
  if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components)) return;

  const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
  const std::wstring host(components.lpszHostName, components.dwHostNameLength);
  const std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);

  HINTERNET session = WinHttpOpen(L"cpp-agent-queryloop-debug/0.1",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
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

const char* QueryStageToString(QueryStage stage) {
  switch (stage) {
    case QueryStage::ToolResultBudget: return "ToolResultBudget";
    case QueryStage::Snip: return "Snip";
    case QueryStage::Microcompact: return "Microcompact";
    case QueryStage::Collapse: return "Collapse";
    case QueryStage::Autocompact: return "Autocompact";
    case QueryStage::ModelCall: return "ModelCall";
    case QueryStage::Validator: return "Validator";
    case QueryStage::StopHooks: return "StopHooks";
    case QueryStage::RunTools: return "RunTools";
    case QueryStage::Completed: return "Completed";
  }
  return "Unknown";
}

void EmitQueryLoopEvent(const QueryLoopContext& ctx,
                        QueryLoopEvent::Type type,
                        QueryStage stage,
                        const Message* message = nullptr,
                        const std::string& terminalReason = std::string()) {
  if (!ctx.eventCallback) return;
  QueryLoopEvent event;
  event.type = type;
  event.stage = stage;
  if (message != nullptr) {
    event.message = *message;
  }
  event.terminalReason = terminalReason;
  ctx.eventCallback(event);
}

std::vector<ContentBlock> CollectToolUseBlocks(
    const std::vector<Message>& messages) {
  std::vector<ContentBlock> toolUses;
  for (const auto& message : messages)
    for (const auto& block : message.content)
      if (block.type == BlockType::ToolUse)
        toolUses.push_back(block);
  return toolUses;
}

bool ContainsToken(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string ToLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

std::string NormalizePathSeparators(std::string value) {
  std::replace(value.begin(), value.end(), '/', '\\');
  return value;
}

std::string MakeWorkspaceRelativePath(const std::string& workspaceRoot,
                                      const std::string& candidatePath) {
  if (workspaceRoot.empty() || candidatePath.empty()) return std::string();
  std::string normalizedRoot = NormalizePathSeparators(workspaceRoot);
  std::string normalizedCandidate = NormalizePathSeparators(candidatePath);
  if (normalizedRoot.empty() || normalizedCandidate.empty()) return std::string();
  if (normalizedRoot.back() != '\\') normalizedRoot.push_back('\\');

  const std::string lowerRoot = ToLowerAscii(normalizedRoot);
  const std::string lowerCandidate = ToLowerAscii(normalizedCandidate);
  if (lowerCandidate.size() < lowerRoot.size()) return std::string();
  if (lowerCandidate.compare(0, lowerRoot.size(), lowerRoot) != 0) {
    return std::string();
  }

  std::string relative = normalizedCandidate.substr(normalizedRoot.size());
  while (!relative.empty() &&
         (relative.front() == '\\' || relative.front() == '/')) {
    relative.erase(relative.begin());
  }
  return relative;
}

bool RewriteWorkspaceRelativePaths(json* value,
                                   const std::string& workspaceRoot) {
  if (value == nullptr || workspaceRoot.empty()) return false;
  bool changed = false;
  if (value->is_object()) {
    static const std::set<std::string> kPathKeys = {
        "file_path", "path", "cwd", "notebook_path"};
    for (auto it = value->begin(); it != value->end(); ++it) {
      if (kPathKeys.find(it.key()) != kPathKeys.end() && it->is_string()) {
        const std::string relative =
            MakeWorkspaceRelativePath(workspaceRoot, it->get<std::string>());
        if (!relative.empty()) {
          *it = relative;
          changed = true;
        }
        continue;
      }
      changed = RewriteWorkspaceRelativePaths(&(*it), workspaceRoot) || changed;
    }
  } else if (value->is_array()) {
    for (auto& item : *value) {
      changed = RewriteWorkspaceRelativePaths(&item, workspaceRoot) || changed;
    }
  }
  return changed;
}

std::string CollectText(const std::vector<Message>& messages) {
  std::ostringstream out;
  bool first = true;
  for (const auto& msg : messages) {
    for (const auto& block : msg.content) {
      if (block.type != BlockType::Text) continue;
      if (!first) out << "\n";
      first = false;
      out << block.asText.text;
    }
  }
  return out.str();
}

std::string NormalizeForFingerprint(const std::string& text) {
  std::string normalized;
  normalized.reserve(text.size());
  bool previousWasSpace = false;
  for (unsigned char ch : text) {
    if (std::isspace(ch)) {
      if (!previousWasSpace && !normalized.empty()) normalized.push_back(' ');
      previousWasSpace = true;
      continue;
    }
    normalized.push_back(static_cast<char>(std::tolower(ch)));
    previousWasSpace = false;
    if (normalized.size() >= 160) break;
  }
  while (!normalized.empty() && normalized.back() == ' ') {
    normalized.pop_back();
  }
  return normalized;
}

std::string CollectToolResultFingerprint(const Message& message,
                                         bool errorsOnly) {
  std::ostringstream out;
  bool first = true;
  for (const auto& block : message.content) {
    if (block.type != BlockType::ToolResult) continue;
    if (errorsOnly && !block.asToolResult.isError) continue;
    const std::string normalized =
        NormalizeForFingerprint(block.asToolResult.content);
    if (normalized.empty()) continue;
    if (!first) out << "|";
    first = false;
    out << (block.asToolResult.isError ? "error:" : "ok:") << normalized;
  }
  return out.str();
}

int CountConsecutiveRecentToolResults(const std::vector<Message>& messages,
                                      bool errorsOnly,
                                      std::string* latestFingerprint) {
  std::string target;
  int count = 0;
  for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
    const std::string fingerprint =
        CollectToolResultFingerprint(*it, errorsOnly);
    if (fingerprint.empty()) continue;
    if (target.empty()) {
      target = fingerprint;
      count = 1;
      continue;
    }
    if (fingerprint != target) break;
    ++count;
  }
  if (latestFingerprint != nullptr) *latestFingerprint = target;
  return count;
}

std::string BuildRecentExecutionMemory(const QueryLoopContext& ctx,
                                       const QueryLoopInternalState& state) {
  std::vector<std::string> lines;
  // Note: validator retry guidance removed — align with local-ace which does
  // not inject execution-memory system messages. The validator retry path
  // in HandleNoToolContinuation already handles retries directly.

  std::string repeatedErrorFingerprint;
  const int repeatedErrorCount = CountConsecutiveRecentToolResults(
      ctx.messages, true, &repeatedErrorFingerprint);
  if (repeatedErrorCount >= 2 && !repeatedErrorFingerprint.empty()) {
    lines.push_back(
        "The same error tool result has appeared "
        + std::to_string(repeatedErrorCount)
        + " consecutive times. Do not rerun the same failing action without a"
          " concrete change. Latest error: "
        + repeatedErrorFingerprint);
  }

  // Also detect repeated non-error tool results (e.g., reading same file, same Glob)
  std::string repeatedOkFingerprint;
  const int repeatedOkCount = CountConsecutiveRecentToolResults(
      ctx.messages, false, &repeatedOkFingerprint);
  if (repeatedOkCount >= 3 && !repeatedOkFingerprint.empty()) {
    lines.push_back(
        "The same successful tool result has appeared "
        + std::to_string(repeatedOkCount)
        + " consecutive times. You are repeating actions that have already "
        "produced results. Do NOT repeat the same Glob or Read calls. "
        "Move forward with the information you already have.");
  }

  if (lines.empty()) return std::string();

  std::ostringstream out;
  out << "[Recent execution memory]";
  for (const auto& line : lines) {
    out << "\n- " << line;
  }
  return out.str();
}

Message MakeTerminalTranscriptRecord(const QueryLoopInternalState& state) {
  Message record;
  record.role = MessageRole::System;
  record.uuid = "query-loop-terminal-" + state.terminalReason;
  record.isMeta = true;
  record.stopReason = state.terminalReason;
  record.content.push_back(ContentBlock::MakeText(
      "[session] query loop completed with terminal reason: "
      + state.terminalReason
      + " (turn_count=" + std::to_string(state.turnCount)
      + ", model_calls=" + std::to_string(state.modelCallCount) + ")"));
  return record;
}

bool MessageHasTextOrToolContent(const Message& message) {
  for (const auto& block : message.content) {
    if (block.type == BlockType::Text && !block.asText.text.empty()) return true;
    if (block.type == BlockType::ToolUse) return true;
    if (block.type == BlockType::ToolResult &&
        !block.asToolResult.content.empty()) {
      return true;
    }
  }
  return false;
}

Message MakeHookMessage(const std::string& uuid,
                        const std::string& text,
                        bool isError) {
  Message message;
  message.role = MessageRole::System;
  message.uuid = uuid;
  message.isMeta = true;
  message.content.push_back(ContentBlock::MakeText(text));
  message.isApiErrorMessage = isError;
  return message;
}

void AppendHookResultMessage(const hooks::HookResult& hookResult,
                             const std::string& uuidPrefix,
                             bool asError,
                             std::vector<Message>* out) {
  if (out == nullptr) return;
  if (!hookResult.message.content.empty()) {
    out->push_back(hookResult.message);
    return;
  }
  std::string text = hookResult.reason;
  if (text.empty()) text = hookResult.stdoutText;
  if (text.empty()) text = hookResult.stderrText;
  if (text.empty()) return;
  out->push_back(MakeHookMessage(uuidPrefix, text, asError));
}

void MergeHookMessages(const hooks::HookBatchResult& batch,
                       const std::string& uuidPrefix,
                       std::vector<Message>* followups,
                       std::vector<Message>* blocking) {
  for (std::size_t i = 0; i < batch.results.size(); ++i) {
    const hooks::HookResult& hookResult = batch.results[i];
    const std::string id = uuidPrefix + "-" + std::to_string(i + 1);
    if (hookResult.outcome == hooks::HookOutcome::Blocking) {
      AppendHookResultMessage(hookResult, id, true, blocking);
      continue;
    }
    if (hookResult.continueSession &&
        (!hookResult.reason.empty() || !hookResult.stdoutText.empty() ||
         !hookResult.message.content.empty())) {
      AppendHookResultMessage(hookResult, id, false, followups);
    }
  }
}

bool IsExplorationToolName(const std::string& toolName) {
  return toolName == "Read" || toolName == "FileRead" ||
         toolName == "Grep" || toolName == "Glob" || toolName == "LS";
}

bool IsWorkspaceWriteToolName(const std::string& toolName) {
  return toolName == "Write" || toolName == "FileWrite" ||
         toolName == "NotebookEdit";
}

bool HasExplorationToolUse(
    const std::vector<ContentBlock>& toolUseBlocks) {
  for (const auto& block : toolUseBlocks) {
    if (block.type != BlockType::ToolUse) continue;
    if (IsExplorationToolName(block.asToolUse.name)) return true;
  }
  return false;
}

bool HasPriorExplorationEvidence(const std::vector<Message>& messages) {
  for (const auto& msg : messages) {
    for (const auto& block : msg.content) {
      if (block.type == BlockType::ToolUse &&
          IsExplorationToolName(block.asToolUse.name)) {
        return true;
      }
      if (block.type == BlockType::ToolResult &&
          (msg.uuid.find("tool-result-Read") != std::string::npos ||
           msg.uuid.find("tool-result-FileRead") != std::string::npos ||
           msg.uuid.find("tool-result-Grep") != std::string::npos ||
           msg.uuid.find("tool-result-Glob") != std::string::npos ||
           msg.uuid.find("tool-result-LS") != std::string::npos)) {
        return true;
      }
    }
  }
  return false;
}

bool HasWorkspaceWriteToolUse(const std::vector<ContentBlock>& toolUseBlocks) {
  for (const auto& block : toolUseBlocks) {
    if (block.type != BlockType::ToolUse) continue;
    if (IsWorkspaceWriteToolName(block.asToolUse.name)) return true;
  }
  return false;
}

bool UserRequestedDirectCreation(const std::vector<Message>& messages) {
  for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
    if (it->role != MessageRole::User) continue;
    const std::string prompt = CollectText({*it});
    const std::string lower = ToLowerAscii(prompt);
    return ContainsToken(lower, "create a new project") ||
           ContainsToken(lower, "new project from scratch") ||
           ContainsToken(lower, "start from scratch") ||
           ContainsToken(lower, "scaffold a new project") ||
           ContainsToken(lower, "create a file") ||
           ContainsToken(lower, "create the file") ||
           ContainsToken(lower, "write a file") ||
           ContainsToken(lower, "generate a file") ||
           ContainsToken(prompt, "创建文件") ||
           ContainsToken(prompt, "创建一个文件") ||
           ContainsToken(prompt, "新建项目") ||
           ContainsToken(prompt, "从零开始") ||
           ContainsToken(prompt, "直接创建") ||
           ContainsToken(prompt, "直接新建");
  }
  return false;
}

std::string GetEnvString(const char* name) {
  char buffer[256] = {0};
  DWORD len = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
  if (len == 0 || len >= sizeof(buffer)) return {};
  return std::string(buffer, len);
}

std::string ResolveValidatorModel(const QueryLoopContext& ctx) {
  if (!ctx.validatorModel.empty()) return ctx.validatorModel;
  std::string model = GetEnvString("CPP_AGENT_VALIDATOR_MODEL");
  if (!model.empty()) return model;
  return GetEnvString("LOCALMODEL_VALIDATION_MODEL");
}

bool ShouldRunValidation(const QueryLoopContext& ctx) {
  return !ResolveValidatorModel(ctx).empty();
}

bool IsOpenAIEndpoint(const std::string& ep) {
  return ep.find("api.anthropic.com") == std::string::npos;
}

std::string ExtractXml(const std::string& text, const std::string& tag) {
  auto open = "<" + tag + ">";
  auto close = "</" + tag + ">";
  auto s = text.find(open);
  if (s == std::string::npos) return {};
  s += open.size();
  auto e = text.find(close, s);
  if (e == std::string::npos) return {};
  return text.substr(s, e - s);
}

std::string ExtractJsonStringField(const std::string& json,
                                   const std::string& key) {
  std::string token = "\"" + key + "\":";
  auto p = json.find(token);
  if (p == std::string::npos) return {};
  p += token.size();
  while (p < json.size() && (json[p] == ' ' || json[p] == '\n')) ++p;
  if (p >= json.size()) return {};
  if (json[p] == '"') {
    auto e = p + 1;
    while (e < json.size()) {
      if (json[e] == '\\') { e += 2; continue; }
      if (json[e] == '"') break;
      ++e;
    }
    if (e >= json.size()) return {};
    return json.substr(p + 1, e - p - 1);
  }
  return {};
}

ValidationResult ParseValidationResponse(const std::string& text) {
  ValidationResult result;
  std::string jsonBlock = ExtractXml(text, "validation_json");

  if (!jsonBlock.empty()) {
    try {
      auto j = json::parse(jsonBlock);

      if (j.contains("text_correction") && j["text_correction"].is_object()) {
        const auto& correction = j["text_correction"];
        if (correction.contains("needed") &&
            correction["needed"].is_boolean() &&
            correction["needed"].get<bool>() &&
            correction.contains("corrected_text") &&
            correction["corrected_text"].is_string()) {
          result.correctedText =
              correction["corrected_text"].get<std::string>();
        }
      }

      // Fallback: accept top-level corrected_text (some LLMs omit the text_correction wrapper)
      if (result.correctedText.empty() && j.contains("corrected_text") &&
          j["corrected_text"].is_string()) {
        result.correctedText = j["corrected_text"].get<std::string>();
      }

      if (j.contains("final_response_action") &&
          j["final_response_action"].is_string())
        result.finalResponseAction = j["final_response_action"].get<std::string>();

      if (j.contains("retry_guidance") && j["retry_guidance"].is_string())
        result.retryGuidance = j["retry_guidance"].get<std::string>();

      if (j.contains("tool_interventions") && j["tool_interventions"].is_array()) {
        for (const auto& ti : j["tool_interventions"]) {
          ValidationToolIntervention vti;
          if (ti.contains("tool_use_id"))
            vti.toolUseId = ti["tool_use_id"].get<std::string>();
          if (ti.contains("action"))
            vti.action = ti["action"].get<std::string>();
          if (ti.contains("corrected_name"))
            vti.correctedName = ti["corrected_name"].get<std::string>();
          if (ti.contains("corrected_input"))
            vti.correctedInputJson = ti["corrected_input"].dump();
          if (ti.contains("block_reason"))
            vti.blockGuidance = ti["block_reason"].get<std::string>();
          if (!vti.toolUseId.empty() && !vti.action.empty())
            result.toolInterventions.push_back(vti);
        }
      }
    } catch (...) {
    }
  }

  std::string correctedBlock = ExtractXml(text, "corrected_text");
  if (!correctedBlock.empty() && result.correctedText.empty())
    result.correctedText = correctedBlock;

  return result;
}

bool HasFencedCodeBlock(const std::string& text) {
  return text.find("```") != std::string::npos;
}

std::string ExtractFenceLanguage(const std::string& text) {
  std::size_t start = text.find("```");
  if (start == std::string::npos) return "";
  start += 3;
  std::size_t end = text.find('\n', start);
  if (end == std::string::npos) return "";
  std::string lang = text.substr(start, end - start);
  std::string trimmed;
  for (char c : lang) { if (c != ' ' && c != '\r') trimmed += c; }
  return trimmed;
}

std::string RestoreFencePresentation(const std::string& corrected,
                                     const std::string& original) {
  if (!HasFencedCodeBlock(original)) return corrected;
  std::string lang = ExtractFenceLanguage(original);
  if (HasFencedCodeBlock(corrected)) return corrected;
  std::string opener = lang.empty() ? "```" : "```" + lang;
  return opener + "\n" + corrected + "\n```";
}

void ApplyTextCorrection(const std::string& correctedText,
                         std::vector<Message>& assistantMessages) {
  if (correctedText.empty() || assistantMessages.empty()) return;
  std::string finalText = correctedText;
  for (const auto& msg : assistantMessages) {
    for (const auto& block : msg.content) {
      if (block.type == BlockType::Text) {
        finalText = RestoreFencePresentation(correctedText, block.asText.text);
        break;
      }
    }
    if (finalText != correctedText) break;
  }
  bool applied = false;
  for (auto& msg : assistantMessages) {
    for (auto& block : msg.content) {
      if (block.type == BlockType::Text) {
        block.asText.text = finalText;
        msg.isMeta = true;
        applied = true;
        return;
      }
    }
  }
  if (!applied) {
    for (auto& msg : assistantMessages) {
      msg.isMeta = true;
    }
  }
}

struct ToolInterventionResult {
  std::vector<ContentBlock> rewrittenBlocks;
  std::set<std::string> blockedIds;
  std::map<std::string, std::string> blockGuidance;
};

void ApplyToolInterventions(
    const std::vector<ValidationToolIntervention>& interventions,
    std::vector<ContentBlock>& toolUseBlocks,
    ToolInterventionResult& result) {
  result.rewrittenBlocks.clear();
  result.blockedIds.clear();
  result.blockGuidance.clear();

  for (const auto& block : toolUseBlocks) {
    bool matched = false;
    for (const auto& ti : interventions) {
      if (block.asToolUse.id != ti.toolUseId) continue;
      matched = true;
      if (ti.action == "rewrite") {
        ContentBlock rewritten = block;
        if (!ti.correctedName.empty())
          rewritten.asToolUse.name = ti.correctedName;
        if (!ti.correctedInputJson.empty())
          rewritten.asToolUse.inputJson = ti.correctedInputJson;
        result.rewrittenBlocks.push_back(rewritten);
      } else if (ti.action == "block") {
        result.blockedIds.insert(ti.toolUseId);
        result.blockGuidance[ti.toolUseId] =
            ti.blockGuidance.empty() ? "unsafe" : ti.blockGuidance;
      }
      break;
    }
    if (!matched) result.rewrittenBlocks.push_back(block);
  }
}

std::string BuildValidationContext(
    const std::vector<Message>& messages,
    const std::vector<Message>& assistantMessages,
    const std::vector<ContentBlock>& toolUseBlocks,
    const tools::ToolRegistry* toolRegistry,
    const std::string& workspaceRoot) {
  std::string goal;
  for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
    if (it->role != MessageRole::User) continue;
    for (const auto& b : it->content) {
      if (b.type == BlockType::Text) {
        goal = b.asText.text;
        break;
      }
    }
    if (!goal.empty()) break;
  }
  if (goal.size() > 4000) goal = goal.substr(0, 4000);
  std::string assistantText;
  if (!assistantMessages.empty()) {
    for (const auto& b : assistantMessages.back().content) {
      if (b.type != BlockType::Text) continue;
      assistantText = b.asText.text;
      break;
    }
  }
  if (assistantText.size() > 8000) assistantText = assistantText.substr(0, 8000);

  json actions = json::array();
  for (const auto& tb : toolUseBlocks) {
    json action;
    action["id"] = tb.asToolUse.id;
    action["name"] = tb.asToolUse.name;
    try {
      action["input"] = tb.asToolUse.inputJson.empty()
          ? json::object()
          : json::parse(tb.asToolUse.inputJson);
      RewriteWorkspaceRelativePaths(&action["input"], workspaceRoot);
    } catch (...) {
      action["input"] = json::object();
    }
    actions.push_back(action);
  }

  json executionEvidence = json::array();
  json recentFailureSummary = json::object();
  std::set<std::string> timeoutPatterns;
  std::set<std::string> errorPatterns;
  int timeoutCount = 0;
  int evCount = 0;
  for (auto it = messages.rbegin(); it != messages.rend() && evCount < 8; ++it) {
    if (it->role != MessageRole::User) continue;
    for (const auto& b : it->content) {
      if (b.type != BlockType::ToolResult) continue;
      std::string c = b.asToolResult.content;
      if (c.size() > 500) c = c.substr(0, 500);
      executionEvidence.push_back(c);
      // Track failure patterns for summary
      if (b.asToolResult.isError) {
        auto lower = c;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (lower.find("timed out") != std::string::npos ||
            lower.find("timeout") != std::string::npos) {
          timeoutCount++;
          timeoutPatterns.insert(c.substr(0, std::min<size_t>(c.size(), 120)));
        }
        if (lower.find("error") != std::string::npos ||
            lower.find("exit code") != std::string::npos) {
          errorPatterns.insert(c.substr(0, std::min<size_t>(c.size(), 120)));
        }
      }
      ++evCount;
    }
  }
  if (timeoutCount > 0) {
    recentFailureSummary["timeout_count"] = timeoutCount;
    json timeoutArr = json::array();
    for (const auto& tp : timeoutPatterns) timeoutArr.push_back(tp);
    recentFailureSummary["timeout_samples"] = timeoutArr;
  }
  if (!errorPatterns.empty()) {
    json errorArr = json::array();
    for (const auto& ep : errorPatterns) errorArr.push_back(ep);
    recentFailureSummary["error_samples"] = errorArr;
  }

  json relevantSchemas = json::array();
  if (toolRegistry != nullptr) {
    std::set<std::string> referencedToolNames;
    for (const auto& block : toolUseBlocks) {
      referencedToolNames.insert(block.asToolUse.name);
    }
    const auto tools = toolRegistry->ListTools();
    for (const auto& tool : tools) {
      if (referencedToolNames.find(tool->Name()) == referencedToolNames.end()) continue;
      json schema;
      schema["name"] = tool->Name();
      schema["description"] = tool->UserFacingDescription();
      try {
        std::string isj = tool->InputSchemaJson();
        schema["input_schema"] = isj.empty()
            ? json::object()
            : json::parse(isj);
      } catch (...) {
        schema["input_schema"] = json::object();
      }
      relevantSchemas.push_back(schema);
    }
  }

  json ctxJson;
  ctxJson["user_goal"] = goal;
  ctxJson["assistant_text"] = assistantText;
  ctxJson["assistant_tool_calls"] = actions;
  ctxJson["relevant_tool_schemas"] = relevantSchemas;
  ctxJson["execution_evidence"] = executionEvidence;
  ctxJson["recent_failure_summary"] = recentFailureSummary;
  return ctxJson.dump(2, ' ', false, json::error_handler_t::replace);
}

std::string BuildValidatorSystemPrompt() {
  return R"VALIDATOR(You are a validation model. Your job is to review an AI assistant's responses and tool calls for correctness, completeness, and safety.

You will receive a JSON object containing:
- "user_goal": what the user asked for
- "assistant_text": the assistant's text response (may be empty if only tool calls)
- "assistant_tool_calls": list of tool calls the assistant wants to make (name, input)
- "relevant_tool_schemas": schemas for the tools being called
- "execution_evidence": recent tool results and file changes

Your task:
1. Verify the assistant's text is correct, complete, safe, and addresses the user's goal
2. Verify tool calls use correct names and valid inputs matching their schemas
3. If the assistant made an error, provide corrections

Respond ONLY with the following XML structure. No text outside the tags:

<validation_json>
{
  "text_correction": {
    "needed": true or false,
    "corrected_text": "corrected text if needed, otherwise omit this field"
  },
  "tool_interventions": [
    {
      "tool_use_id": "exact tool_use id from the assistant",
      "action": "rewrite" or "block",
      "corrected_name": "corrected tool name (only for rewrite, omit if name unchanged)",
      "corrected_input": { "key": "corrected value" },
      "block_reason": "reason for blocking (only for block)"
    }
  ],
  "final_response_action": "approve" or "retry_from_tools",
  "retry_guidance": "what the assistant should fix (only for retry_from_tools)"
}
</validation_json>

If you corrected the assistant_text, also include the corrected text in:
<corrected_text>
corrected text here
</corrected_text>

CRITICAL ENVIRONMENT FACTS (read before making any judgment):
- The runtime OS is WINDOWS. The shell is PowerShell, NOT bash.
- Unix tools (grep, head, tail, sed, awk, xargs) DO NOT EXIST. The tool engine auto-converts these to PowerShell equivalents (Select-String, Select-Object -First/-Last).
- Commands like "pip list | head -100" will be auto-converted. Do NOT flag pipe syntax as invalid.
- "python -c" is the correct way to run inline Python on this system. "python3" may not exist.
- Paths use backslashes (C:\\foo\\bar) but forward slashes are also accepted.
- "2>/dev/null" does not work; redirect to $null or use -ErrorAction SilentlyContinue.
- If the assistant uses Unix syntax, the tool engine will normalize it. ONLY intervene if the assistant fundamentally misunderstands the user goal or is about to take a harmful action.

TOOL FAILURE AWARENESS (critical for avoiding retry loops):
- ALWAYS check "execution_evidence" for recent tool failures BEFORE issuing retry_from_tools.
- If the SAME command (e.g., "pip list") has timed out (120s) or errored REPEATEDLY in execution_evidence, do NOT demand retrying it. The environment cannot execute it.
- When a prerequisite command consistently fails (timeout, syntax error, command-not-found), ACCEPT alternative approaches: using importlib, checking individual packages, or skipping the prerequisite and proceeding with the main task.
- The assistant is NOT at fault for environmental failures. Do NOT block or retry_from_tools for commands that failed due to: timeout, corrupted packages, missing system utilities, or platform incompatibility.
- If execution_evidence contains error results with "[exit code: 1]" or "timed out after 120s", recognize these as environment problems, not assistant errors.
- If a tool result shows a timeout or repeated error, and the assistant is trying a different tool or approach, this is CORRECT behavior. APPROVE it.

Rules:
- Only intervene when there is a real error or safety concern
- NEVER issue retry_from_tools demanding the exact same command that just failed in execution_evidence
- For tool_interventions, you can both rewrite some tools and block others
- If the only issue is path formatting for files inside the current workspace,
  prefer a "rewrite" intervention over "block" or "retry_from_tools"
- Absolute workspace paths and relative workspace paths are both acceptable for
  read/search tools when they resolve to the same in-workspace target
- "retry_from_tools" means the assistant fundamentally misunderstood and needs to redo tool work
- Never invent information not present in the context)VALIDATOR";
}

std::string BuildToolsJson(const tools::ToolRegistry* toolRegistry) {
  if (!toolRegistry) return "[]";
  const auto tools = toolRegistry->ListTools();
  json jarr = json::array();
  for (const auto& tool : tools) {
    json jtool;
    jtool["type"] = "function";
    jtool["function"]["name"] = tool->Name();
    jtool["function"]["description"] = tool->UserFacingDescription();
    try {
      jtool["function"]["parameters"] = json::parse(tool->InputSchemaJson());
    } catch (...) {
      jtool["function"]["parameters"] = json::object();
    }
    jarr.push_back(jtool);
  }
  return jarr.dump(-1, ' ', false, json::error_handler_t::replace);
}

void PersistOversizedResult(const std::string& sessionDir,
                            const std::string& toolUseId,
                            const std::string& content) {
  if (sessionDir.empty()) return;
  std::string dir = sessionDir + "\\.tool-results";
  CreateDirectoryW(DebugToWide(dir).c_str(), nullptr);
  std::string path = dir + "\\" + toolUseId + ".txt";
  HANDLE handle = CreateFileW(DebugToWide(path).c_str(), GENERIC_WRITE, 0,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(handle, content.data(), static_cast<DWORD>(content.size()),
            &written, nullptr);
  CloseHandle(handle);
}

void PersistMessagesToTranscript(infra::SessionManager* sm,
                                  const std::vector<Message>& msgs) {
  if (!sm) return;
  for (const auto& m : msgs) sm->AppendMessageToTranscript(m);
}

bool HandleMissingWorkspaceExploration(QueryLoopContext& ctx,
                                       QueryLoopInternalState& state) {
  if (state.hasPromptedForWorkspaceExploration) return false;
  if (state.turnCount != 1) return false;
  if (state.toolUseBlocks.empty()) return false;
  if (!HasWorkspaceWriteToolUse(state.toolUseBlocks)) return false;
  if (HasExplorationToolUse(state.toolUseBlocks)) return false;
  if (HasPriorExplorationEvidence(ctx.messages)) return false;
  if (UserRequestedDirectCreation(ctx.messages)) return false;

  for (const auto& msg : state.assistantMessages)
    ctx.messages.push_back(msg);
  PersistMessagesToTranscript(ctx.sessionManager, state.assistantMessages);

  Message nudge;
  nudge.role = MessageRole::System;
  nudge.uuid = "workspace-exploration-nudge";
  nudge.isMeta = true;
  nudge.content.push_back(ContentBlock::MakeText(
      "Before writing project files for an analysis or modification task, "
      "you must first inspect the existing workspace with Read, Grep, or Glob. "
      "Do not call Write/FileWrite yet. Explore the relevant files first, then "
      "decide what to change."));
  ctx.messages.push_back(nudge);
  PersistMessagesToTranscript(ctx.sessionManager, {nudge});

  state.assistantMessages.clear();
  state.toolUseBlocks.clear();
  state.hasPromptedForWorkspaceExploration = true;
  state.transition = TransitionReason::ForcedContinuation;
  state.stage = QueryStage::ToolResultBudget;
  return true;
}

}  // namespace

long long CurrentTimeMs() {
  return static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

QueryLoop::QueryLoop(tools::ToolOrchestrator& toolOrchestrator,
                     permissions::PermissionEngine& permissionEngine,
                     api::ModelClient& modelClient,
                     api::SideQueryClient& sideQueryClient)
    : toolOrchestrator_(toolOrchestrator),
      permissionEngine_(permissionEngine),
      modelClient_(modelClient),
      sideQueryClient_(sideQueryClient) {}

void QueryLoop::SetMaxTurns(int maxTurns) {
  maxTurns_ = maxTurns > 0 ? maxTurns : 0;
}

void QueryLoop::SetWallClockBudget(long long budgetMs) {
  wallClockBudgetMs_ = budgetMs;
}

bool QueryLoop::IsWallClockExpired(QueryLoopContext& ctx) const {
  if (wallClockBudgetMs_ <= 0) return false;
  if (loopStartTimeMs_ <= 0) return false;
  const long long elapsedMs = CurrentTimeMs() - loopStartTimeMs_;
  if (elapsedMs < wallClockBudgetMs_) return false;

  Message timeout;
  timeout.role = MessageRole::System;
  timeout.uuid = "wall-clock-timeout";
  timeout.isMeta = true;
  timeout.content.push_back(ContentBlock::MakeText(
      "[system] Terminating: wall-clock budget exceeded after " +
      std::to_string(elapsedMs) + " ms (budget " +
      std::to_string(wallClockBudgetMs_) + " ms)."));
  ctx.messages.push_back(timeout);
  if (ctx.sessionManager != nullptr) {
    ctx.sessionManager->AppendMessageToTranscript(timeout);
  }
  return true;
}

std::string QueryLoop::MakeToolFingerprint(const ContentBlock& block) const {
  if (block.type != BlockType::ToolUse) return std::string();
  std::string input = block.asToolUse.inputJson;
  input.erase(std::remove_if(input.begin(), input.end(),
      [](unsigned char c) { return std::isspace(c); }),
      input.end());
  return block.asToolUse.name + ":" + input;
}

bool QueryLoop::ShouldTerminateOnExcessiveExploration(
    QueryLoopContext& ctx,
    QueryLoopInternalState& state) const {
  // Count consecutive turns where the model produces ONLY exploration/planning
  // tools (Read, Glob, Grep, LS, TodoWrite) without any action tools
  // (Write, FileWrite, NotebookEdit, Bash, TaskCreate).
  if (state.toolUseBlocks.empty()) {
    state.consecutiveExplorationOnlyTurns = 0;
    return false;
  }

  bool hasActionTool = false;
  for (const auto& block : state.toolUseBlocks) {
    if (block.type != BlockType::ToolUse) continue;
    const std::string& name = block.asToolUse.name;
    if (IsWorkspaceWriteToolName(name) ||
        name == "Bash" || name == "TaskCreate") {
      hasActionTool = true;
      break;
    }
  }

  if (hasActionTool) {
    state.consecutiveExplorationOnlyTurns = 0;
    return false;
  }

  ++state.consecutiveExplorationOnlyTurns;

  static const int kMaxExplorationOnlyTurns = 12;
  if (state.consecutiveExplorationOnlyTurns < kMaxExplorationOnlyTurns) {
    return false;
  }

  // Hard terminate: the model has spent too many turns only exploring.
  // The runner can detect this termination and restart with the nudge.
  Message terminationMsg;
  terminationMsg.role = MessageRole::System;
  terminationMsg.uuid = "exploration-limit-terminate";
  terminationMsg.isMeta = true;
  terminationMsg.content.push_back(ContentBlock::MakeText(
      "[system] Terminating: " +
      std::to_string(state.consecutiveExplorationOnlyTurns) +
      " consecutive turns with only exploration/planning tools "
      "(Read, Glob, Grep, TodoWrite, LS). "
      "The agent must produce actionable output (Write, FileWrite, Bash, "
      "TaskCreate) or provide a final answer. "
      "If continuing, you MUST stop reading files and start writing code "
      "or executing commands immediately."));
  ctx.messages.push_back(terminationMsg);
  if (ctx.sessionManager) {
    ctx.sessionManager->AppendMessageToTranscript(terminationMsg);
  }
  return true;
}

bool QueryLoop::ShouldTerminateOnDuplicates(
    QueryLoopContext& ctx,
    QueryLoopInternalState& state) const {
  std::vector<ContentBlock> currentBlocks =
      CollectToolUseBlocks(state.assistantMessages);
  if (currentBlocks.empty()) return false;

  std::string latestFingerprint;
  for (const auto& block : currentBlocks) {
    std::string fp = MakeToolFingerprint(block);
    if (!fp.empty()) {
      if (!latestFingerprint.empty()) latestFingerprint += "|";
      latestFingerprint += fp;
    }
  }

  if (!latestFingerprint.empty() &&
      !state.recentToolFingerprints.empty()) {
    // Check for direct consecutive duplicates
    bool isDuplicate = (state.recentToolFingerprints.back() == latestFingerprint);
    // Also check for 2-alternating pattern: A, B, A, B, ...
    if (!isDuplicate && state.recentToolFingerprints.size() >= 3) {
      size_t sz = state.recentToolFingerprints.size();
      if (state.recentToolFingerprints[sz - 1] == state.recentToolFingerprints[sz - 3] &&
          latestFingerprint == state.recentToolFingerprints[sz - 2]) {
        isDuplicate = true;
      }
    }
    if (isDuplicate) {
      state.consecutiveDuplicateToolCalls++;
      // Read-only exploration tools (Glob, Read, Grep, LS) are expected to
      // return stable results; calling them again is a sign of confusion
      // but not as severe as duplicate write/execute tools. Require a higher
      // threshold before terminating.
      bool isExplorationFingerprint = false;
      {
        size_t colonPos = latestFingerprint.find(':');
        if (colonPos != std::string::npos) {
          std::string toolName = latestFingerprint.substr(0, colonPos);
          isExplorationFingerprint = IsExplorationToolName(toolName);
        }
      }
      int dupThreshold = isExplorationFingerprint ? 6 : 3;
      if (state.consecutiveDuplicateToolCalls >= dupThreshold) {
        Message terminationMsg;
        terminationMsg.role = MessageRole::System;
        terminationMsg.uuid = "dup-terminate";
        terminationMsg.isMeta = true;
        std::string termText =
            "[system] Terminating: repetitive tool calls detected "
            + std::to_string(state.consecutiveDuplicateToolCalls)
            + " consecutive times. The agent appears to be in a loop. "
            "Latest fingerprint: " + latestFingerprint;
        if (isExplorationFingerprint) {
          termText += " (exploration tool threshold: " + std::to_string(dupThreshold) + ")";
        }
        terminationMsg.content.push_back(ContentBlock::MakeText(termText));
        ctx.messages.push_back(terminationMsg);
        EmitQueryLoopEvent(ctx, QueryLoopEvent::Type::LoopCompleted,
                           QueryStage::Completed, nullptr,
                           "duplicate_tool_loop");
        return true;
      }
    } else {
      state.consecutiveDuplicateToolCalls = 1;
    }
  } else {
    state.consecutiveDuplicateToolCalls = 1;
  }

  state.recentToolFingerprints.push_back(latestFingerprint);
  if (state.recentToolFingerprints.size() > 10)
    state.recentToolFingerprints.erase(state.recentToolFingerprints.begin());

  return false;
}

std::vector<Message> QueryLoop::BuildMessagesForTurn(
    const QueryLoopContext& ctx,
    const QueryLoopInternalState& state) const {
  std::vector<Message> messages = ctx.messages;
  const std::string executionMemory = BuildRecentExecutionMemory(ctx, state);
  if (!executionMemory.empty()) {
    Message memory;
    memory.role = MessageRole::System;
    memory.uuid = "recent-execution-memory";
    memory.isMeta = true;
    memory.content.push_back(ContentBlock::MakeText(executionMemory));
    messages.insert(messages.begin(), memory);
  }
  return messages;
}

void QueryLoop::AppendTurnArtifacts(
    QueryLoopContext& ctx,
    const std::vector<Message>& assistantMessages,
    const std::vector<Message>& toolResults,
    const std::vector<Message>& followups) const {
  auto append = [&ctx](const std::vector<Message>& messages) {
    for (const auto& message : messages) {
      ctx.messages.push_back(message);
      if (ctx.sessionManager) {
        ctx.sessionManager->AppendMessageToTranscript(message);
      }
    }
  };
  append(assistantMessages);
  append(toolResults);
  append(followups);
}

void QueryLoop::PostToolTurnProcessing(QueryLoopContext& ctx,
                                       QueryLoopInternalState& state) {
  (void)ctx;
  (void)state;
}

bool QueryLoop::ContinueWithFollowup(QueryLoopContext& ctx,
                                     QueryLoopInternalState& state,
                                     const std::vector<Message>& followups,
                                     TransitionReason reason,
                                     bool resetTurnCount) {
  AppendTurnArtifacts(
      ctx, state.assistantMessages, state.toolResultMessages, followups);
  state.assistantMessages.clear();
  state.toolResultMessages.clear();
  state.pendingFollowupMessages.clear();
  state.toolUseBlocks.clear();
  state.forceContinuation = false;
  state.forceContinuationReason.clear();
  state.stage = QueryStage::ToolResultBudget;
  state.transition = reason;
  if (resetTurnCount) state.turnCount = 0;
  return true;
}

int QueryLoop::EstimateTokens(const std::string& text) {
  // Unicode-aware token estimation aligned with local-ace's
  // tokenCountWithEstimation. ASCII ~4 chars/token, CJK ~1.5 chars/token
  // (each CJK char is 3 UTF-8 bytes but ~1-2 tokens).
  int tokens = 0;
  std::size_t i = 0;
  const std::size_t len = text.size();
  int asciiRun = 0;
  while (i < len) {
    unsigned char ch = static_cast<unsigned char>(text[i]);
    if (ch < 0x80) {
      // ASCII byte
      ++asciiRun;
      ++i;
    } else {
      // Flush ASCII run
      if (asciiRun > 0) {
        tokens += asciiRun / 4;
        if (asciiRun % 4 > 0) ++tokens;
        asciiRun = 0;
      }
      // Multi-byte UTF-8: count the character as ~1.5 tokens
      tokens += 2;  // approximate: 1-2 tokens per CJK/non-ASCII char
      // Skip continuation bytes
      if ((ch & 0xE0) == 0xC0) { i += 2; }
      else if ((ch & 0xF0) == 0xE0) { i += 3; }
      else if ((ch & 0xF8) == 0xF0) { i += 4; }
      else { ++i; }  // invalid byte, skip
    }
  }
  if (asciiRun > 0) {
    tokens += asciiRun / 4;
    if (asciiRun % 4 > 0) ++tokens;
  }
  return std::max(1, tokens);
}

int QueryLoop::EstimateMessageTokens(const std::vector<Message>& msgs) {
  int total = 0;
  for (const auto& msg : msgs)
    for (const auto& block : msg.content) {
      if (block.type == BlockType::Text)
        total += EstimateTokens(block.asText.text);
      else if (block.type == BlockType::ToolUse)
        total += EstimateTokens(block.asToolUse.inputJson) + 10;
      else if (block.type == BlockType::ToolResult)
        total += EstimateTokens(block.asToolResult.content) + 5;
    }
  return total;
}

int QueryLoop::CountToolResultBytes(const Message& msg) {
  int total = 0;
  for (const auto& block : msg.content)
    if (block.type == BlockType::ToolResult)
      total += static_cast<int>(block.asToolResult.content.size());
  return total;
}

std::vector<Message> QueryLoop::DoCollapseCompact(
    const std::vector<Message>& input, int keepRecent) {
  std::vector<Message> result;

  if (keepRecent < 0) {
    std::size_t half = input.size() / 2;
    if (half < 1) half = 1;
    Message boundary;
    boundary.role = MessageRole::System;
    boundary.uuid = "collapse-boundary";
    boundary.isMeta = true;
    boundary.content.push_back(ContentBlock::MakeText(
        "[Context Collapse] Earlier conversation archived."));
    result.push_back(boundary);
    result.insert(result.end(), input.begin() + input.size() - half,
                  input.end());
    return result;
  }

  auto start = input.begin();
  if (keepRecent > 0 && static_cast<int>(input.size()) > keepRecent)
    start = input.end() - keepRecent;
  if (start != input.begin()) {
    Message boundary;
    boundary.role = MessageRole::System;
    boundary.uuid = "collapse-boundary";
    boundary.isMeta = true;
    boundary.content.push_back(ContentBlock::MakeText(
        "[Context Collapse] Earlier conversation archived."));
    result.push_back(boundary);
  }
  result.insert(result.end(), start, input.end());
  return result;
}

std::vector<Message> QueryLoop::DoReactiveCompact(
    const std::vector<Message>& input) {
  return DoCollapseCompact(input, 5);
}

bool QueryLoop::IsPromptTooLong(const Message& msg) {
  if (!msg.isApiErrorMessage) return false;
  for (const auto& block : msg.content) {
    if (block.type == BlockType::Text) {
      const auto& t = block.asText.text;
      if (t.find("prompt too long") != std::string::npos ||
          t.find("413") != std::string::npos ||
          t.find("Payload Too Large") != std::string::npos ||
          t.find("prompt_too_long") != std::string::npos)
        return true;
    }
  }
  return false;
}

void QueryLoop::ApplyStepBudget(QueryLoopContext& ctx,
                                QueryLoopInternalState& state) {
  for (auto& msg : state.messagesForTurn) {
    if (msg.role != MessageRole::User) continue;
    int totalBytes = CountToolResultBytes(msg);
    if (totalBytes <= kPerMessageBudgetLimit) continue;

    int largestIdx = -1, largestSize = 0;
    for (int i = 0; i < static_cast<int>(msg.content.size()); ++i) {
      if (msg.content[i].type != BlockType::ToolResult) continue;
      int sz = static_cast<int>(msg.content[i].asToolResult.content.size());
      if (sz > largestSize) { largestSize = sz; largestIdx = i; }
    }
    if (largestIdx < 0) continue;
    const std::string& toolUseId =
        msg.content[largestIdx].asToolResult.toolUseId;

    if (ctx.replacementState.HasSeen(toolUseId)) {
      msg.content[largestIdx].asToolResult.content =
          ctx.replacementState.GetReplacement(toolUseId);
      continue;
    }

    const std::string& originalContent =
        msg.content[largestIdx].asToolResult.content;
    PersistOversizedResult(ctx.sessionDir, toolUseId, originalContent);

    std::ostringstream summary;
    summary << "[Large tool result (" << largestSize
            << " bytes) persisted to disk. Replacement summary: "
            << originalContent.substr(0, 200) << "...]";
    ctx.replacementState.RecordReplacement(toolUseId, summary.str());
    msg.content[largestIdx].asToolResult.content = summary.str();
  }
}

void QueryLoop::ApplyStepSnip(QueryLoopContext& ctx,
                              QueryLoopInternalState& state) {
  // Disabled: align with local-ace's snipCompact which is a no-op/disabled
  // feature. History snip aggressively truncates middle context, causing
  // information loss that required compensatory mechanisms like
  // BuildRecentExecutionMemory. Context management is handled by
  // autoCompact (LLM summarization) + microcompact (tool result clearing).
  (void)ctx;
  (void)state;
}

// ===== P2-01 Fix: Microcompact preserves tool name, result summary, and error status =====
void QueryLoop::ApplyStepMicrocompact(QueryLoopContext& ctx,
                                      QueryLoopInternalState& state) {
  (void)ctx;
  // Guard: skip when too few messages to have meaningful old tool results.
  // Aligned with local-ace's microcompact which returns early when no
  // compactable tool results exist.
  if (state.messagesForTurn.size() < 4) return;
  const long long now = CurrentTimeMs();

  // Build a map from tool_use_id to tool name by scanning all messages
  std::map<std::string, std::string> toolUseIdToName;
  for (const auto& msg : state.messagesForTurn) {
    for (const auto& block : msg.content) {
      if (block.type == BlockType::ToolUse &&
          !block.asToolUse.id.empty()) {
        toolUseIdToName[block.asToolUse.id] = block.asToolUse.name;
      }
    }
  }

  std::string latestToolResultId;
  for (const auto& msg : state.messagesForTurn) {
    for (const auto& block : msg.content) {
      if (block.type == BlockType::ToolResult &&
          !block.asToolResult.toolUseId.empty()) {
        latestToolResultId = block.asToolResult.toolUseId;
      }
    }
  }

  int compactedCount = 0;
  for (auto& msg : state.messagesForTurn) {
    for (auto& block : msg.content) {
      if (block.type != BlockType::ToolResult) continue;
      if (!latestToolResultId.empty() &&
          block.asToolResult.toolUseId == latestToolResultId) {
        continue;
      }
      if (static_cast<int>(block.asToolResult.content.size()) <=
          kMicroCompactOldMarkerBytes) continue;

      // P2-01: Build a compact summary instead of just a placeholder
      const std::string& toolUseId = block.asToolResult.toolUseId;
      std::string toolName = "unknown";
      auto it = toolUseIdToName.find(toolUseId);
      if (it != toolUseIdToName.end()) toolName = it->second;

      // Truncate to a reasonable summary (first 120 chars)
      const std::string& origContent = block.asToolResult.content;
      std::string summary;
      if (origContent.size() <= 200) {
        summary = origContent;
      } else {
        summary = origContent.substr(0, 120) + "...";
        // Replace newlines with spaces for compactness
        std::replace(summary.begin(), summary.end(), '\n', ' ');
      }

      std::ostringstream compact;
      compact << "[Tool: " << toolName << "] "
              << (block.asToolResult.isError ? "(error) " : "(ok) ")
              << "[" << summary << "]"
              << " [compacted " << static_cast<int>(origContent.size())
              << " bytes at " << now << "]";

      block.asToolResult.content = compact.str();
      ++compactedCount;
    }
  }
  if (compactedCount > 0) {
    Message boundary;
    boundary.role = MessageRole::System;
    boundary.uuid = "micro-compact-boundary";
    boundary.isMeta = true;
    boundary.content.push_back(ContentBlock::MakeText(
        "[microcompact] " + std::to_string(compactedCount) +
        " old tool results compacted (tool name + summary retained)."));
    state.messagesForTurn.insert(state.messagesForTurn.begin(), boundary);
  }
}

void QueryLoop::ApplyStepCollapse(QueryLoopContext& ctx,
                                  QueryLoopInternalState& state) {
  // Alignment with local-ace: isContextCollapseEnabled() always returns false
  // in local-ace (disabled feature flag). Context management is handled by
  // microcompact (tool result clearing) and autoCompact (LLM summarization).
  // Collapse should only fire as a LAST-RESORT safety valve when autoCompact
  // has failed and context is about to overflow.
  //
  // Changes from previous version:
  // - min messages raised from 10 to 30 (need substantial history)
  // - threshold raised to 95% of context window (last resort only)
  // - Added model-aware context window calculation
  if (state.messagesForTurn.size() < 30) return;
  const int estimatedTokens = EstimateMessageTokens(state.messagesForTurn);
  const int contextWindow = GetContextWindowForFamily(ctx.model);
  // Last-resort threshold: 95% of effective context window.
  // This ensures collapse only fires when autoCompact hasn't kept up.
  const int effectiveWindow = contextWindow - kMaxOutputTokensForSummary;
  const int threshold = static_cast<int>(effectiveWindow * 0.95);
  if (estimatedTokens <= threshold) return;
  const int beforeCount = static_cast<int>(state.messagesForTurn.size());
  if (ctx.hookExecutor != nullptr) {
    ctx.hookExecutor->RunPreCompactHooks(
        "collapse", beforeCount, 10000);
  }
  int keepRecent = 20;
  if (static_cast<int>(state.messagesForTurn.size()) <= keepRecent) {
    keepRecent =
        std::max(5, static_cast<int>(state.messagesForTurn.size()) / 2);
  }
  state.messagesForTurn = DoCollapseCompact(state.messagesForTurn, keepRecent);
  if (ctx.hookExecutor != nullptr) {
    ctx.hookExecutor->RunPostCompactHooks(
        beforeCount,
        static_cast<int>(state.messagesForTurn.size()),
        std::max(0, beforeCount - static_cast<int>(state.messagesForTurn.size())),
        10000);
  }
}

bool QueryLoop::ApplyStepAutocompact(QueryLoopContext& ctx,
                                     QueryLoopInternalState& state) {
  // Guard: skip autocompact when too few messages. Aligned with local-ace's
  // autoCompactIfNeeded() which checks isAboveAutoCompactThreshold and
  // returns false when token count is well below threshold.
  if (state.messagesForTurn.size() < 10) return false;
  // Recursion guard: skip for compact/session_memory queries
  if (!ctx.querySource.empty() &&
      (ctx.querySource == "compact" || ctx.querySource == "session_memory")) {
    return false;
  }
  const int estimatedTokens = EstimateMessageTokens(state.messagesForTurn);
  // Use model-aware context window instead of hardcoded 200k
  const int contextWindow = GetContextWindowForFamily(ctx.model);
  const int threshold =
      contextWindow - kMaxOutputTokensForSummary - kAutoCompactBufferTokens;
  if (estimatedTokens <= threshold) return false;
  if (state.consecutiveAutoCompactFailures >= kAutoCompactMaxFailures)
    return false;
  const int beforeCount = static_cast<int>(state.messagesForTurn.size());
  if (ctx.hookExecutor != nullptr) {
    ctx.hookExecutor->RunPreCompactHooks(
        "autocompact", beforeCount, 15000);
  }

  std::vector<Message> compactInput;
  compactInput.push_back(state.messagesForTurn.back());

  std::vector<Message> summaryResponse =
      modelClient_.GenerateResponse(compactInput,
          "Summarize this conversation concisely as a bulleted list. "
          "Focus on key decisions, files modified, unresolved issues. "
          "Under 500 words.", ctx.model);

  if (summaryResponse.empty()) {
    ++state.consecutiveAutoCompactFailures;
    return false;
  }

  std::string summaryText = "Auto-compact summary (exceeded " +
      std::to_string(threshold) + " tokens)";
  if (!summaryResponse[0].content.empty() &&
      summaryResponse[0].content[0].type == BlockType::Text)
    summaryText = summaryResponse[0].content[0].asText.text;

  Message summary;
  summary.role = MessageRole::System;
  summary.uuid = "auto-compact";
  summary.isMeta = true;
  summary.content.push_back(ContentBlock::MakeText(summaryText));

  size_t keepCount = std::min<size_t>(3, state.messagesForTurn.size());
  std::vector<Message> compacted;
  for (size_t i = 0; i < keepCount; ++i)
    compacted.push_back(state.messagesForTurn[i]);
  compacted.push_back(summary);
  for (size_t i = keepCount; i < state.messagesForTurn.size(); ++i)
    compacted.push_back(state.messagesForTurn[i]);

  state.messagesForTurn = compacted;
  if (ctx.hookExecutor != nullptr) {
    ctx.hookExecutor->RunPostCompactHooks(
        beforeCount,
        static_cast<int>(state.messagesForTurn.size()),
        std::max(0, estimatedTokens - EstimateMessageTokens(state.messagesForTurn)),
        15000);
  }
  state.consecutiveAutoCompactFailures = 0;
  ctx.autoCompactTracking.compacted = true;
  ctx.autoCompactTracking.turnCounter = 0;
  ctx.autoCompactTracking.consecutiveFailures = 0;
  return true;
}

bool QueryLoop::ApplyStepModelCall(QueryLoopContext& ctx,
                                   QueryLoopInternalState& state) {
  state.messagesForTurn = state.messagesForTurn.empty()
      ? BuildMessagesForTurn(ctx, state)
      : state.messagesForTurn;
  state.assistantMessages.clear();
  state.toolResultMessages.clear();
  state.pendingFollowupMessages.clear();
  state.toolUseBlocks.clear();
  state.validatorRequestedRetry = false;
  state.forceContinuation = false;
  state.forceContinuationReason.clear();

  Message currentAssistant;
  currentAssistant.role = MessageRole::Assistant;
  currentAssistant.uuid = "stream-asst";

  std::ostringstream textBuffer;

  // Tool execution stays in the normal RunTools stage so guards such as
  // workspace-first validation can inspect streamed tool_use blocks before
  // any side effects happen.
  const bool useStreamingExecution = false;
  const std::string modelTraceId = MakeQueryLoopTraceId("model-call");
  ReportQueryLoopDebugEvent(
      "1", "QueryLoop.cpp:model-call:start",
      "[DEBUG] About to start model call",
      {{"turnCount", state.turnCount},
       {"messageCountForTurn", static_cast<int>(state.messagesForTurn.size())},
       {"contextMessageCount", static_cast<int>(ctx.messages.size())},
       {"activeModelCandidate", ctx.model},
       {"validatorEnabled", ShouldRunValidation(ctx)},
       {"useStreamingExecution", useStreamingExecution},
       {"maxOutputTokensOverride", state.maxOutputTokensOverride},
       {"transition", static_cast<int>(state.transition)}},
      modelTraceId);

  api::SseEventCallback callback =
      [&](const std::string& event, const std::string& data) {
    if (event == "text_delta") {
      textBuffer << data;
      Message streamMessage;
      streamMessage.role = MessageRole::Assistant;
      streamMessage.uuid = "stream-delta";
      streamMessage.content.push_back(ContentBlock::MakeText(data));
      EmitQueryLoopEvent(
          ctx, QueryLoopEvent::Type::AssistantMessage, QueryStage::ModelCall,
          &streamMessage);
    } else if (event == "tool_use") {
      if (!textBuffer.str().empty()) {
        currentAssistant.content.push_back(
            ContentBlock::MakeText(textBuffer.str()));
        textBuffer.str(""); textBuffer.clear();
      }
      std::string toolId;
      std::string toolName;
      std::string inputJson = "{}";
      try {
        auto j = json::parse(data);
        if (j.contains("id") && j["id"].is_string())
          toolId = j["id"].get<std::string>();
        if (j.contains("name") && j["name"].is_string())
          toolName = j["name"].get<std::string>();
        if (j.contains("input"))
          inputJson = j["input"].dump();
      } catch (...) {
      }
      if (!toolId.empty()) {
        ContentBlock tb = ContentBlock::MakeToolUse(toolId, toolName, inputJson);
        currentAssistant.content.push_back(tb);
        state.toolUseBlocks.push_back(tb);
        Message toolMessage;
        toolMessage.role = MessageRole::Assistant;
        toolMessage.uuid = "tool-progress-" + toolId;
        toolMessage.content.push_back(tb);
        EmitQueryLoopEvent(
            ctx, QueryLoopEvent::Type::ToolProgress, QueryStage::ModelCall,
            &toolMessage);
      }
    } else if (event == "stop_reason") {
      currentAssistant.stopReason = data;
    } else if (event == "api_error") {
      currentAssistant.isApiErrorMessage = true;
      currentAssistant.content.push_back(ContentBlock::MakeText(data));
      Message errorMessage;
      errorMessage.role = MessageRole::Assistant;
      errorMessage.uuid = "stream-api-error";
      errorMessage.isApiErrorMessage = true;
      errorMessage.content.push_back(ContentBlock::MakeText(data));
      EmitQueryLoopEvent(
          ctx, QueryLoopEvent::Type::AssistantMessage, QueryStage::ModelCall,
          &errorMessage);
    }
  };

  state.activeModel = ctx.model;
  if (ctx.sessionManager) {
    ctx.sessionManager->AppendModelIoRecord(
        infra::ModelIoLogKind::Main, "request", state.activeModel,
        ctx.systemPrompt, state.messagesForTurn, state.turnCount);
  }
  modelClient_.StreamResponse(state.messagesForTurn, ctx.systemPrompt,
                              state.activeModel,
                              BuildToolsJson(toolOrchestrator_.GetToolRegistry()),
                              callback,
                              state.maxOutputTokensOverride);
  ReportQueryLoopDebugEvent(
      "1", "QueryLoop.cpp:model-call:return",
      "[DEBUG] Model call returned control to QueryLoop",
      {{"turnCount", state.turnCount},
       {"assistantCount", static_cast<int>(state.assistantMessages.size())},
       {"toolUseCount", static_cast<int>(state.toolUseBlocks.size())},
       {"pendingFollowupCount",
        static_cast<int>(state.pendingFollowupMessages.size())},
       {"currentStopReason", currentAssistant.stopReason},
       {"currentIsApiError", currentAssistant.isApiErrorMessage},
       {"bufferedTextSize", static_cast<int>(textBuffer.str().size())}},
      modelTraceId);

  if (!textBuffer.str().empty())
    currentAssistant.content.push_back(
        ContentBlock::MakeText(textBuffer.str()));

  if (!currentAssistant.content.empty()) {
    currentAssistant.uuid = "asst-" +
        std::to_string(ctx.messages.size() + state.turnCount);
    state.assistantMessages.push_back(currentAssistant);
  }

  if (ctx.sessionManager) {
    ctx.sessionManager->AppendModelIoRecord(
        infra::ModelIoLogKind::Main, "response", state.activeModel,
        std::string(), state.assistantMessages, state.turnCount);
  }

  return !state.toolUseBlocks.empty();
}

void QueryLoop::ApplyStepValidator(QueryLoopContext& ctx,
                                   QueryLoopInternalState& state) {
  if (!ShouldRunValidation(ctx)) return;
  if (state.assistantMessages.empty()) return;

  const std::string validatorModel = ResolveValidatorModel(ctx);
  const std::string validatorTraceId = MakeQueryLoopTraceId("validator");
  ReportQueryLoopDebugEvent(
      "2", "QueryLoop.cpp:validator:start",
      "[DEBUG] Starting validator side query",
      {{"turnCount", state.turnCount},
       {"validatorModel", validatorModel},
       {"assistantCount", static_cast<int>(state.assistantMessages.size())},
       {"toolUseCount", static_cast<int>(state.toolUseBlocks.size())}},
      validatorTraceId);

  std::string contextJson = BuildValidationContext(
      ctx.messages, state.assistantMessages, state.toolUseBlocks,
      toolOrchestrator_.GetToolRegistry(), toolOrchestrator_.workspaceRoot());

  api::SideQueryRequest request;
  request.querySource = "validator";
  request.model = validatorModel;
  request.systemPrompt = BuildValidatorSystemPrompt();
  request.messages.clear();

  Message userMsg;
  userMsg.role = MessageRole::User;
  userMsg.content.push_back(ContentBlock::MakeText(contextJson));
  request.messages.push_back(userMsg);

  if (ctx.sessionManager) {
    ctx.sessionManager->AppendModelIoRecord(
        infra::ModelIoLogKind::Validator, "request", validatorModel,
        request.systemPrompt, request.messages, state.turnCount);
  }

  api::SideQueryResponse response = sideQueryClient_.Query(request);
  ReportQueryLoopDebugEvent(
      "2", "QueryLoop.cpp:validator:return",
      "[DEBUG] Validator side query returned",
      {{"turnCount", state.turnCount},
       {"ok", response.ok},
       {"error", response.error},
       {"messageCount", static_cast<int>(response.messages.size())}},
      validatorTraceId);
  if (ctx.sessionManager) {
    ctx.sessionManager->AppendModelIoRecord(
        infra::ModelIoLogKind::Validator, "response", validatorModel,
        std::string(), response.messages, state.turnCount, response.error);
  }
  if (!response.ok) return;

  std::string fullResponse;
  for (const auto& msg : response.messages)
    for (const auto& block : msg.content)
      if (block.type == BlockType::Text)
        fullResponse += block.asText.text;

  const std::string traceId = MakeQueryLoopTraceId("validator");
  ReportQueryLoopDebugEvent(
      "2", "QueryLoop.cpp:validator:raw",
      "[DEBUG] Validator raw response collected",
      {{"turnCount", state.turnCount},
       {"assistantCount", static_cast<int>(state.assistantMessages.size())},
       {"toolUseCount", static_cast<int>(state.toolUseBlocks.size())},
       {"responseSize", static_cast<int>(fullResponse.size())},
       {"responsePrefix", TruncateDebugText(fullResponse)}},
      traceId);

  ValidationResult vresult = ParseValidationResponse(fullResponse);
  ReportQueryLoopDebugEvent(
      "2", "QueryLoop.cpp:validator:parsed",
      "[DEBUG] Validator parsed response",
      {{"turnCount", state.turnCount},
       {"hasValidationJson",
        fullResponse.find("<validation_json>") != std::string::npos},
       {"finalResponseAction", vresult.finalResponseAction},
       {"retryGuidancePrefix", TruncateDebugText(vresult.retryGuidance)},
       {"correctedTextSize", static_cast<int>(vresult.correctedText.size())},
       {"toolInterventionCount",
        static_cast<int>(vresult.toolInterventions.size())}},
      traceId);

  if (!vresult.correctedText.empty())
    ApplyTextCorrection(vresult.correctedText, state.assistantMessages);

  if (!vresult.toolInterventions.empty()) {
    ToolInterventionResult tir;
    ApplyToolInterventions(vresult.toolInterventions, state.toolUseBlocks, tir);
    state.toolUseBlocks = tir.rewrittenBlocks;
    for (const auto& [blockId, guidance] : tir.blockGuidance) {
      Message synthetic;
      synthetic.role = MessageRole::User;
      synthetic.uuid = "blocked-" + blockId;
      synthetic.isMeta = true;
      synthetic.content.push_back(ContentBlock::MakeToolResult(
          blockId,
          "Tool call blocked by validation: " + guidance,
          true));
      state.toolResultMessages.push_back(synthetic);
    }
    if (state.toolUseBlocks.empty() && !state.toolResultMessages.empty()) {
      state.forceContinuation = true;
      state.forceContinuationReason = "validator_blocked_tools";
    }
  }

  if (vresult.finalResponseAction == "retry_from_tools") {
    const std::string retryGuidance =
        vresult.retryGuidance.empty()
            ? "Retry from tools."
            : vresult.retryGuidance;
    state.validatorRequestedRetry = true;
    ++state.validatorRetryCount;
    state.lastValidatorGuidance = retryGuidance;
    state.toolUseBlocks.clear();
    Message guidance;
    guidance.role = MessageRole::User;
    guidance.uuid = "validator-retry";
    guidance.isMeta = true;
    guidance.content.push_back(ContentBlock::MakeText(
        "[Validator] " + retryGuidance));
    state.pendingFollowupMessages.push_back(guidance);
  } else {
    state.validatorRetryCount = 0;
    state.lastValidatorGuidance.clear();
  }
}

bool QueryLoop::Handle413Recovery(QueryLoopContext& ctx,
                                  QueryLoopInternalState& state) {
  if (state.assistantMessages.empty()) return false;
  const Message& lastMsg = state.assistantMessages.back();
  if (!lastMsg.isApiErrorMessage) return false;
  if (!IsPromptTooLong(lastMsg)) return false;

  if (!state.hasAttemptedCollapseDrain &&
      state.transition != TransitionReason::CollapseDrainRetry) {
    state.hasAttemptedCollapseDrain = true;
    ctx.messages = DoCollapseCompact(ctx.messages, 10);
    state.transition = TransitionReason::CollapseDrainRetry;
    state.assistantMessages.clear();
    state.toolUseBlocks.clear();
    return true;
  }

  if (!state.hasAttemptedReactiveCompact) {
    state.hasAttemptedReactiveCompact = true;
    ctx.hasAttemptedReactiveCompact = true;
    int keepRecent = std::max(1, static_cast<int>(ctx.messages.size()) / 4);
    ctx.messages = DoCollapseCompact(ctx.messages, keepRecent);
    if (!ctx.messages.empty()) {
      Message recoveryNote;
      recoveryNote.role = MessageRole::System;
      recoveryNote.uuid = "reactive-compact-note";
      recoveryNote.isMeta = true;
      recoveryNote.content.push_back(ContentBlock::MakeText(
          "[ReactiveCompact] Context reduced due to 413 error. "
          "Continue from where you left off. No recap needed."));
      ctx.messages.push_back(recoveryNote);
    }
    state.transition = TransitionReason::ReactiveCompactRetry;
    state.assistantMessages.clear();
    state.toolUseBlocks.clear();
    return true;
  }

  state.completed = true;
  state.terminalReason = "prompt_too_long";
  return false;
}

bool QueryLoop::HandleMaxOutputTokens(QueryLoopContext& ctx,
                                      QueryLoopInternalState& state) {
  if (state.assistantMessages.empty()) return false;
  const Message& lastMsg = state.assistantMessages.back();
  if (!lastMsg.isApiErrorMessage) return false;

  bool isMaxTokens = false;
  for (const auto& block : lastMsg.content) {
    if (block.type != BlockType::Text) continue;
    if (block.asText.text.find("max_output_tokens") != std::string::npos ||
        block.asText.text.find("output token limit") != std::string::npos) {
      isMaxTokens = true; break;
    }
  }
  if (!isMaxTokens && !lastMsg.stopReason.empty() &&
      (lastMsg.stopReason.find("max_tokens") != std::string::npos ||
       lastMsg.stopReason.find("length") != std::string::npos))
    isMaxTokens = true;
  // P0-FIX: Also detect max_output_tokens when model returns text-only
  // without tools (Qwen may return stop_reason="stop" when truncated).
  if (!isMaxTokens && state.toolUseBlocks.empty() &&
      !lastMsg.isApiErrorMessage) {
    std::string lastText = CollectText(state.assistantMessages);
    // Heuristic: long text with no tools suggests token limit truncation
    if (lastText.size() > 2000 && state.forcedContinuationCount >= 3) {
      isMaxTokens = true;
    }
  }
  if (!isMaxTokens) return false;

  if (state.maxOutputTokensOverride == 0 &&
      state.transition != TransitionReason::MaxOutputTokensEscalate) {
    state.maxOutputTokensOverride = kEscalatedMaxTokens;
    state.transition = TransitionReason::MaxOutputTokensEscalate;
    state.assistantMessages.clear();
    state.toolUseBlocks.clear();
    return true;
  }

  if (state.maxOutputTokensRecoveryCount < kMaxOutputTokensRecoveryLimit) {
    ++state.maxOutputTokensRecoveryCount;
    Message recovery;
    recovery.role = MessageRole::System;
    recovery.uuid = "recovery-msg";
    recovery.isMeta = true;
    recovery.content.push_back(ContentBlock::MakeText(
        "Output token limit hit. Resume directly - no apology, "
        "no recap of what you were doing. Pick up mid-thought. "
        "Break remaining work into smaller pieces."));
    for (const auto& am : state.assistantMessages)
      ctx.messages.push_back(am);
    PersistMessagesToTranscript(ctx.sessionManager, state.assistantMessages);
    ctx.messages.push_back(recovery);
    PersistMessagesToTranscript(ctx.sessionManager, {recovery});
    state.assistantMessages.clear();
    state.toolUseBlocks.clear();
    state.transition = TransitionReason::MaxOutputTokensRecovery;
    return true;
  }
  return false;
}

bool QueryLoop::HandleTokenBudget(QueryLoopContext& ctx,
                                  QueryLoopInternalState& state) {
  int outputTokens = 0;
  for (const auto& msg : state.assistantMessages)
    outputTokens += msg.usage.outputTokens;
  static const int kTurnTokenBudget = 500000;
  if (outputTokens < kTurnTokenBudget) return false;

  Message nudge;
  nudge.role = MessageRole::System;
  nudge.uuid = "budget-nudge";
  nudge.isMeta = true;
  nudge.content.push_back(ContentBlock::MakeText(
      "Token budget approaching limit. Be concise and focus on essentials."));
  for (const auto& am : state.assistantMessages)
    ctx.messages.push_back(am);
  PersistMessagesToTranscript(ctx.sessionManager, state.assistantMessages);
  ctx.messages.push_back(nudge);
  PersistMessagesToTranscript(ctx.sessionManager, {nudge});
  state.assistantMessages.clear();
  state.toolUseBlocks.clear();
  state.transition = TransitionReason::TokenBudgetContinuation;
  state.maxOutputTokensRecoveryCount = 0;
  state.hasAttemptedReactiveCompact = false;
  return true;
}

StopHookResult QueryLoop::ExecuteStopHooks(QueryLoopContext& ctx,
                                           QueryLoopInternalState& state) {
  StopHookResult result;
  if (state.assistantMessages.empty()) return result;
  if (state.stopHookActive) return result;
  const Message& lastMsg = state.assistantMessages.back();
  if (lastMsg.isApiErrorMessage) return result;

  bool hasUnresolvedTools = false;
  for (const auto& am : state.assistantMessages)
    for (const auto& block : am.content)
      if (block.type == BlockType::ToolUse) {
        hasUnresolvedTools = true; break;
      }

  if (!hasUnresolvedTools) {
    bool hasContent = false;
    for (const auto& am : state.assistantMessages)
      for (const auto& block : am.content)
        if (block.type == BlockType::Text) hasContent = true;
    if (!hasContent) {
      if (state.maxOutputTokensRecoveryCount < kMaxOutputTokensRecoveryLimit) {
        ++state.maxOutputTokensRecoveryCount;
        Message cont;
        cont.role = MessageRole::System;
        cont.uuid = "auto-continue";
        cont.isMeta = true;
        cont.content.push_back(ContentBlock::MakeText(
            "[Continue] Auto-continuing turn " +
            std::to_string(state.maxOutputTokensRecoveryCount + 1)));
        result.followupMessages.push_back(cont);
        return result;
      }
    }
  }

  if (ctx.hookExecutor != nullptr) {
    const std::string stopReason =
        !lastMsg.stopReason.empty()
            ? lastMsg.stopReason
            : (hasUnresolvedTools ? "tool_use" : "end_turn");
    const hooks::HookBatchResult batch =
        ctx.hookExecutor->RunStopHooks(stopReason, 30000);
    MergeHookMessages(batch, "stop-hook", &result.followupMessages,
                      &result.blockingErrors);
    for (const auto& hookResult : batch.results) {
      if (hookResult.outcome == hooks::HookOutcome::Blocking) {
        result.preventContinuation = false;
      }
      if (hookResult.shouldStop && result.followupMessages.empty() &&
          result.blockingErrors.empty()) {
        result.preventContinuation = true;
      }
    }
    if (!result.followupMessages.empty() || !result.blockingErrors.empty() ||
        result.preventContinuation) {
      state.stopHookActive = true;
    }
  }
  return result;
}

bool QueryLoop::ApplyStepRunTools(QueryLoopContext& ctx,
                                  QueryLoopInternalState& state) {
  if (permissionEngine_.GetPermissionMode() == PermissionMode::Plan) {
    for (const auto& msg : state.assistantMessages)
      ctx.messages.push_back(msg);
    PersistMessagesToTranscript(ctx.sessionManager, state.assistantMessages);

    std::vector<Message> simulatedResults;
    for (const auto& block : state.toolUseBlocks) {
      Message toolMsg;
      toolMsg.role = MessageRole::User;
      toolMsg.uuid = "plan-tool-" + block.asToolUse.id;
      toolMsg.content.push_back(ContentBlock::MakeToolResult(
          block.asToolUse.id,
          "[plan mode] Tool execution skipped. Intended call: " +
              block.asToolUse.name + " " + block.asToolUse.inputJson,
          false));
      simulatedResults.push_back(toolMsg);
      ctx.messages.push_back(toolMsg);
    }

    PersistMessagesToTranscript(ctx.sessionManager, simulatedResults);
    for (const auto& toolMsg : simulatedResults) {
      EmitQueryLoopEvent(
          ctx, QueryLoopEvent::Type::ToolResult, QueryStage::RunTools,
          &toolMsg);
    }
    state.assistantMessages.clear();
    state.stopHookActive = false;
    state.validatorRetryCount = 0;
    state.lastValidatorGuidance.clear();
    state.forcedContinuationCount = 0;
    state.toolUseBlocks.clear();
    return !simulatedResults.empty();
  }

  auto canUseTool = permissionEngine_.BuildCanUseTool();
  tools::ToolOrchestrator::ExecuteResult execResult =
      toolOrchestrator_.Execute(state.toolUseBlocks, canUseTool,
                                ctx.messages);
  ReportQueryLoopDebugEvent(
      "3", "QueryLoop.cpp:run-tools:result",
      "[DEBUG] Tool execution finished",
      {{"turnCount", state.turnCount},
       {"requestedToolCount", static_cast<int>(state.toolUseBlocks.size())},
       {"toolResultCount", static_cast<int>(execResult.userMessages.size())},
       {"deniedCount", execResult.deniedCount},
       {"errorCount", execResult.errorCount},
       {"firstToolResultIsError",
        !execResult.userMessages.empty() &&
            !execResult.userMessages.front().content.empty() &&
            execResult.userMessages.front().content.front().type ==
                BlockType::ToolResult
            ? execResult.userMessages.front()
                  .content.front()
                  .asToolResult.isError
            : false},
       {"firstToolResultPrefix",
        !execResult.userMessages.empty() &&
            !execResult.userMessages.front().content.empty() &&
            execResult.userMessages.front().content.front().type ==
                BlockType::ToolResult
            ? TruncateDebugText(execResult.userMessages.front()
                                    .content.front()
                                    .asToolResult.content)
            : std::string()}},
      MakeQueryLoopTraceId("tools"));
  for (const auto& msg : state.assistantMessages)
    ctx.messages.push_back(msg);
  for (const auto& rm : execResult.userMessages)
    ctx.messages.push_back(rm);
  PersistMessagesToTranscript(ctx.sessionManager, state.assistantMessages);
  PersistMessagesToTranscript(ctx.sessionManager, execResult.userMessages);
  for (const auto& rm : execResult.userMessages) {
    EmitQueryLoopEvent(
        ctx, QueryLoopEvent::Type::ToolResult, QueryStage::RunTools, &rm);
  }
  state.assistantMessages.clear();
  state.stopHookActive = false;
  state.validatorRetryCount = 0;
  state.lastValidatorGuidance.clear();
  // P0-FIX: Only reset forced-continuation count when the model
  // takes productive action (Write/Bash/TaskCreate), not on exploration reads.
  {
    bool hasProductiveTool = false;
    for (const auto& block : state.toolUseBlocks) {
      if (block.type != BlockType::ToolUse) continue;
      const std::string& name = block.asToolUse.name;
      if (IsWorkspaceWriteToolName(name) ||
          name == "Bash" || name == "TaskCreate") {
        hasProductiveTool = true;
        break;
      }
    }
    if (hasProductiveTool) {
      state.forcedContinuationCount = 0;
    }
  }
  state.toolUseBlocks.clear();
  return !execResult.userMessages.empty();
}

bool QueryLoop::ApplyStepTerminate(QueryLoopContext& ctx,
                                   QueryLoopInternalState& state) {
  state.toolUseBlocks = CollectToolUseBlocks(state.assistantMessages);
  if (state.toolUseBlocks.empty()) {
    AppendTurnArtifacts(
        ctx, state.assistantMessages, state.toolResultMessages,
        state.pendingFollowupMessages);
    auto& at = ctx.autoCompactTracking;
    if (at.compacted) { at.compacted = false; at.turnId.clear(); }
    state.assistantMessages.clear();
    state.toolResultMessages.clear();
    state.pendingFollowupMessages.clear();
    state.completed = true;
    state.terminalReason = "completed";
    return false;
  }
  return true;
}

// Check if the conversation history contains recent tool_result blocks,
// indicating the model was actively using tools before this no-tool response.
static bool HasRecentToolActivity(const QueryLoopContext& ctx) {
  // Scan the last N messages for tool_result content blocks
  const int msgCount = static_cast<int>(ctx.messages.size());
  const int scanWindow = (msgCount < 20) ? msgCount : 20;
  const int start = msgCount - scanWindow;
  for (int i = start; i < msgCount; ++i) {
    for (const auto& block : ctx.messages[i].content) {
      if (block.type == BlockType::ToolResult) return true;
    }
  }
  return false;
}

bool QueryLoop::HandleNoToolContinuation(QueryLoopContext& ctx,
                                         QueryLoopInternalState& state) {
  static const int kMaxValidatorRetryContinuations = 3;
  static const int kMaxRepeatedErrorToolResults = 3;
  ReportQueryLoopDebugEvent(
      "4", "QueryLoop.cpp:no-tool:entry",
      "[DEBUG] Evaluating no-tool continuation",
      {{"turnCount", state.turnCount},
       {"validatorRequestedRetry", state.validatorRequestedRetry},
       {"pendingFollowupCount",
        static_cast<int>(state.pendingFollowupMessages.size())},
       {"toolResultMessageCount",
        static_cast<int>(state.toolResultMessages.size())},
       {"assistantPreview",
        TruncateDebugText(CollectText(state.assistantMessages))},
       {"forceContinuation", state.forceContinuation},
       {"forceContinuationReason", state.forceContinuationReason}},
      MakeQueryLoopTraceId("no-tool"));
  if (state.validatorRequestedRetry &&
      state.validatorRetryCount >= kMaxValidatorRetryContinuations) {
    if (state.validatorNudgeCount >= 1) {
      // Already nudged once without improvement - hard-terminate
      Message note;
      note.role = MessageRole::System;
      note.uuid = "validator-retry-limit";
      note.isMeta = true;
      std::string text =
          "[system] Terminating: validator requested retry_from_tools "
          + std::to_string(state.validatorRetryCount
              + state.validatorNudgeCount * kMaxValidatorRetryContinuations)
          + " consecutive times across multiple cycles. Stop the retry loop"
          " and surface the current state.";
      if (!state.lastValidatorGuidance.empty()) {
        text += " Latest guidance: " + state.lastValidatorGuidance;
      }
      note.content.push_back(ContentBlock::MakeText(text));
      AppendTurnArtifacts(
          ctx, state.assistantMessages, state.toolResultMessages, {note});
      state.assistantMessages.clear();
      state.toolResultMessages.clear();
      state.pendingFollowupMessages.clear();
      state.toolUseBlocks.clear();
      state.completed = true;
      state.terminalReason = "validator_retry_limit";
      return false;
    }
    // First cycle: send a stronger nudge (aligned with local-ace forced continuation)
    ++state.validatorNudgeCount;
    state.validatorRetryCount = 0;
    Message nudge;
    nudge.role = MessageRole::User;
    nudge.uuid = "validator-retry-limit-nudge";
    nudge.isMeta = true;
    std::string nudgeText =
        "[system] Validator has requested retry_from_tools "
        + std::to_string(kMaxValidatorRetryContinuations)
        + " consecutive times. You MUST now take a concrete action: either"
        + " provide a complete final answer or execute a tool call immediately."
        + " Do NOT repeat the same plan or reasoning.";
    if (!state.lastValidatorGuidance.empty()) {
      nudgeText += " Validator guidance: " + state.lastValidatorGuidance;
    }
    nudge.content.push_back(ContentBlock::MakeText(nudgeText));
    std::vector<Message> nudgeFollowups = {nudge};
    return ContinueWithFollowup(
        ctx, state, nudgeFollowups,
        TransitionReason::ValidatorRetry, false);
  }
  if (state.validatorRequestedRetry || !state.pendingFollowupMessages.empty()) {
    ReportQueryLoopDebugEvent(
        "3", "QueryLoop.cpp:no-tool:validator-retry",
        "[DEBUG] Continuing turn due to validator retry (incremental, skip pipeline)",
        {{"turnCount", state.turnCount},
         {"followupCount", static_cast<int>(state.pendingFollowupMessages.size())},
         {"assistantCount", static_cast<int>(state.assistantMessages.size())},
         {"messagesForTurnSize", static_cast<int>(state.messagesForTurn.size())}},
        MakeQueryLoopTraceId("continue"));

    // Append turn artifacts to persistent transcript
    for (const auto& am : state.assistantMessages) ctx.messages.push_back(am);
    for (const auto& tr : state.toolResultMessages) ctx.messages.push_back(tr);
    for (const auto& fu : state.pendingFollowupMessages) ctx.messages.push_back(fu);
    if (ctx.sessionManager) {
      for (const auto& am : state.assistantMessages)
        ctx.sessionManager->AppendMessageToTranscript(am);
      for (const auto& tr : state.toolResultMessages)
        ctx.sessionManager->AppendMessageToTranscript(tr);
      for (const auto& fu : state.pendingFollowupMessages)
        ctx.sessionManager->AppendMessageToTranscript(fu);
    }

    // Build next turn messages incrementally from current turn + new data
    // This avoids the expensive full pipeline (ToolResultBudget-Snip-Microcompact-Collapse)
    std::vector<Message> nextMessages = state.messagesForTurn;
    for (const auto& am : state.assistantMessages) nextMessages.push_back(am);
    for (const auto& tr : state.toolResultMessages) nextMessages.push_back(tr);
    for (const auto& fu : state.pendingFollowupMessages) nextMessages.push_back(fu);

    state.messagesForTurn = std::move(nextMessages);
    state.assistantMessages.clear();
    state.toolResultMessages.clear();
    state.pendingFollowupMessages.clear();
    state.toolUseBlocks.clear();
    state.forceContinuation = false;
    state.forceContinuationReason.clear();

    // Inject fresh execution memory at FRONT so the model sees error-loop
    // warnings BEFORE re-processing any conversation history. This is the
    // critical fix for the jianlai-graph restart-from-beginning bug.
    const std::string execMem = BuildRecentExecutionMemory(ctx, state);
    if (!execMem.empty()) {
      Message memMsg;
      memMsg.role = MessageRole::System;
      memMsg.uuid = "recent-execution-memory";
      memMsg.isMeta = true;
      memMsg.content.push_back(ContentBlock::MakeText(execMem));
      state.messagesForTurn.insert(state.messagesForTurn.begin(), memMsg);
    }

    state.transition = TransitionReason::ValidatorRetry;
    state.stage = QueryStage::Autocompact;
    return true;
  }

  StopHookResult hooksResult = ExecuteStopHooks(ctx, state);
  if (hooksResult.preventContinuation) {
    state.completed = true;
    state.terminalReason = "stop_hook_prevented";
    return false;
  }
  if (!hooksResult.followupMessages.empty()) {
    return ContinueWithFollowup(
        ctx, state, hooksResult.followupMessages,
        TransitionReason::ForcedContinuation, false);
  }
  if (!hooksResult.blockingErrors.empty()) {
    return ContinueWithFollowup(
        ctx, state, hooksResult.blockingErrors,
        TransitionReason::StopHookBlocking, false);
  }

  // --- Bounded continuation nudge (aligned with local-ace max_output_tokens
  // recovery). When the model was actively using tools and then stops with
  // text-only output (no tool_use), it likely hit a generation limit rather
  // than genuinely finishing. Inject a short "resume" nudge, capped at 3
  // attempts to prevent death spirals.
  static const int kMaxNoToolNudges = 3;
  const bool hasPriorToolActivity = HasRecentToolActivity(ctx);
  const bool modelProducedText = !CollectText(state.assistantMessages).empty();

  if (hasPriorToolActivity && modelProducedText &&
      state.missingToolUsePromptCount < kMaxNoToolNudges) {
    ++state.missingToolUsePromptCount;
    ReportQueryLoopDebugEvent(
        "4", "QueryLoop.cpp:no-tool:bounded-nudge",
        "[DEBUG] Injecting bounded continuation nudge",
        {{"nudgeCount", state.missingToolUsePromptCount},
         {"modelCallCount", state.modelCallCount},
         {"turnCount", state.turnCount}},
        MakeQueryLoopTraceId("nudge"));

    Message nudge;
    nudge.role = MessageRole::User;
    nudge.uuid = "bounded-continuation-nudge-"
                 + std::to_string(state.missingToolUsePromptCount);
    nudge.isMeta = true;
    // Escalating nudge messages (aligned with local-ace max_output_tokens
    // recovery). Later nudges are more directive to break the model out of
    // text-only planning loops.
    std::string nudgeText;
    if (state.missingToolUsePromptCount <= 1) {
      nudgeText =
          "[system] Your previous response ended without a tool call. "
          "Resume directly - no apology, no recap of what you were doing. "
          "Pick up mid-thought if that is where the cut happened. "
          "Break remaining work into smaller pieces.";
    } else {
      // Stronger directive for repeated failures
      nudgeText =
          "[system] You keep describing what you will do instead of doing it. "
          "STOP planning. Emit a Write tool call NOW for the next file. "
          "If the file is large, create a skeleton first with class/function "
          "stubs, then use Write to add implementation in separate calls. "
          "Do NOT describe the file content in text - just write it.";
    }
    nudge.content.push_back(ContentBlock::MakeText(nudgeText));
    std::vector<Message> nudgeFollowups = {nudge};
    return ContinueWithFollowup(
        ctx, state, nudgeFollowups,
        TransitionReason::ForcedContinuation, false);
  }

  // Align with local-ace: when model produces no tool_use and no validator
  // retry is needed, check token budget then terminate.
  if (HandleTokenBudget(ctx, state)) {
    state.stage = QueryStage::ToolResultBudget;
    return true;
  }

  ReportQueryLoopDebugEvent(
      "4", "QueryLoop.cpp:no-tool:terminate",
      "[DEBUG] Completing turn without additional continuation",
      {{"turnCount", state.turnCount},
       {"assistantCount", static_cast<int>(state.assistantMessages.size())},
       {"toolResultCount", static_cast<int>(state.toolResultMessages.size())},
       {"nudgeCount", state.missingToolUsePromptCount}},
      MakeQueryLoopTraceId("terminate"));
  ApplyStepTerminate(ctx, state);
  return false;
}

void QueryLoop::RunFull(QueryLoopContext& ctx) {
  QueryLoopInternalState state;
  loopStartTimeMs_ = CurrentTimeMs();

  auto persistMsg = [&ctx](const Message& msg) {
    if (ctx.sessionManager) ctx.sessionManager->AppendMessageToTranscript(msg);
  };
  auto persistMsgs = [&persistMsg](const std::vector<Message>& msgs) {
    for (const auto& m : msgs) persistMsg(m);
  };

  while (!state.completed) {
    if (state.stage == QueryStage::ToolResultBudget &&
        state.modelCallCount > 0 &&
        IsWallClockExpired(ctx)) {
      state.completed = true;
      state.terminalReason = "wall_clock_budget_exceeded";
      continue;
    }
    EmitQueryLoopEvent(
        ctx, QueryLoopEvent::Type::StageChanged, state.stage, nullptr);
    ReportQueryLoopDebugEvent(
        "1", "QueryLoop.cpp:stage:enter",
        "[DEBUG] Entering query loop stage",
        {{"stage", QueryStageToString(state.stage)},
         {"turnCount", state.turnCount},
         {"messageCount", static_cast<int>(ctx.messages.size())},
         {"assistantCount", static_cast<int>(state.assistantMessages.size())},
         {"toolResultCount", static_cast<int>(state.toolResultMessages.size())},
         {"pendingFollowupCount",
          static_cast<int>(state.pendingFollowupMessages.size())},
         {"transition", static_cast<int>(state.transition)}},
        MakeQueryLoopTraceId("stage"));
    switch (state.stage) {
      case QueryStage::ToolResultBudget: {
        state.messagesForTurn = BuildMessagesForTurn(ctx, state);
        state.nextTurnCount = state.turnCount + 1;
        ++state.turnCount;
        if (maxTurns_ > 0 && state.turnCount > maxTurns_) {
          state.completed = true;
          state.terminalReason = "max_turns";
          continue;
        }
        ApplyStepBudget(ctx, state);
        // First-iteration fast path: skip compact stages when context is
        // trivially small. Aligned with local-ace where snip/microcompact/
        // collapse/autocompact all no-op on first turn because their
        // threshold checks (token count, message count) are not met.
        const int msgCount = static_cast<int>(state.messagesForTurn.size());
        const bool isFirstIteration = (state.modelCallCount == 0);
        const bool contextIsSmall = (msgCount < 10);
        if (isFirstIteration && contextIsSmall) {
          state.stage = QueryStage::ModelCall;
          continue;
        }
        state.stage = QueryStage::Snip;
        continue;
      }
      case QueryStage::Snip: {
        ApplyStepSnip(ctx, state);
        state.stage = QueryStage::Microcompact;
        continue;
      }
      case QueryStage::Microcompact: {
        ApplyStepMicrocompact(ctx, state);
        state.stage = QueryStage::Collapse;
        continue;
      }
      case QueryStage::Collapse: {
        ApplyStepCollapse(ctx, state);
        state.stage = QueryStage::Autocompact;
        continue;
      }
      case QueryStage::Autocompact: {
        ApplyStepAutocompact(ctx, state);
        state.stage = QueryStage::ModelCall;
        continue;
      }
      case QueryStage::ModelCall: {
        bool hasTools = ApplyStepModelCall(ctx, state);
        ++state.modelCallCount;

        if (Handle413Recovery(ctx, state)) {
          state.stage = QueryStage::ToolResultBudget; continue;
        }
        if (HandleMaxOutputTokens(ctx, state)) {
          state.stage = QueryStage::ToolResultBudget; continue;
        }
        if (state.assistantMessages.empty()) {
          Message empty;
          empty.role = MessageRole::System;
          empty.uuid = "empty-response";
          empty.isMeta = true;
          empty.content.push_back(ContentBlock::MakeText(
              "[error] LLM returned an empty response."));
          ctx.messages.push_back(empty);
          persistMsg(empty);
          state.completed = true;
          state.terminalReason = "empty_response"; continue;
        }
        const Message& lastMsg = state.assistantMessages.back();
        ReportQueryLoopDebugEvent(
            "1", "QueryLoop.cpp:model-call:result",
            "[DEBUG] Model call produced assistant messages",
            {{"turnCount", state.turnCount},
             {"assistantCount", static_cast<int>(state.assistantMessages.size())},
             {"toolUseCount",
              static_cast<int>(CollectToolUseBlocks(state.assistantMessages).size())},
             {"lastStopReason", lastMsg.stopReason},
             {"lastIsApiError", lastMsg.isApiErrorMessage},
             {"assistantPreview",
              TruncateDebugText(CollectText(state.assistantMessages))}},
            MakeQueryLoopTraceId("model"));
        if (state.toolUseBlocks.empty() &&
            wallClockBudgetMs_ > 0 &&
            loopStartTimeMs_ > 0 &&
            CurrentTimeMs() - loopStartTimeMs_ >= wallClockBudgetMs_) {
          AppendTurnArtifacts(
              ctx, state.assistantMessages, state.toolResultMessages,
              state.pendingFollowupMessages);
          state.assistantMessages.clear();
          state.toolResultMessages.clear();
          state.pendingFollowupMessages.clear();
          state.toolUseBlocks.clear();
          IsWallClockExpired(ctx);
          state.completed = true;
          state.terminalReason = "wall_clock_budget_exceeded";
          continue;
        }
        if (lastMsg.isApiErrorMessage &&
            !IsPromptTooLong(lastMsg) &&
            state.transition != TransitionReason::MaxOutputTokensRecovery &&
            state.transition != TransitionReason::MaxOutputTokensEscalate) {
          if (!ctx.fallbackModel.empty() && state.activeModel != ctx.fallbackModel) {
            Message warning;
            warning.role = MessageRole::System;
            warning.uuid = "fallback-warn";
            warning.isMeta = true;
            warning.content.push_back(ContentBlock::MakeText(
                "Fallback: switching from " + state.activeModel +
                " to " + ctx.fallbackModel));
            ctx.messages.push_back(warning);
            persistMsg(warning);
            ctx.model = ctx.fallbackModel;
            state.activeModel = ctx.fallbackModel;
            state.assistantMessages.clear();
            state.toolResultMessages.clear();
            state.pendingFollowupMessages.clear();
            state.toolUseBlocks.clear();
            state.stage = QueryStage::ToolResultBudget;
            continue;
          }
          AppendTurnArtifacts(
              ctx, state.assistantMessages, state.toolResultMessages, {});
          state.completed = true;
          state.terminalReason = "api_error"; continue;
        }
        (void)hasTools;
        if (ShouldRunValidation(ctx)) {
          state.stage = QueryStage::Validator;
        } else if (state.toolUseBlocks.empty()) {
          HandleNoToolContinuation(ctx, state);
        } else {
          if (HandleMissingWorkspaceExploration(ctx, state)) {
            continue;
          }
          state.stage = QueryStage::StopHooks;
        }
        continue;
      }
      case QueryStage::Validator: {
        ApplyStepValidator(ctx, state);
        if (state.validatorRequestedRetry ||
            !state.pendingFollowupMessages.empty()) {
          HandleNoToolContinuation(ctx, state);
          continue;
        }
        if (state.toolUseBlocks.empty()) {
          HandleNoToolContinuation(ctx, state);
          continue;
        }
        if (HandleMissingWorkspaceExploration(ctx, state)) {
          continue;
        }
        state.stage = QueryStage::StopHooks;
        continue;
      }
      case QueryStage::StopHooks: {
        StopHookResult hooksResult = ExecuteStopHooks(ctx, state);
        if (hooksResult.preventContinuation) {
          state.completed = true;
          state.terminalReason = "stop_hook_prevented"; continue;
        }
        if (!hooksResult.followupMessages.empty()) {
          ContinueWithFollowup(
              ctx, state, hooksResult.followupMessages,
              TransitionReason::ForcedContinuation, false);
          continue;
        }
        state.toolUseBlocks = CollectToolUseBlocks(state.assistantMessages);
        if (!hooksResult.blockingErrors.empty()) {
          for (const auto& am : state.assistantMessages)
            ctx.messages.push_back(am);
          persistMsgs(state.assistantMessages);
          for (const auto& err : hooksResult.blockingErrors)
            ctx.messages.push_back(err);
          persistMsgs(hooksResult.blockingErrors);
          state.assistantMessages.clear();
          state.toolResultMessages.clear();
          state.pendingFollowupMessages.clear();
          state.toolUseBlocks.clear();
          state.transition = TransitionReason::StopHookBlocking;
          state.stage = QueryStage::ToolResultBudget; continue;
        }
        state.stage = QueryStage::RunTools;
        continue;
      }
      case QueryStage::RunTools: {
        // P0-FIX: Check for excessive exploration-only turns
        if (ShouldTerminateOnExcessiveExploration(ctx, state)) {
          state.completed = true;
          state.terminalReason = "excessive_exploration";
          continue;
        }
        // P0-02: Check for duplicate tool call loops
        if (ShouldTerminateOnDuplicates(ctx, state)) {
          state.completed = true;
          state.terminalReason = "duplicate_tool_loop";
          continue;
        }
        bool hasToolResults = ApplyStepRunTools(ctx, state);
        if (!hasToolResults) {
          state.completed = true;
          state.terminalReason = "tool_execution_without_results";
          continue;
        }
        PostToolTurnProcessing(ctx, state);
        state.stage = QueryStage::ToolResultBudget;
        state.transition = TransitionReason::ToolResultContinuation;
        continue;
      }
      case QueryStage::Completed:
      default:
        state.completed = true;
        if (state.terminalReason.empty())
          state.terminalReason = "completed";
        continue;
    }
  }
  ReportQueryLoopDebugEvent(
      "4", "QueryLoop.cpp:run:complete",
      "[DEBUG] Query loop completed",
      {{"terminalReason", state.terminalReason},
       {"turnCount", state.turnCount},
       {"messageCount", static_cast<int>(ctx.messages.size())}},
      MakeQueryLoopTraceId("complete"));
  EmitQueryLoopEvent(
      ctx, QueryLoopEvent::Type::LoopCompleted, QueryStage::Completed, nullptr,
      state.terminalReason);
  ctx.lastTerminalReason = state.terminalReason;
  if (ctx.sessionManager != nullptr) {
    SessionMetadata metadata = ctx.sessionManager->metadata();
    metadata.lastTerminalReason = state.terminalReason;
    ctx.sessionManager->SetMetadata(metadata);
    ctx.sessionManager->AppendMessageToTranscript(
        MakeTerminalTranscriptRecord(state));
  }
  ctx.maxOutputTokensRecoveryCount = state.maxOutputTokensRecoveryCount;
  ctx.hasAttemptedReactiveCompact = state.hasAttemptedReactiveCompact;
}

}  // namespace core
}  // namespace agent
