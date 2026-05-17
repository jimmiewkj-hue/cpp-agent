#include "core/QueryLoop.h"

#include "api/ModelClient.h"
#include "api/SideQueryClient.h"
#include "core/StreamingToolExecutor.h"
#include "permissions/PermissionEngine.h"
#include "tools/ToolOrchestrator.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

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
static const int kMaxOutputTokensDefault = 4096;
static const int kMicroCompactAgeMs = 5 * 60 * 1000;

namespace {

std::vector<ContentBlock> CollectToolUseBlocks(
    const std::vector<Message>& messages) {
  std::vector<ContentBlock> toolUses;
  for (const auto& message : messages)
    for (const auto& block : message.content)
      if (block.type == BlockType::ToolUse)
        toolUses.push_back(block);
  return toolUses;
}

bool IsValidationEnabled() {
  char buffer[256] = {0};
  DWORD len = GetEnvironmentVariableA(
      "LOCALMODEL_VALIDATION_MODEL", buffer, sizeof(buffer));
  return len > 0 && len < sizeof(buffer);
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
    auto correctedText = ExtractJsonStringField(jsonBlock, "corrected_text");
    if (!correctedText.empty()) result.correctedText = correctedText;

    auto action = ExtractJsonStringField(jsonBlock, "final_response_action");
    if (action == "retry_from_tools")
      result.finalResponseAction = "retry_from_tools";

    auto guidance = ExtractJsonStringField(jsonBlock, "retry_guidance");
    if (!guidance.empty()) result.retryGuidance = guidance;

    std::string ti = "\"tool_interventions\":";
    auto tiPos = jsonBlock.find(ti);
    if (tiPos != std::string::npos) {
      auto arrStart = jsonBlock.find('[', tiPos);
      if (arrStart != std::string::npos) {
        int depth = 0;
        auto arrEnd = arrStart;
        for (; arrEnd < jsonBlock.size(); ++arrEnd) {
          if (jsonBlock[arrEnd] == '[') ++depth;
          else if (jsonBlock[arrEnd] == ']') { --depth; if (!depth) break; }
        }
        std::string arr = jsonBlock.substr(arrStart, arrEnd - arrStart + 1);

        int objDepth = 0;
        std::size_t objStart = std::string::npos;
        for (std::size_t i = 0; i < arr.size(); ++i) {
          if (arr[i] == '{') {
            if (objDepth == 0) objStart = i;
            ++objDepth;
          } else if (arr[i] == '}') {
            --objDepth;
            if (objDepth == 0 && objStart != std::string::npos) {
              std::string obj = arr.substr(objStart, i - objStart + 1);
              ValidationToolIntervention vti;
              vti.toolUseId = ExtractJsonStringField(obj, "tool_use_id");
              vti.action = ExtractJsonStringField(obj, "action");
              vti.correctedName = ExtractJsonStringField(obj, "corrected_name");
              vti.correctedInputJson = ExtractJsonStringField(obj, "corrected_input");
              vti.blockGuidance = ExtractJsonStringField(obj, "block_reason");
              if (!vti.toolUseId.empty() && !vti.action.empty())
                result.toolInterventions.push_back(vti);
              objStart = std::string::npos;
            }
          }
        }
      }
    }
  }

  std::string correctedBlock = ExtractXml(text, "corrected_text");
  if (!correctedBlock.empty() && result.correctedText.empty())
    result.correctedText = correctedBlock;

  return result;
}

void ApplyTextCorrection(const std::string& correctedText,
                         std::vector<Message>& assistantMessages) {
  if (correctedText.empty() || assistantMessages.empty()) return;
  for (auto& block : assistantMessages.back().content) {
    if (block.type == BlockType::Text) {
      block.asText.text = correctedText;
      return;
    }
  }
}

void ApplyToolInterventions(
    const std::vector<ValidationToolIntervention>& interventions,
    std::vector<ContentBlock>& toolUseBlocks,
    std::vector<Message>& messages) {
  for (const auto& ti : interventions) {
    for (auto& block : toolUseBlocks) {
      if (block.asToolUse.id != ti.toolUseId) continue;
      if (ti.action == "rewrite") {
        if (!ti.correctedName.empty())
          block.asToolUse.name = ti.correctedName;
        if (!ti.correctedInputJson.empty())
          block.asToolUse.inputJson = ti.correctedInputJson;
      } else if (ti.action == "block") {
        Message synthetic;
        synthetic.role = MessageRole::User;
        synthetic.content.push_back(ContentBlock::MakeToolResult(
            ti.toolUseId,
            "Tool call blocked by validation: " +
                (ti.blockGuidance.empty() ? "unsafe" : ti.blockGuidance),
            true));
        messages.push_back(synthetic);
      }
    }
  }
}

std::string BuildValidationContext(
    const std::vector<Message>& messages,
    const std::vector<Message>& assistantMessages,
    const std::vector<ContentBlock>& toolUseBlocks) {
  std::ostringstream ctx;
  ctx << "{\"user_goal\":\"";

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
  for (char c : goal)
    ctx << (c == '"' ? "\\\"" : c == '\n' ? "\\n" : std::string(1, c));

  ctx << "\",\"assistant_text\":\"";
  if (!assistantMessages.empty()) {
    std::string text;
    for (const auto& b : assistantMessages.back().content)
      if (b.type == BlockType::Text) { text = b.asText.text; break; }
    if (text.size() > 8000) text = text.substr(0, 8000);
    for (char c : text)
      ctx << (c == '"' ? "\\\"" : c == '\n' ? "\\n" : std::string(1, c));
  }

  ctx << "\",\"assistant_tool_calls\":[";
  bool first = true;
  for (const auto& tb : toolUseBlocks) {
    if (!first) ctx << ","; first = false;
    ctx << "{\"id\":\"" << tb.asToolUse.id << "\",\"name\":\""
        << tb.asToolUse.name << "\",\"input\":"
        << (tb.asToolUse.inputJson.empty() ? "{}" : tb.asToolUse.inputJson) << "}";
  }

  ctx << "],\"execution_evidence\":[";
  first = true;
  int evCount = 0;
  for (auto it = messages.rbegin(); it != messages.rend() && evCount < 5; ++it) {
    if (it->role != MessageRole::User) continue;
    for (const auto& b : it->content) {
      if (b.type != BlockType::ToolResult) continue;
      if (!first) ctx << ","; first = false;
      std::string c = b.asToolResult.content;
      if (c.size() > 500) c = c.substr(0, 500);
      ctx << "\"";
      for (char ch : c)
        ctx << (ch == '"' ? "\\\"" : ch == '\n' ? "\\n" : std::string(1, ch));
      ctx << "\"";
      ++evCount;
    }
  }
  ctx << "]}";
  return ctx.str();
}

std::string BuildValidatorSystemPrompt() {
  return "You are a validation model. Review the assistant output. "
         "Return only <validation_json>{...}</validation_json> with "
         "corrected_text, tool_interventions (rewrite/block), "
         "and final_response_action (approve/retry_from_tools).";
}

void PersistOversizedResult(const std::string& sessionDir,
                            const std::string& toolUseId,
                            const std::string& content) {
  if (sessionDir.empty()) return;
  std::string dir = sessionDir + "\\.tool-results";
  CreateDirectoryA(dir.c_str(), nullptr);
  std::string path = dir + "\\" + toolUseId + ".txt";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (out) out << content;
}

}  // namespace

QueryLoop::QueryLoop(tools::ToolOrchestrator& toolOrchestrator,
                     permissions::PermissionEngine& permissionEngine,
                     api::ModelClient& modelClient,
                     api::SideQueryClient& sideQueryClient)
    : toolOrchestrator_(toolOrchestrator),
      permissionEngine_(permissionEngine),
      modelClient_(modelClient),
      sideQueryClient_(sideQueryClient) {}

void QueryLoop::SetMaxTurns(int maxTurns) { maxTurns_ = maxTurns; }

int QueryLoop::EstimateTokens(const std::string& text) {
  return static_cast<int>(text.size()) / 4;
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

std::vector<Message> QueryLoop::DoHistorySnip(
    const std::vector<Message>& input) {
  std::vector<Message> result;
  result.reserve(input.size() + 1);

  Message snipBoundary;
  snipBoundary.role = MessageRole::System;
  snipBoundary.uuid = "snip-boundary";
  snipBoundary.isMeta = true;
  snipBoundary.content.push_back(
      ContentBlock::MakeText("<snip_boundary>Conversation trunked</snip_boundary>"));
  result.push_back(snipBoundary);

  std::size_t start = input.size() > 10 ? input.size() - 10 : 0;
  result.insert(result.end(), input.begin() + start, input.end());
  return result;
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

long long CurrentTimeMs() {
  return static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

void QueryLoop::ApplyStepBudget(QueryLoopContext& ctx,
                                QueryLoopInternalState& /*state*/) {
  auto& messages = ctx.messages;
  for (auto& msg : messages) {
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
                              QueryLoopInternalState& /*state*/) {
  if (ctx.messages.size() > 20)
    ctx.messages = DoHistorySnip(ctx.messages);
}

void QueryLoop::ApplyStepMicrocompact(QueryLoopContext& ctx,
                                      QueryLoopInternalState& /*state*/) {
  const long long now = CurrentTimeMs();

  for (auto& msg : ctx.messages)
    for (auto& block : msg.content) {
      if (block.type != BlockType::ToolResult) continue;
      if (static_cast<int>(block.asToolResult.content.size()) <=
          kMicroCompactOldMarkerBytes) continue;

      block.asToolResult.content = "[Old tool result content cleared]";
    }
}

void QueryLoop::ApplyStepCollapse(QueryLoopContext& ctx,
                                  QueryLoopInternalState& /*state*/) {
  const int estimatedTokens = EstimateMessageTokens(ctx.messages);
  const int threshold =
      kContextWindow - kMaxOutputTokensForSummary - kAutoCompactBufferTokens;
  if (estimatedTokens > threshold * 2) {
    ctx.messages = DoCollapseCompact(ctx.messages, 20);
  }
}

bool QueryLoop::ApplyStepAutocompact(QueryLoopContext& ctx,
                                     QueryLoopInternalState& state) {
  const int estimatedTokens = EstimateMessageTokens(ctx.messages);
  const int threshold =
      kContextWindow - kMaxOutputTokensForSummary - kAutoCompactBufferTokens;
  if (estimatedTokens <= threshold) return false;
  if (state.consecutiveAutoCompactFailures >= kAutoCompactMaxFailures)
    return false;

  std::vector<Message> compactInput;
  compactInput.push_back(ctx.messages.back());

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

  size_t keepCount = std::min<size_t>(3, ctx.messages.size());
  std::vector<Message> compacted;
  for (size_t i = 0; i < keepCount; ++i)
    compacted.push_back(ctx.messages[i]);
  compacted.push_back(summary);
  for (size_t i = keepCount; i < ctx.messages.size(); ++i)
    compacted.push_back(ctx.messages[i]);

  ctx.messages = compacted;
  state.consecutiveAutoCompactFailures = 0;
  ctx.autoCompactTracking.compacted = true;
  ctx.autoCompactTracking.turnCounter = 0;
  ctx.autoCompactTracking.consecutiveFailures = 0;
  return true;
}

bool QueryLoop::ApplyStepModelCall(QueryLoopContext& ctx,
                                   QueryLoopInternalState& state) {
  state.assistantMessages.clear();
  state.toolUseBlocks.clear();

  StreamingToolExecutor executor(
      const_cast<tools::ToolOrchestrator&>(toolOrchestrator_), ctx.messages);

  Message currentAssistant;
  currentAssistant.role = MessageRole::Assistant;
  currentAssistant.uuid = "stream-asst";

  std::ostringstream textBuffer;

  api::SseEventCallback callback =
      [&](const std::string& event, const std::string& data) {
    if (event == "text_delta") {
      textBuffer << data;
    } else if (event == "tool_use") {
      if (!textBuffer.str().empty()) {
        currentAssistant.content.push_back(
            ContentBlock::MakeText(textBuffer.str()));
        textBuffer.str(""); textBuffer.clear();
      }
      auto extractStr = [&](const char* key) -> std::string {
        std::string n = std::string("\"") + key + "\":\"";
        auto p = data.find(n); if (p == std::string::npos) return {};
        auto s = p + n.size(); auto e = data.find('"', s);
        if (e == std::string::npos) return {};
        return data.substr(s, e - s);
      };
      std::string toolId = extractStr("id");
      std::string toolName = extractStr("name");
      std::string inputJson = "{}";
      auto ip = data.find("\"input\":");
      if (ip != std::string::npos) {
        auto s = data.find('{', ip);
        if (s != std::string::npos) {
          int d = 0; auto e = s;
          for (; e < data.size(); ++e) {
            if (data[e] == '{') ++d;
            else if (data[e] == '}') { --d; if (!d) { ++e; break; } }
          }
          inputJson = data.substr(s, e - s);
        }
      }
      if (!toolId.empty()) {
        ContentBlock tb = ContentBlock::MakeToolUse(toolId, toolName, inputJson);
        currentAssistant.content.push_back(tb);
        state.toolUseBlocks.push_back(tb);
        executor.AddTool(tb);
        if (toolName == "FileRead" || toolName == "Grep" || toolName == "Glob") {
          executor.ExecutePending();
          for (auto& r : executor.YieldCompletedResults()) {
            if (r.type == BlockType::ToolResult) {
              Message em; em.role = MessageRole::User;
              em.content.push_back(r); ctx.messages.push_back(em);
            }
          }
        }
      }
    } else if (event == "stop_reason") {
      currentAssistant.stopReason = data;
    } else if (event == "api_error") {
      currentAssistant.isApiErrorMessage = true;
      currentAssistant.content.push_back(ContentBlock::MakeText(data));
    }
  };

  modelClient_.StreamResponse(ctx.messages, ctx.systemPrompt,
                              ctx.model, callback);

  if (!textBuffer.str().empty())
    currentAssistant.content.push_back(
        ContentBlock::MakeText(textBuffer.str()));

  if (!currentAssistant.content.empty()) {
    currentAssistant.uuid = "asst-" +
        std::to_string(ctx.messages.size() + state.turnCount);
    state.assistantMessages.push_back(currentAssistant);
  }

  if (!state.toolUseBlocks.empty()) {
    executor.ExecutePending();
    for (auto& r : executor.YieldCompletedResults()) {
      if (r.type == BlockType::ToolResult) {
        Message lm; lm.role = MessageRole::User;
        lm.content.push_back(r); ctx.messages.push_back(lm);
      }
    }
  }

  return !state.toolUseBlocks.empty();
}

void QueryLoop::ApplyStepValidator(QueryLoopContext& ctx,
                                   QueryLoopInternalState& state) {
  if (!IsValidationEnabled()) return;

  std::string contextJson = BuildValidationContext(
      ctx.messages, state.assistantMessages, state.toolUseBlocks);

  api::SideQueryRequest request;
  request.querySource = "validator";
  request.model = ctx.model;
  request.systemPrompt = BuildValidatorSystemPrompt();
  request.messages.clear();

  Message userMsg;
  userMsg.role = MessageRole::User;
  userMsg.content.push_back(ContentBlock::MakeText(contextJson));
  request.messages.push_back(userMsg);

  api::SideQueryResponse response = sideQueryClient_.Query(request);
  if (!response.ok) return;

  std::string fullResponse;
  for (const auto& msg : response.messages)
    for (const auto& block : msg.content)
      if (block.type == BlockType::Text)
        fullResponse += block.asText.text;

  ValidationResult vresult = ParseValidationResponse(fullResponse);

  if (!vresult.correctedText.empty())
    ApplyTextCorrection(vresult.correctedText, state.assistantMessages);

  if (!vresult.toolInterventions.empty())
    ApplyToolInterventions(vresult.toolInterventions, state.toolUseBlocks,
                           ctx.messages);

  if (vresult.finalResponseAction == "retry_from_tools") {
    Message guidance;
    guidance.role = MessageRole::System;
    guidance.uuid = "validator-retry";
    guidance.isMeta = true;
    guidance.content.push_back(ContentBlock::MakeText(
        "[Validator] " + (vresult.retryGuidance.empty()
            ? "Retry from tools." : vresult.retryGuidance)));
    ctx.messages.push_back(guidance);
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
    ctx.messages = DoReactiveCompact(ctx.messages);
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
      lastMsg.stopReason.find("max_tokens") != std::string::npos)
    isMaxTokens = true;
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
    ctx.messages.push_back(recovery);
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
  ctx.messages.push_back(nudge);
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
        ctx.messages.push_back(cont);
        return result;
      }
    }
  }
  return result;
}

bool QueryLoop::ApplyStepRunTools(QueryLoopContext& ctx,
                                  QueryLoopInternalState& state) {
  auto canUseTool = permissionEngine_.BuildCanUseTool();
  tools::ToolOrchestrator::ExecuteResult execResult =
      toolOrchestrator_.Execute(state.toolUseBlocks, canUseTool,
                                ctx.messages);
  for (const auto& msg : state.assistantMessages)
    ctx.messages.push_back(msg);
  for (const auto& rm : execResult.userMessages)
    ctx.messages.push_back(rm);
  state.assistantMessages.clear();
  state.toolUseBlocks.clear();
  if (execResult.errorCount > 0 &&
      state.maxOutputTokensRecoveryCount < kMaxOutputTokensRecoveryLimit) {
    ++state.maxOutputTokensRecoveryCount;
    return true;
  }
  return false;
}

bool QueryLoop::ApplyStepTerminate(QueryLoopContext& ctx,
                                   QueryLoopInternalState& state) {
  state.toolUseBlocks = CollectToolUseBlocks(state.assistantMessages);
  if (state.toolUseBlocks.empty()) {
    for (const auto& msg : state.assistantMessages)
      ctx.messages.push_back(msg);
    auto& at = ctx.autoCompactTracking;
    if (at.compacted) { at.compacted = false; at.turnId.clear(); }
    state.completed = true;
    state.terminalReason = "completed";
    return false;
  }
  return true;
}

void QueryLoop::RunFull(QueryLoopContext& ctx) {
  QueryLoopInternalState state;

  while (!state.completed) {
    switch (state.stage) {
      case QueryStage::ToolResultBudget: {
        ++state.turnCount;
        if (state.turnCount > maxTurns_) {
          state.completed = true;
          state.terminalReason = "max_turns";
          continue;
        }
        ApplyStepBudget(ctx, state);
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

        if (Handle413Recovery(ctx, state)) {
          state.stage = QueryStage::ModelCall; continue;
        }
        if (HandleMaxOutputTokens(ctx, state)) {
          state.stage = QueryStage::ModelCall; continue;
        }
        if (state.assistantMessages.empty()) {
          state.completed = true;
          state.terminalReason = "empty_response"; continue;
        }
        if (HandleTokenBudget(ctx, state)) {
          state.stage = QueryStage::ModelCall; continue;
        }
        const Message& lastMsg = state.assistantMessages.back();
        if (lastMsg.isApiErrorMessage &&
            !IsPromptTooLong(lastMsg) &&
            state.transition != TransitionReason::MaxOutputTokensRecovery &&
            state.transition != TransitionReason::MaxOutputTokensEscalate) {
          state.completed = true;
          state.terminalReason = "api_error"; continue;
        }
        if (hasTools) {
          state.stage = QueryStage::Validator;
        } else {
          state.stage = QueryStage::Completed;
          if (!ApplyStepTerminate(ctx, state)) continue;
          state.stage = QueryStage::ToolResultBudget;
          state.turnCount = 0;
        }
        continue;
      }
      case QueryStage::Validator: {
        ApplyStepValidator(ctx, state);
        state.stage = QueryStage::StopHooks;
        continue;
      }
      case QueryStage::StopHooks: {
        StopHookResult hooksResult = ExecuteStopHooks(ctx, state);
        if (hooksResult.preventContinuation) {
          state.completed = true;
          state.terminalReason = "stop_hook_prevented"; continue;
        }
        state.toolUseBlocks = CollectToolUseBlocks(state.assistantMessages);
        if (!hooksResult.blockingErrors.empty()) {
          for (const auto& am : state.assistantMessages)
            ctx.messages.push_back(am);
          for (const auto& err : hooksResult.blockingErrors)
            ctx.messages.push_back(err);
          state.assistantMessages.clear();
          state.toolUseBlocks.clear();
          state.transition = TransitionReason::StopHookBlocking;
          state.stage = QueryStage::ModelCall; continue;
        }
        state.stage = QueryStage::RunTools;
        continue;
      }
      case QueryStage::RunTools: {
        bool shouldContinue = ApplyStepRunTools(ctx, state);
        if (shouldContinue) {
          state.stage = QueryStage::ModelCall; continue;
        }
        state.stage = QueryStage::Completed;
        if (ApplyStepTerminate(ctx, state) &&
            state.maxOutputTokensRecoveryCount < kMaxOutputTokensRecoveryLimit) {
          state.stage = QueryStage::ModelCall;
          state.turnCount = 0;
        } else if (!state.completed) {
          state.stage = QueryStage::ToolResultBudget;
        }
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
  ctx.maxOutputTokensRecoveryCount = state.maxOutputTokensRecoveryCount;
  ctx.hasAttemptedReactiveCompact = state.hasAttemptedReactiveCompact;
}

}  // namespace core
}  // namespace agent
