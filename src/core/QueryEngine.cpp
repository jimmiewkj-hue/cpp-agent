#include "core/QueryEngine.h"

#include "api/ModelClient.h"
#include "api/SideQueryClient.h"
#include "agents/SubAgentManager.h"
#include "core/QueryLoop.h"
#include "infra/SessionManager.h"
#include "infra/StabilityWatchdog.h"
#include "memory/MemoryIndex.h"
#include "permissions/PermissionEngine.h"
#include "tools/ToolRegistry.h"
#include "tools/ToolOrchestrator.h"

#include <algorithm>
#include <sstream>

namespace agent {
namespace core {

bool ContentReplacementState::HasSeen(const std::string& toolUseId) const {
  for (const auto& id : seenIds) {
    if (id == toolUseId) return true;
  }
  return false;
}

std::string ContentReplacementState::GetReplacement(
    const std::string& toolUseId) const {
  for (std::size_t i = 0; i < seenIds.size() && i < replacementTexts.size();
       ++i) {
    if (seenIds[i] == toolUseId) return replacementTexts[i];
  }
  return std::string();
}

void ContentReplacementState::RecordReplacement(
    const std::string& toolUseId,
    const std::string& replacement) {
  seenIds.push_back(toolUseId);
  replacementTexts.push_back(replacement);
  lastSeenId = toolUseId;
}

QueryEngine::QueryEngine(tools::ToolOrchestrator& toolOrchestrator,
                         permissions::PermissionEngine& permissionEngine,
                         api::ModelClient& modelClient,
                         api::SideQueryClient& sideQueryClient,
                         tools::ToolRegistry& toolRegistry,
                         infra::SessionManager& sessionManager)
    : toolOrchestrator_(toolOrchestrator),
      permissionEngine_(permissionEngine),
      modelClient_(modelClient),
      sideQueryClient_(sideQueryClient),
      toolRegistry_(toolRegistry),
      sessionManager_(sessionManager) {
  config_ = AgentConfig::FromDefaults();
  systemPrompt_ = config_.systemPrompt;
  model_ = config_.defaultModel;
  metadata_ = sessionManager_.metadata();
  sessionManager_.SetMetadata(metadata_);
}

void QueryEngine::SetConfig(const AgentConfig& config) {
  config_ = config;
  systemPrompt_ = config_.systemPrompt;
  if (!config_.defaultModel.empty()) {
    model_ = config_.defaultModel;
  }
}

void QueryEngine::SetSystemPrompt(const std::string& systemPrompt) {
  systemPrompt_ = systemPrompt;
}

void QueryEngine::SetModel(const std::string& model) {
  model_ = model;
}

void QueryEngine::SetFallbackModel(const std::string& model) {
  fallbackModel_ = model;
}

void QueryEngine::SetValidatorModel(const std::string& model) {
  validatorModel_ = model;
}

void QueryEngine::SetMemoryIndex(memory::MemoryIndex* memoryIndex) {
  memoryIndex_ = memoryIndex;
  if (memoryIndex_ != nullptr) {
    memoryIndex_->SetSideQueryClient(&sideQueryClient_);
  }
}

void QueryEngine::SetSubAgentManager(agents::SubAgentManager* subAgentManager) {
  subAgentManager_ = subAgentManager;
  SyncSessionState();
  ConfigureWatchdogBindings();
}

void QueryEngine::SetStabilityWatchdog(infra::StabilityWatchdog* watchdog) {
  stabilityWatchdog_ = watchdog;
  ConfigureWatchdogBindings();
}

void QueryEngine::SetMaxTurns(int maxTurns) {
  maxTurns_ = maxTurns;
}

void QueryEngine::SetWallClockBudgetMs(long long budgetMs) {
  wallClockBudgetMs_ = budgetMs;
}

void QueryEngine::SetSessionDir(const std::string& sessionDir) {
  sessionDir_ = sessionDir;
  if (metadata_.id.empty() && !sessionDir.empty()) {
    metadata_.id = sessionDir;
    sessionManager_.SetMetadata(metadata_);
  }
}

void QueryEngine::SetEventCallback(QueryLoopEventCallback callback) {
  eventCallback_ = callback;
}

void QueryEngine::SetHookExecutor(hooks::HookExecutor* hookExecutor) {
  hookExecutor_ = hookExecutor;
}

void QueryEngine::SubmitUserPrompt(const std::string& prompt) {
  Message userMessage;
  userMessage.role = MessageRole::User;
  userMessage.uuid = "user-" + std::to_string(messages_.size());
  userMessage.content.push_back(ContentBlock::MakeText(prompt));
  messages_.push_back(userMessage);
  SyncSessionState();
}

void QueryEngine::SaveCheckpoint() {
  SyncSessionState();
  sessionManager_.PersistSnapshot();
}

void QueryEngine::RunTurn() {
  if (stabilityWatchdog_) {
    stabilityWatchdog_->Heartbeat();
  }

  toolOrchestrator_.SetToolRegistry(&toolRegistry_);
  SyncSessionState();

  loopCtx_.messages = messages_;
  loopCtx_.systemPrompt = BuildEffectiveSystemPrompt();
  loopCtx_.model = model_;
  loopCtx_.fallbackModel = fallbackModel_;
  loopCtx_.validatorModel = validatorModel_;
  loopCtx_.sessionDir = sessionDir_;
  loopCtx_.sessionManager = &sessionManager_;
  loopCtx_.eventCallback = eventCallback_;
  loopCtx_.hookExecutor = hookExecutor_;

  QueryLoop loop(toolOrchestrator_, permissionEngine_, modelClient_,
                 sideQueryClient_);
  loop.SetMaxTurns(maxTurns_);
  loop.SetWallClockBudget(wallClockBudgetMs_);
  loop.RunFull(loopCtx_);

  messages_ = loopCtx_.messages;
  SyncSessionState();

  sessionManager_.FlushTranscriptBuffer();

  ++metadata_.turnCount;
  sessionManager_.SetMetadata(metadata_);
  if (stabilityWatchdog_) {
    stabilityWatchdog_->SignalTurnComplete(true);
  }
}

bool QueryEngine::RunTurnWithRecovery() {
  try {
    SaveCheckpoint();
    RunTurn();
    return true;
  } catch (const std::exception&) {
    if (stabilityWatchdog_) {
      stabilityWatchdog_->SignalTurnComplete(false);
    }
    sessionManager_.PersistSnapshot();
    return false;
  }
}

bool QueryEngine::PrepareForContinuationAfterWallClockTimeout() {
  if (messages_.empty()) return false;
  const Message& last = messages_.back();
  if (last.role != MessageRole::System || !last.isMeta ||
      last.uuid != "wall-clock-timeout") {
    return false;
  }
  messages_.pop_back();
  SyncSessionState();
  return true;
}

bool QueryEngine::HandleFallback() {
  if (loopCtx_.fallbackModel.empty()) return false;
  if (loopCtx_.model == loopCtx_.fallbackModel) return false;

  Message warning;
  warning.role = MessageRole::System;
  warning.uuid = "fallback-warn";
  warning.isMeta = true;
  warning.content.push_back(ContentBlock::MakeText(
      "Fallback: switching from " + loopCtx_.model +
      " to " + loopCtx_.fallbackModel));
  messages_.push_back(warning);

  loopCtx_.model = loopCtx_.fallbackModel;
  return true;
}

const std::vector<Message>& QueryEngine::messages() const {
  return messages_;
}

const QueryLoopContext& QueryEngine::loopContext() const {
  return loopCtx_;
}

std::string QueryEngine::BuildEffectiveSystemPrompt() const {
  std::string effectivePrompt = systemPrompt_;
  if (effectivePrompt.empty()) {
    effectivePrompt = config_.systemPrompt;
  }

  std::string memoryPrompt;
  if (memoryIndex_ != nullptr) {
    std::vector<std::string> relevantTopics;
    const std::string latestUserQuery = BuildLatestUserQuery();
    if (!latestUserQuery.empty()) {
      const std::vector<memory::MemoryIndex::RelevantMemory> relevantMemories =
          memoryIndex_->FindRelevantMemories(latestUserQuery, {});
      for (const auto& memory : relevantMemories) {
        if (!memory.fileName.empty()) {
          relevantTopics.push_back(memory.fileName);
        }
      }
    }
    memoryPrompt = memoryIndex_->BuildSystemPromptInjection(relevantTopics);
  } else if (!config_.memoryRoot.empty()) {
    memory::MemoryIndex tempIndex(config_.memoryRoot);
    if (!tempIndex.ReadEntrypoint().empty()) {
      memoryPrompt = tempIndex.BuildSystemPromptInjection();
    }
  }

  if (memoryPrompt.empty()) return effectivePrompt;
  if (effectivePrompt.empty()) return memoryPrompt;
  return effectivePrompt + "\n\n" + memoryPrompt;
}

std::string QueryEngine::BuildLatestUserQuery() const {
  for (std::vector<Message>::const_reverse_iterator it = messages_.rbegin();
       it != messages_.rend(); ++it) {
    if (it->role != MessageRole::User) continue;
    std::ostringstream query;
    for (const auto& block : it->content) {
      if (block.type != BlockType::Text) continue;
      if (query.tellp() > 0) query << "\n";
      query << block.asText.text;
    }
    return query.str();
  }
  return std::string();
}

void QueryEngine::SyncSessionState() {
  sessionManager_.SetMessages(messages_);
  sessionManager_.SetMetadata(metadata_);
  if (subAgentManager_ != nullptr) {
    sessionManager_.SetSubAgentTasks(subAgentManager_->ListTasks());
    sessionManager_.SetSubAgentExecutors(subAgentManager_->ListExecutors());
  }
}

void QueryEngine::ConfigureWatchdogBindings() {
  if (stabilityWatchdog_ == nullptr) return;

  stabilityWatchdog_->SetSnapshotCallback([this]() {
    SyncSessionState();
    sessionManager_.PersistSnapshot();
  });
  stabilityWatchdog_->SetCrashRecoveryCallback([this]() {
    return RecoverFromSnapshot();
  });
  stabilityWatchdog_->SetTaskStateCallback([this]() {
    infra::TaskStateMetrics metrics;
    if (subAgentManager_ == nullptr) return metrics;
    const agents::SubAgentTaskSummary summary =
        subAgentManager_->SummarizeTasks();
    metrics.pending = summary.pending;
    metrics.running = summary.running;
    metrics.failed = summary.failed;
    return metrics;
  });
}

bool QueryEngine::RecoverFromSnapshot() {
  if (!sessionManager_.RestoreFromDisk()) return false;

  messages_ = sessionManager_.messages();
  metadata_ = sessionManager_.metadata();
  if (subAgentManager_ != nullptr) {
    subAgentManager_->RestoreTasksForRecovery(
        sessionManager_.subAgentTasks(),
        sessionManager_.subAgentExecutors());
    sessionManager_.SetSubAgentTasks(subAgentManager_->ListTasks());
    sessionManager_.SetSubAgentExecutors(subAgentManager_->ListExecutors());
  }
  return true;
}

}  // namespace core
}  // namespace agent
