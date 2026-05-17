#include "agents/SubAgentManager.h"

#include "agents/SubAgentWorkerProtocol.h"
#include "infra/ProcessRunner.h"
#include "memory/MemoryIndex.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

namespace agent {
namespace agents {

namespace {

const DWORD kWorkerPreemptExitCode = 3;
const DWORD kWorkerPollIntervalMs = 100;

long long NowUnixMs() {
  return static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

int ParseTaskSequence(const std::string& taskId) {
  const std::string prefix = "subtask-";
  if (taskId.find(prefix) != 0) {
    return 0;
  }
  return std::atoi(taskId.substr(prefix.size()).c_str());
}

bool IsTerminalState(SubAgentTaskState state) {
  return state == SubAgentTaskState::Completed ||
         state == SubAgentTaskState::Failed ||
         state == SubAgentTaskState::Cancelled;
}

std::string JoinPath(const std::string& lhs, const std::string& rhs) {
  if (lhs.empty()) {
    return rhs;
  }
  if (lhs.back() == '\\' || lhs.back() == '/') {
    return lhs + rhs;
  }
  return lhs + "\\" + rhs;
}

std::wstring ToWide(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int size = MultiByteToWideChar(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &wide[0], size);
  return wide;
}

std::wstring QuoteWindowsArg(const std::string& value) {
  const std::wstring wide = ToWide(value);
  if (wide.find_first_of(L" \t\"") == std::wstring::npos) {
    return wide;
  }
  std::wstring quoted;
  quoted.push_back(L'"');
  for (wchar_t ch : wide) {
    if (ch == L'"') {
      quoted.append(L"\\\"");
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back(L'"');
  return quoted;
}

std::string CurrentModuleDirectory() {
  wchar_t buffer[MAX_PATH] = {0};
  const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return std::string();
  }
  std::wstring path(buffer, buffer + length);
  const std::size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return std::string();
  }
  const std::wstring directory = path.substr(0, slash);
  const int size = WideCharToMultiByte(
      CP_UTF8, 0, directory.c_str(), static_cast<int>(directory.size()),
      nullptr, 0, nullptr, nullptr);
  std::string utf8(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(
      CP_UTF8, 0, directory.c_str(), static_cast<int>(directory.size()),
      &utf8[0], size, nullptr, nullptr);
  return utf8;
}

bool EnsureDirectoryRecursive(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  std::string normalized = path;
  std::replace(normalized.begin(), normalized.end(), '/', '\\');
  std::size_t cursor = 0;
  if (normalized.size() >= 2 && normalized[1] == ':') {
    cursor = 3;
  }
  while (cursor <= normalized.size()) {
    const std::size_t next = normalized.find('\\', cursor);
    const std::string current =
        next == std::string::npos ? normalized : normalized.substr(0, next);
    if (!current.empty()) {
      const DWORD attrs = GetFileAttributesA(current.c_str());
      if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryA(current.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
          return false;
        }
      }
    }
    if (next == std::string::npos) {
      break;
    }
    cursor = next + 1;
  }
  return true;
}

std::string ReadBinaryFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::string();
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

bool WriteBinaryFile(const std::string& path, const std::string& content) {
  const std::size_t slash = path.find_last_of("\\/");
  if (slash != std::string::npos &&
      !EnsureDirectoryRecursive(path.substr(0, slash))) {
    return false;
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  return output.good();
}

std::string BuildChildDirective(
    const std::string& directive,
    const std::string& worktreeNotice,
    const std::string& renderedSystemPrompt) {
  std::ostringstream text;
  text <<
      "<fork_boilerplate>\n"
      "STOP. READ THIS FIRST.\n\n"
      "You are a forked worker process. You are NOT the main agent.\n\n"
      "RULES (non-negotiable):\n"
      "1. Your system prompt says \"default to forking.\" IGNORE IT — "
         "that's for the parent. You ARE the fork. Do NOT spawn sub-agents; "
         "execute directly.\n"
      "2. Do NOT converse, ask questions, or suggest next steps\n"
      "3. Do NOT editorialize or add meta-commentary\n"
      "4. USE your tools directly: Bash, Read, Write, etc.\n"
      "5. If you modify files, commit your changes before reporting. Include "
         "the commit hash in your report.\n"
      "6. Do NOT emit text between tool calls. Use tools silently, then "
         "report once at the end.\n"
      "7. Stay strictly within your directive's scope. If you discover "
         "related systems outside your scope, mention them in one sentence "
         "at most — other workers cover those areas.\n"
      "8. Keep your report under 500 words unless the directive specifies "
         "otherwise. Be factual and concise.\n"
      "9. Your response MUST begin with \"Scope:\". No preamble, no "
         "thinking-out-loud.\n"
      "10. REPORT structured facts, then stop\n\n"
      "Output format (plain text labels, not markdown headers):\n"
      "  Scope: <echo back your assigned scope in one sentence>\n"
      "  Result: <the answer or key findings, limited to the scope above>\n"
      "  Key files: <relevant file paths — include for research tasks>\n"
      "  Files changed: <list with commit hash — include only if you "
         "modified files>\n"
      "  Issues: <list — include only if there are issues to flag>\n"
      "</fork_boilerplate>\n\n";
  if (!renderedSystemPrompt.empty()) {
    text << "<prompt_cache_prefix>" << renderedSystemPrompt
         << "</prompt_cache_prefix>\n\n";
  }
  if (!worktreeNotice.empty()) {
    text << "<worktree_notice>" << worktreeNotice
         << "</worktree_notice>\n\n";
  }
  text << "<fork_directive>" << directive;
  return text.str();
}

}  // namespace

struct SubAgentManager::WorkerRuntime {
  HANDLE processHandle = nullptr;
  HANDLE threadHandle = nullptr;
  std::string executorId;
  std::string runtimeDir;
  std::string requestPath;
  std::string statusPath;
  std::string controlPath;
};

SubAgentManager::SubAgentManager() {
  monitorThread_ = std::thread(&SubAgentManager::MonitorLoop, this);
}

SubAgentManager::~SubAgentManager() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopMonitor_ = true;
    for (auto& entry : workers_) {
      if (entry.second.processHandle != nullptr) {
        TerminateProcess(entry.second.processHandle, 1);
      }
    }
  }
  monitorCv_.notify_all();
  if (monitorThread_.joinable()) {
    monitorThread_.join();
  }
  for (auto& entry : workers_) {
    if (entry.second.threadHandle != nullptr) {
      CloseHandle(entry.second.threadHandle);
    }
    if (entry.second.processHandle != nullptr) {
      CloseHandle(entry.second.processHandle);
    }
  }
}

std::vector<core::Message> SubAgentManager::BuildForkedMessages(
    const std::string& directive,
    const core::Message& assistantMessage,
    const std::string& parentCwd,
    const std::string& worktreeCwd,
    const std::string& renderedSystemPrompt) const {
  core::Message fullAssistantMessage = assistantMessage;
  fullAssistantMessage.uuid = "forked-assistant";
  const std::string worktreeNotice = BuildWorktreeNotice(parentCwd, worktreeCwd);

  std::vector<core::ContentBlock> toolResultBlocks;
  for (const auto& block : assistantMessage.content) {
    if (block.type == core::BlockType::ToolUse) {
      toolResultBlocks.push_back(core::ContentBlock::MakeToolResult(
          block.asToolUse.id, kForkPlaceholderResult, false));
    }
  }

  if (toolResultBlocks.empty()) {
    core::Message childDirectiveOnly;
    childDirectiveOnly.role = core::MessageRole::User;
    childDirectiveOnly.content.push_back(core::ContentBlock::MakeText(
        BuildChildDirective(directive, worktreeNotice, renderedSystemPrompt)));
    return {childDirectiveOnly};
  }

  core::Message toolResultMessage;
  toolResultMessage.role = core::MessageRole::User;
  toolResultMessage.uuid = "forked-tool-results";
  toolResultMessage.content = toolResultBlocks;
  toolResultMessage.content.push_back(core::ContentBlock::MakeText(
      BuildChildDirective(directive, worktreeNotice, renderedSystemPrompt)));

  return {fullAssistantMessage, toolResultMessage};
}

std::vector<std::string> SubAgentManager::ApplyExactToolWhitelist(
    const std::vector<std::string>& parentTools,
    const std::vector<std::string>& allowedTools) const {
  if (allowedTools.empty()) {
    return parentTools;
  }

  std::vector<std::string> exactTools;
  for (const auto& tool : parentTools) {
    if (std::find(allowedTools.begin(), allowedTools.end(), tool) !=
        allowedTools.end()) {
      exactTools.push_back(tool);
    }
  }
  return exactTools;
}

std::string SubAgentManager::BuildWorktreeNotice(
    const std::string& parentCwd,
    const std::string& worktreeCwd) const {
  if (parentCwd.empty() || worktreeCwd.empty()) {
    return std::string();
  }

  std::ostringstream notice;
  notice << "You inherited context from a parent agent working in "
         << parentCwd << ". You are running in an isolated worktree at "
         << worktreeCwd << ". Translate inherited paths to the worktree root, "
         << "re-read files before editing, and assume your changes stay "
         << "isolated from the parent working copy.";
  return notice.str();
}

std::string SubAgentManager::StartSubTask(const SubAgentTask& task) {
  std::lock_guard<std::mutex> lock(mutex_);
  EnsureRuntimeDefaultsLocked();

  SubAgentTaskLifecycle lifecycle;
  lifecycle.taskId = "subtask-" + std::to_string(nextTaskId_++);
  lifecycle.task = task;
  lifecycle.directive = task.prompt;
  lifecycle.worktreeNotice = BuildWorktreeNotice(task.parentCwd, task.worktreeCwd);
  lifecycle.placeholderResult = kForkPlaceholderResult;
  lifecycle.state = SubAgentTaskState::Pending;
  lifecycle.createdAtUnixMs = NowUnixMs();
  lifecycle.updatedAtUnixMs = lifecycle.createdAtUnixMs;
  tasks_.push_back(lifecycle);
  SchedulePendingTasksLocked();
  monitorCv_.notify_all();
  return lifecycle.taskId;
}

bool SubAgentManager::UpdateTaskState(
    const std::string& taskId,
    SubAgentTaskState state,
    const std::string& summary) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& task : tasks_) {
    if (task.taskId != taskId) {
      continue;
    }
    if (state != SubAgentTaskState::Running) {
      RequestWorkerStopLocked(taskId, state == SubAgentTaskState::Pending, summary);
    }
    task.state = state;
    task.updatedAtUnixMs = NowUnixMs();
    if (!summary.empty()) {
      task.summary = summary;
      if (state == SubAgentTaskState::Failed) {
        task.lastFailureReason = summary;
      }
    }
    if (IsTerminalState(state)) {
      task.assignedExecutorId.clear();
    }
    NormalizeExecutorLoadLocked();
    if (state == SubAgentTaskState::Pending) {
      SchedulePendingTasksLocked();
    }
    monitorCv_.notify_all();
    return true;
  }
  return false;
}

bool SubAgentManager::TryGetTask(
    const std::string& taskId,
    SubAgentTaskLifecycle* lifecycle) const {
  if (lifecycle == nullptr) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& task : tasks_) {
    if (task.taskId == taskId) {
      *lifecycle = task;
      return true;
    }
  }
  return false;
}

std::vector<SubAgentTaskLifecycle> SubAgentManager::ListTasks() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tasks_;
}

void SubAgentManager::SetExecutors(
    const std::vector<SubAgentExecutorSlot>& executors) {
  std::lock_guard<std::mutex> lock(mutex_);
  executors_ = executors;
  RebuildWorkerAssignmentsLocked();
  NormalizeExecutorLoadLocked();
  SchedulePendingTasksLocked();
  monitorCv_.notify_all();
}

void SubAgentManager::UpsertExecutor(const SubAgentExecutorSlot& executor) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& existing : executors_) {
    if (existing.executorId == executor.executorId) {
      existing = executor;
      RebuildWorkerAssignmentsLocked();
      NormalizeExecutorLoadLocked();
      SchedulePendingTasksLocked();
      monitorCv_.notify_all();
      return;
    }
  }
  executors_.push_back(executor);
  RebuildWorkerAssignmentsLocked();
  NormalizeExecutorLoadLocked();
  SchedulePendingTasksLocked();
  monitorCv_.notify_all();
}

std::vector<SubAgentExecutorSlot> SubAgentManager::ListExecutors() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return executors_;
}

void SubAgentManager::SetWorkerExecutablePath(
    const std::string& workerExecutablePath) {
  std::lock_guard<std::mutex> lock(mutex_);
  workerExecutablePath_ = workerExecutablePath;
}

void SubAgentManager::SetWorkerRuntimeRoot(const std::string& runtimeRoot) {
  std::lock_guard<std::mutex> lock(mutex_);
  runtimeRoot_ = runtimeRoot;
  EnsureRuntimeDefaultsLocked();
}

bool SubAgentManager::SaveCheckpoint(
    const std::string& taskId,
    const SubAgentTaskCheckpoint& checkpoint) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& task : tasks_) {
    if (task.taskId == taskId) {
      task.checkpoint = checkpoint;
      task.updatedAtUnixMs = NowUnixMs();
      return true;
    }
  }
  return false;
}

bool SubAgentManager::RecordExecutorFailure(
    const std::string& executorId,
    const std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  bool found = false;
  const long long now = NowUnixMs();
  for (auto& executor : executors_) {
    if (executor.executorId == executorId) {
      executor.healthy = false;
      executor.state = SubAgentExecutorState::Recovering;
      executor.lastHeartbeatUnixMs = now;
      executor.lastError = error;
      found = true;
      break;
    }
  }
  if (!found) {
    return false;
  }

  for (auto& task : tasks_) {
    if (task.assignedExecutorId != executorId ||
        task.state != SubAgentTaskState::Running) {
      continue;
    }
    RequestWorkerStopLocked(task.taskId, false, error);
    task.state = SubAgentTaskState::Pending;
    task.assignedExecutorId.clear();
    task.lastFailureReason = error;
    task.updatedAtUnixMs = now;
    ++task.attemptCount;
    if (task.checkpoint.resumable) {
      task.summary = "Executor failure detected; resumable checkpoint queued for reassignment.";
    } else {
      task.summary = "Executor failure detected; task queued for clean replay.";
    }
  }
  NormalizeExecutorLoadLocked();
  SchedulePendingTasksLocked();
  monitorCv_.notify_all();
  return true;
}

void SubAgentManager::RestoreTasksForRecovery(
    const std::vector<SubAgentTaskLifecycle>& tasks,
    const std::vector<SubAgentExecutorSlot>& executors) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!executors.empty()) {
    executors_ = executors;
  }
  RebuildWorkerAssignmentsLocked();
  tasks_ = tasks;
  int maxSequence = 0;
  const long long now = NowUnixMs();
  for (auto& task : tasks_) {
    maxSequence = std::max(maxSequence, ParseTaskSequence(task.taskId));
    if (task.state == SubAgentTaskState::Running ||
        !task.assignedExecutorId.empty()) {
      task.state = SubAgentTaskState::Pending;
      task.updatedAtUnixMs = now;
      task.assignedExecutorId.clear();
      ++task.attemptCount;
      if (task.checkpoint.resumable) {
        task.summary =
            "Recovered from persisted snapshot; resumable checkpoint queued for executor reschedule.";
      } else if (task.summary.empty()) {
        task.summary =
            "Recovered from persisted snapshot; awaiting executor reschedule.";
      }
    }
  }
  nextTaskId_ = std::max(1, maxSequence + 1);
  NormalizeExecutorLoadLocked();
  SchedulePendingTasksLocked();
  monitorCv_.notify_all();
}

SubAgentTaskSummary SubAgentManager::SummarizeTasks() const {
  std::lock_guard<std::mutex> lock(mutex_);

  SubAgentTaskSummary summary;
  for (const auto& task : tasks_) {
    switch (task.state) {
      case SubAgentTaskState::Pending:
        ++summary.pending;
        break;
      case SubAgentTaskState::Running:
        ++summary.running;
        break;
      case SubAgentTaskState::Completed:
        ++summary.completed;
        break;
      case SubAgentTaskState::Failed:
        ++summary.failed;
        break;
      case SubAgentTaskState::Cancelled:
        ++summary.cancelled;
        break;
    }
  }
  return summary;
}

bool SubAgentManager::IsForkCandidate(
    const core::Message& assistantMessage) const {
  return assistantMessage.hasToolUse();
}

bool SubAgentManager::IsInForkChild(
    const std::vector<core::Message>& messages) const {
  for (const auto& message : messages) {
    if (message.role != core::MessageRole::User) {
      continue;
    }
    for (const auto& block : message.content) {
      if (block.type == core::BlockType::Text &&
          block.asText.text.find("<fork_boilerplate>") != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}

void SubAgentManager::EnsureRuntimeDefaultsLocked() {
  if (workerExecutablePath_.empty()) {
    const std::string moduleDir = CurrentModuleDirectory();
    workerExecutablePath_ = JoinPath(moduleDir, "agent_subagent_worker.exe");
  }
  if (runtimeRoot_.empty()) {
    const std::string moduleDir = CurrentModuleDirectory();
    runtimeRoot_ = JoinPath(moduleDir, "subagent-runtime");
  }
  EnsureDirectoryRecursive(runtimeRoot_);
}

void SubAgentManager::RebuildWorkerAssignmentsLocked() {
  std::vector<std::string> staleTaskIds;
  for (const auto& entry : workers_) {
    bool keep = false;
    for (const auto& task : tasks_) {
      if (task.taskId == entry.first && task.state == SubAgentTaskState::Running) {
        keep = true;
        break;
      }
    }
    if (!keep) {
      staleTaskIds.push_back(entry.first);
    }
  }
  for (const auto& taskId : staleTaskIds) {
    RequestWorkerStopLocked(taskId, false, "worker assignment reset");
  }
}

void SubAgentManager::MonitorLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopMonitor_) {
    RefreshWorkerStatesLocked();
    monitorCv_.wait_for(lock, std::chrono::milliseconds(kWorkerPollIntervalMs));
  }
}

void SubAgentManager::RefreshWorkerStatesLocked() {
  bool scheduleNeeded = false;
  for (auto it = workers_.begin(); it != workers_.end();) {
    SubAgentWorkerStatus status;
    const bool hasStatus =
        DeserializeWorkerStatus(ReadBinaryFile(it->second.statusPath), &status);
    for (auto& task : tasks_) {
      if (task.taskId != it->first) {
        continue;
      }
      if (hasStatus) {
        task.checkpoint.checkpointId = status.checkpointId;
        task.checkpoint.resumeCursor = status.resumeCursor;
        task.checkpoint.savedAtUnixMs = status.updatedAtUnixMs;
        task.checkpoint.resumable = true;
        if (!status.summary.empty() && task.state == SubAgentTaskState::Running) {
          task.summary = status.summary;
        }
      }
      break;
    }

    if (WaitForSingleObject(it->second.processHandle, 0) != WAIT_OBJECT_0) {
      ++it;
      continue;
    }

    DWORD exitCode = 0;
    GetExitCodeProcess(it->second.processHandle, &exitCode);
    for (auto& task : tasks_) {
      if (task.taskId != it->first) {
        continue;
      }
      task.assignedExecutorId.clear();
      task.updatedAtUnixMs = NowUnixMs();
      if (hasStatus) {
        task.checkpoint.checkpointId = status.checkpointId;
        task.checkpoint.resumeCursor = status.resumeCursor;
        task.checkpoint.savedAtUnixMs = status.updatedAtUnixMs;
        task.checkpoint.resumable = true;
      }
      if (hasStatus && status.state == WorkerTaskState::Completed &&
          exitCode == 0) {
        task.state = SubAgentTaskState::Completed;
        task.summary = status.summary;
      } else if ((hasStatus && status.state == WorkerTaskState::Preempted) ||
                 exitCode == kWorkerPreemptExitCode) {
        task.state = SubAgentTaskState::Pending;
        if (task.summary.empty()) {
          task.summary = "Worker preempted; checkpoint queued for reassignment.";
        }
      } else if (!IsTerminalState(task.state)) {
        task.state = SubAgentTaskState::Failed;
        task.lastFailureReason =
            hasStatus && !status.summary.empty()
                ? status.summary
                : "worker process exited unexpectedly";
        task.summary = task.lastFailureReason;
      }
      scheduleNeeded = true;
      break;
    }

    if (it->second.threadHandle != nullptr) {
      CloseHandle(it->second.threadHandle);
    }
    if (it->second.processHandle != nullptr) {
      CloseHandle(it->second.processHandle);
    }
    it = workers_.erase(it);
  }

  if (scheduleNeeded) {
    NormalizeExecutorLoadLocked();
    SchedulePendingTasksLocked();
  }
}

void SubAgentManager::SpawnWorkerForTaskLocked(
    std::size_t taskIndex,
    std::size_t executorIndex) {
  if (taskIndex >= tasks_.size() || executorIndex >= executors_.size()) {
    return;
  }

  EnsureRuntimeDefaultsLocked();
  SubAgentTaskLifecycle& task = tasks_[taskIndex];
  SubAgentExecutorSlot& executor = executors_[executorIndex];
  const std::string runtimeDir = JoinPath(runtimeRoot_, task.taskId);
  if (!EnsureDirectoryRecursive(runtimeDir)) {
    task.state = SubAgentTaskState::Failed;
    task.lastFailureReason = "failed to create worker runtime directory";
    task.summary = task.lastFailureReason;
    task.assignedExecutorId.clear();
    return;
  }

  WorkerRuntime runtime;
  runtime.executorId = executor.executorId;
  runtime.runtimeDir = runtimeDir;
  runtime.requestPath = JoinPath(runtimeDir, "request.pb");
  runtime.statusPath = JoinPath(runtimeDir, "status.pb");
  runtime.controlPath = JoinPath(runtimeDir, "preempt.flag");
  DeleteFileA(runtime.controlPath.c_str());

  SubAgentWorkerRequest request;
  request.taskId = task.taskId;
  request.executorId = executor.executorId;
  request.prompt = task.task.prompt;
  request.priority = task.task.priority;
  request.checkpointId = task.checkpoint.checkpointId;
  request.resumeCursor = task.checkpoint.resumeCursor;
  if (!WriteBinaryFile(runtime.requestPath, SerializeWorkerRequest(request))) {
    task.state = SubAgentTaskState::Failed;
    task.lastFailureReason = "failed to persist worker request payload";
    task.summary = task.lastFailureReason;
    task.assignedExecutorId.clear();
    return;
  }

  std::wstring commandLine = QuoteWindowsArg(workerExecutablePath_) +
                             L" --request " +
                             QuoteWindowsArg(runtime.requestPath) +
                             L" --status " +
                             QuoteWindowsArg(runtime.statusPath) +
                             L" --control " +
                             QuoteWindowsArg(runtime.controlPath);
  std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
  mutableCommand.push_back(L'\0');

  STARTUPINFOW startupInfo;
  ZeroMemory(&startupInfo, sizeof(startupInfo));
  startupInfo.cb = sizeof(startupInfo);

  PROCESS_INFORMATION processInfo;
  ZeroMemory(&processInfo, sizeof(processInfo));

  if (!CreateProcessW(
          nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
          CREATE_NO_WINDOW, nullptr, ToWide(runtimeDir).c_str(),
          &startupInfo, &processInfo)) {
    task.state = SubAgentTaskState::Failed;
    task.lastFailureReason = "failed to spawn worker process";
    task.summary = task.lastFailureReason;
    task.assignedExecutorId.clear();
    return;
  }

  runtime.processHandle = processInfo.hProcess;
  runtime.threadHandle = processInfo.hThread;
  workers_[task.taskId] = runtime;
}

void SubAgentManager::RequestWorkerStopLocked(
    const std::string& taskId,
    bool preemptive,
    const std::string& reason) {
  auto runtimeIt = workers_.find(taskId);
  if (runtimeIt == workers_.end()) {
    return;
  }
  if (preemptive) {
    WriteBinaryFile(runtimeIt->second.controlPath, "preempt");
  }
  if (runtimeIt->second.processHandle != nullptr) {
    TerminateProcess(
        runtimeIt->second.processHandle,
        preemptive ? kWorkerPreemptExitCode : 1);
    WaitForSingleObject(runtimeIt->second.processHandle, 250);
  }

  SubAgentWorkerStatus status;
  const bool hasStatus =
      DeserializeWorkerStatus(ReadBinaryFile(runtimeIt->second.statusPath), &status);
  for (auto& task : tasks_) {
    if (task.taskId != taskId) {
      continue;
    }
    task.updatedAtUnixMs = NowUnixMs();
    if (hasStatus) {
      task.checkpoint.checkpointId = status.checkpointId;
      task.checkpoint.resumeCursor = status.resumeCursor;
      task.checkpoint.savedAtUnixMs = status.updatedAtUnixMs;
      task.checkpoint.resumable = true;
    }
    if (preemptive) {
      task.summary = reason.empty()
                         ? "Preempted for higher-priority reassignment."
                         : reason;
    }
    break;
  }

  if (runtimeIt->second.threadHandle != nullptr) {
    CloseHandle(runtimeIt->second.threadHandle);
  }
  if (runtimeIt->second.processHandle != nullptr) {
    CloseHandle(runtimeIt->second.processHandle);
  }
  workers_.erase(runtimeIt);
}

void SubAgentManager::NormalizeExecutorLoadLocked() {
  for (auto& executor : executors_) {
    executor.runningTasks = 0;
    if (!executor.healthy && executor.state != SubAgentExecutorState::Offline) {
      executor.state = SubAgentExecutorState::Recovering;
    } else if (executor.healthy && executor.state != SubAgentExecutorState::Offline) {
      executor.state = SubAgentExecutorState::Idle;
    }
  }

  for (const auto& task : tasks_) {
    if (task.state != SubAgentTaskState::Running ||
        task.assignedExecutorId.empty()) {
      continue;
    }
    for (auto& executor : executors_) {
      if (executor.executorId == task.assignedExecutorId) {
        ++executor.runningTasks;
        if (executor.state != SubAgentExecutorState::Offline) {
          executor.state = SubAgentExecutorState::Busy;
        }
        break;
      }
    }
  }
}

int SubAgentManager::FindBestExecutorLocked(
    const SubAgentTaskLifecycle& task) const {
  int bestIndex = -1;
  double bestScore = std::numeric_limits<double>::max();
  for (std::size_t i = 0; i < executors_.size(); ++i) {
    const auto& executor = executors_[i];
    if (!executor.healthy || executor.state == SubAgentExecutorState::Offline) {
      continue;
    }
    if (task.checkpoint.resumable && !executor.supportsCheckpointResume) {
      continue;
    }
    const int required =
        std::max(1, task.task.requiredExecutorSlots);
    if (executor.runningTasks + required > executor.maxParallelTasks) {
      continue;
    }
    const double weightedCapacity =
        static_cast<double>(std::max(1, executor.maxParallelTasks)) *
        std::max(1, executor.weight);
    const double score =
        static_cast<double>(executor.runningTasks) / weightedCapacity;
    if (score < bestScore) {
      bestScore = score;
      bestIndex = static_cast<int>(i);
    }
  }
  return bestIndex;
}

int SubAgentManager::FindPreemptibleTaskLocked(
    const SubAgentTaskLifecycle& task) const {
  int bestIndex = -1;
  for (std::size_t i = 0; i < tasks_.size(); ++i) {
    const auto& candidate = tasks_[i];
    if (candidate.state != SubAgentTaskState::Running ||
        candidate.assignedExecutorId.empty()) {
      continue;
    }
    if (candidate.task.priority >= task.task.priority) {
      continue;
    }
    if (bestIndex < 0 ||
        tasks_[static_cast<std::size_t>(bestIndex)].task.priority >
            candidate.task.priority) {
      bestIndex = static_cast<int>(i);
    }
  }
  return bestIndex;
}

void SubAgentManager::SchedulePendingTasksLocked() {
  NormalizeExecutorLoadLocked();

  std::vector<std::size_t> pendingIndexes;
  for (std::size_t i = 0; i < tasks_.size(); ++i) {
    if (tasks_[i].state == SubAgentTaskState::Pending) {
      pendingIndexes.push_back(i);
    }
  }

  std::sort(pendingIndexes.begin(), pendingIndexes.end(),
            [this](std::size_t lhs, std::size_t rhs) {
              const auto& left = tasks_[lhs];
              const auto& right = tasks_[rhs];
              if (left.task.priority != right.task.priority) {
                return left.task.priority > right.task.priority;
              }
              return left.createdAtUnixMs < right.createdAtUnixMs;
            });

  const long long now = NowUnixMs();
  for (std::size_t index : pendingIndexes) {
    SubAgentTaskLifecycle& task = tasks_[index];
    const int executorIndex = FindBestExecutorLocked(task);
    int resolvedExecutorIndex = executorIndex;
    if (resolvedExecutorIndex < 0) {
      const int preemptIndex = FindPreemptibleTaskLocked(task);
      if (preemptIndex >= 0) {
        SubAgentTaskLifecycle& preempted =
            tasks_[static_cast<std::size_t>(preemptIndex)];
        const std::string preemptReason =
            "Preempted by higher-priority task " + task.taskId + ".";
        RequestWorkerStopLocked(preempted.taskId, true, preemptReason);
        preempted.state = SubAgentTaskState::Pending;
        preempted.assignedExecutorId.clear();
        preempted.updatedAtUnixMs = now;
        preempted.lastFailureReason = preemptReason;
        if (preempted.checkpoint.resumable) {
          preempted.summary =
              "Preempted with checkpoint preserved for later reassignment.";
        } else {
          preempted.summary =
              "Preempted; task will be replayed when capacity returns.";
        }
        NormalizeExecutorLoadLocked();
        resolvedExecutorIndex = FindBestExecutorLocked(task);
      }
    }
    if (resolvedExecutorIndex < 0) {
      continue;
    }

    SubAgentExecutorSlot& executor =
        executors_[static_cast<std::size_t>(resolvedExecutorIndex)];
    task.assignedExecutorId = executor.executorId;
    task.state = SubAgentTaskState::Running;
    task.updatedAtUnixMs = now;
    ++task.attemptCount;
    if (task.checkpoint.resumable && !task.checkpoint.checkpointId.empty()) {
      task.summary = "Assigned to executor " + executor.executorId +
                     " with checkpoint resume token " +
                     task.checkpoint.checkpointId + ".";
    } else if (task.summary.empty()) {
      task.summary = "Assigned to executor " + executor.executorId + ".";
    }
    executor.runningTasks += std::max(1, task.task.requiredExecutorSlots);
    executor.state = SubAgentExecutorState::Busy;
    executor.lastHeartbeatUnixMs = now;
    SpawnWorkerForTaskLocked(index, static_cast<std::size_t>(resolvedExecutorIndex));
  }
}

bool SubAgentManager::CreateWorktree(const std::string& taskId,
                                     const std::string& baseBranch) {
  std::string worktreePath = GetWorktreePath(taskId);

  infra::ProcessRunOptions opts;
  opts.executable = "git";
  opts.arguments = {"worktree", "add", worktreePath, baseBranch};
  opts.timeoutMs = 30000;
  infra::ProcessRunResult r = infra::ProcessRunner().Run(opts);
  if (r.exitCode != 0) {
    EnsureDirectoryRecursive(worktreePath);
  }
  return true;
}

bool SubAgentManager::HasWorktreeChanges(const std::string& worktreePath) const {
  infra::ProcessRunOptions opts;
  opts.executable = "git";
  opts.arguments = {"-C", worktreePath, "status", "--porcelain"};
  opts.timeoutMs = 15000;
  infra::ProcessRunResult r = infra::ProcessRunner().Run(opts);
  return !r.stdoutText.empty();
}

bool SubAgentManager::RemoveWorktree(const std::string& worktreePath,
                                     bool force) {
  infra::ProcessRunOptions opts;
  opts.executable = "git";
  opts.arguments = {"worktree", "remove", worktreePath};
  if (force) opts.arguments.push_back("--force");
  opts.timeoutMs = 30000;
  infra::ProcessRunResult r = infra::ProcessRunner().Run(opts);
  return r.exitCode == 0;
}

std::string SubAgentManager::GetWorktreePath(const std::string& taskId) const {
  return JoinPath(runtimeRoot_, "worktree-" + taskId);
}

bool SubAgentManager::SendWorkerMessage(const std::string& taskId,
                                  const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& task : tasks_) {
    if (task.taskId != taskId) continue;
    task.directive = message;
    task.state = SubAgentTaskState::Pending;
    task.updatedAtUnixMs = NowUnixMs();
    task.assignedExecutorId.clear();
    SchedulePendingTasksLocked();
    monitorCv_.notify_all();
    return true;
  }
  return false;
}

bool SubAgentManager::StopTask(const std::string& taskId) {
  return UpdateTaskState(taskId, SubAgentTaskState::Cancelled, "stopped by user");
}

void SubAgentManager::SetMemoryIndex(memory::MemoryIndex* /*memoryIndex*/) {
}

void SubAgentManager::ExecuteAutoDream() {
}

std::string SubAgentManager::Cwd() const {
  char buf[MAX_PATH] = {0};
  GetCurrentDirectoryA(MAX_PATH, buf);
  return std::string(buf);
}

std::string SubAgentManager::RunAsyncAgentLifecycle(
    const std::string& prompt,
    const std::string& description,
    const std::string& subagentType,
    bool runInBackground,
    const std::string& isolation,
    const std::vector<std::string>& allowedTools) {
  SubAgentTask task;
  task.prompt = prompt;
  task.description = description;
  task.subagentType = subagentType;
  task.runInBackground = runInBackground;
  task.isolation = isolation;
  task.priority = runInBackground ? 10 : 50;

  if (!allowedTools.empty()) {
    task.subagentType = "restricted";
  }

  std::string taskId = StartSubTask(task);
  if (taskId.empty()) return "";

  if (runInBackground) {
    SubAgentTaskCheckpoint ckpt;
    ckpt.checkpointId = taskId + "-init";
    ckpt.resumeCursor = prompt.substr(0, 200);
    ckpt.savedAtUnixMs = NowUnixMs();
    SaveCheckpoint(taskId, ckpt);
  }

  if (!isolation.empty() && isolation != "none") {
    CreateWorktree(taskId);
  }

  return kForkPlaceholderResult;
}

}  // namespace agents
}  // namespace agent
