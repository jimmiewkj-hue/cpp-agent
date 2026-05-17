#include "memory/AutoDream.h"

#include "agents/SubAgentManager.h"
#include "infra/ProcessRunner.h"
#include "memory/MemoryIndex.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace agent {
namespace memory {

namespace {

long long NowUnixMs() {
  return static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

bool FileExists(const std::string& path) {
  DWORD attrs = GetFileAttributesA(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream buf; buf << in.rdbuf(); return buf.str();
}

void WriteFile(const std::string& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (out) out << content;
}

bool EnsureDir(const std::string& path) {
  std::string n = path;
  std::replace(n.begin(), n.end(), '/', '\\');
  std::size_t cursor = 0;
  if (n.size() >= 2 && n[1] == ':') cursor = 3;
  while (cursor <= n.size()) {
    auto next = n.find('\\', cursor);
    auto cur = next == std::string::npos ? n : n.substr(0, next);
    DWORD attrs = GetFileAttributesA(cur.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
      if (!CreateDirectoryA(cur.c_str(), nullptr) &&
          GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    if (next == std::string::npos) break;
    cursor = next + 1;
  }
  return true;
}

int CountSessionFiles(const std::string& transcriptDir,
                      long long sinceMs) {
  int count = 0;
  std::string search = transcriptDir + "\\*";
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(search.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    if (fd.cFileName[0] == '.') continue;
    LARGE_INTEGER ft;
    ft.LowPart = fd.ftLastWriteTime.dwLowDateTime;
    ft.HighPart = fd.ftLastWriteTime.dwHighDateTime;
    long long fileTimeMs = ft.QuadPart / 10000LL - 11644473600000LL;
    if (fileTimeMs > sinceMs) ++count;
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  return count;
}

}  // namespace

AutoDreamEngine::AutoDreamEngine(MemoryIndex* memoryIndex,
                                 agents::SubAgentManager* subAgentManager)
    : memoryIndex_(memoryIndex),
      subAgentManager_(subAgentManager) {
  state_.lastConsolidatedAtMs = NowUnixMs();
}

void AutoDreamEngine::Configure(const AutoDreamConfig& config) {
  config_ = config;
}

void AutoDreamEngine::Disable() { state_.enabled = false; }
void AutoDreamEngine::Enable()  { state_.enabled = true; }
bool AutoDreamEngine::IsEnabled() const { return state_.enabled; }

AutoDreamState AutoDreamEngine::state() const { return state_; }

std::string AutoDreamEngine::LockFilePath() const {
  return memoryIndex_->memoryDir() + "\\.consolidate-lock";
}

bool AutoDreamEngine::AcquireLock() {
  if (memoryIndex_ == nullptr) return false;
  std::string path = LockFilePath();
  if (FileExists(path) && !IsLockExpired()) return false;
  WriteFile(path, std::to_string(GetCurrentProcessId()));
  return true;
}

void AutoDreamEngine::ReleaseLock() {
  std::string path = LockFilePath();
  DeleteFileA(path.c_str());
}

bool AutoDreamEngine::IsLockExpired() const {
  std::string path = LockFilePath();
  if (!FileExists(path)) return true;
  std::string content = ReadFile(path);
  int pid = content.empty() ? 0 : std::atoi(content.c_str());
  if (pid > 0) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                           static_cast<DWORD>(pid));
    if (h != nullptr) { CloseHandle(h); return false; }
  }
  return true;
}

bool AutoDreamEngine::IsGateOpen() const {
  return state_.enabled && memoryIndex_ != nullptr;
}

bool AutoDreamEngine::IsTimeGatePassed() const {
  long long now = NowUnixMs();
  long long elapsed = now - state_.lastConsolidatedAtMs;
  return elapsed >= static_cast<long long>(config_.minHours) * 3600000LL;
}

bool AutoDreamEngine::IsSessionGatePassed() {
  long long now = NowUnixMs();
  long long sinceLastScan = now - state_.lastScanAtMs;
  if (sinceLastScan < config_.scanThrottleMs && state_.lastScanAtMs > 0)
    return false;

  const auto& md = memoryIndex_->memoryDir();
  std::string transcriptDir = md + "\\transcripts";
  if (!EnsureDir(transcriptDir)) return false;

  int count = CountSessionFiles(transcriptDir,
      state_.lastConsolidatedAtMs > 0 ? state_.lastConsolidatedAtMs : 0);
  state_.lastScanAtMs = now;
  return count >= config_.minSessions;
}

bool AutoDreamEngine::ShouldExecute() {
  if (!IsGateOpen()) return false;
  if (!IsTimeGatePassed()) return false;
  if (!IsSessionGatePassed()) return false;
  if (!AcquireLock()) return false;
  return true;
}

bool AutoDreamEngine::RunOrientPhase(std::string* context) {
  if (!memoryIndex_ || !context) return false;
  context->append("=== Memory Directory ===\n");
  context->append(memoryIndex_->ReadEntrypoint());
  context->append("\n");
  return true;
}

bool AutoDreamEngine::RunGatherPhase(std::string* context) {
  if (!memoryIndex_ || !context) return false;
  context->append("=== Recent Sessions ===\n");
  context->append("(gathering from transcripts)\n");
  return true;
}

bool AutoDreamEngine::RunConsolidatePhase(const std::string& context) {
  if (!memoryIndex_ || context.empty()) return false;

  if (subAgentManager_) {
    static const std::vector<std::string> kDreamAllowedTools = {
      "FileRead", "Grep", "Glob"
    };

    agents::SubAgentTask task;
    task.prompt = "Consolidate memories from context. "
        "Use only FileRead, Grep, Glob tools. "
        "Merge new signals, resolve contradictions, update entries.\n\n" +
        context;
    task.runInBackground = true;
    task.isolation = "dream";
    task.description = "Auto-Dream memory consolidation";
    task.subagentType = "dream";
    task.priority = 10;

    std::string taskId = subAgentManager_->StartSubTask(task);
  }

  return true;
}

bool AutoDreamEngine::RunPrunePhase() {
  if (!memoryIndex_) return false;
  const std::string raw = memoryIndex_->ReadEntrypoint();
  auto trunc = memoryIndex_->TruncateEntrypointContent(raw);
  if (trunc.wasLineTruncated || trunc.wasByteTruncated) {
    memoryIndex_->WriteEntrypoint(trunc.content);
  }
  return true;
}

bool AutoDreamEngine::Execute() {
  if (!ShouldExecute()) return false;

  std::string context;
  if (!RunOrientPhase(&context)) { ReleaseLock(); return false; }
  if (!RunGatherPhase(&context))  { ReleaseLock(); return false; }
  if (!RunConsolidatePhase(context)) { ReleaseLock(); return false; }
  if (!RunPrunePhase()) { ReleaseLock(); return false; }

  state_.lastConsolidatedAtMs = NowUnixMs();
  ReleaseLock();
  return true;
}

}  // namespace memory
}  // namespace agent
