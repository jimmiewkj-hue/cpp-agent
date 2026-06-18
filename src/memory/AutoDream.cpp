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

// P0-03: Stale lock threshold (aligned with local-ace HOLDER_STALE_MS)
const long long kHolderStaleMs = 60 * 60 * 1000;  // 1 hour

// P0-03: Scan throttle (aligned with local-ace SESSION_SCAN_INTERVAL_MS)
const long long kSessionScanIntervalMs = 5 * 60 * 1000;  // 5 minutes

}  // namespace

// =============================================================================
// Constructor
// =============================================================================
AutoDreamEngine::AutoDreamEngine(MemoryIndex* memoryIndex,
                                 agents::SubAgentManager* subAgentManager)
    : memoryIndex_(memoryIndex),
      subAgentManager_(subAgentManager) {
  long long lockMtime = ReadLastConsolidatedAt();
  // P0-03: If no prior consolidation (lock file absent), start the clock now.
  // Prevents time gate from passing immediately (epoch mtime = 0 would make
  // hoursSince = epoch-to-now, which always exceeds minHours).
  state_.lastConsolidatedAtMs = (lockMtime > 0) ? lockMtime : NowUnixMs();
}

// =============================================================================
// Config / State
// =============================================================================
void AutoDreamEngine::Configure(const AutoDreamConfig& config) {
  config_ = config;
}

void AutoDreamEngine::Disable() { state_.enabled = false; }
void AutoDreamEngine::Enable()  { state_.enabled = true; }
bool AutoDreamEngine::IsEnabled() const { return state_.enabled; }
AutoDreamState AutoDreamEngine::state() const { return state_; }

// =============================================================================
// Lock file path (lives inside memory dir, aligned with local-ace)
// =============================================================================
std::string AutoDreamEngine::LockFilePath() const {
  if (!memoryIndex_) return ".consolidate-lock";
  return memoryIndex_->memoryDir() + "\\.consolidate-lock";
}

// =============================================================================
// Lock mechanism (P0-03: aligned with local-ace consolidationLock)
// Lock file's mtime IS lastConsolidatedAt. Body is holder PID.
// =============================================================================
long long AutoDreamEngine::ReadLastConsolidatedAt() const {
  std::string path = LockFilePath();
  if (!FileExists(path)) return 0;

  WIN32_FILE_ATTRIBUTE_DATA attr;
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attr))
    return 0;

  LARGE_INTEGER ft;
  ft.LowPart = attr.ftLastWriteTime.dwLowDateTime;
  ft.HighPart = attr.ftLastWriteTime.dwHighDateTime;
  return ft.QuadPart / 10000LL - 11644473600000LL;
}

bool AutoDreamEngine::IsLockExpired() const {
  std::string path = LockFilePath();
  if (!FileExists(path)) return true;

  long long mtimeMs = ReadLastConsolidatedAt();
  long long ageMs = NowUnixMs() - mtimeMs;
  if (ageMs >= kHolderStaleMs) return true;

  // Read PID from lock body
  std::string content = ReadFile(path);
  int pid = content.empty() ? 0 : std::atoi(content.c_str());
  if (pid > 0) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                           static_cast<DWORD>(pid));
    if (h != nullptr) { CloseHandle(h); return false; }
  }
  return true;  // dead PID or stale
}

bool AutoDreamEngine::AcquireLock() {
  if (!memoryIndex_) return false;
  long long priorMtime;
  return TryAcquireLock(&priorMtime);
}

// P0-03: TryAcquireLock returns true if acquired (aligned with local-ace tryAcquireConsolidationLock)
bool AutoDreamEngine::TryAcquireLock(long long* priorMtimeMs) {
  if (!memoryIndex_) return false;

  std::string path = LockFilePath();

  // Read prior state
  long long mtimeMs = 0;
  int holderPid = 0;
  if (FileExists(path)) {
    mtimeMs = ReadLastConsolidatedAt();
    std::string raw = ReadFile(path);
    holderPid = raw.empty() ? 0 : std::atoi(raw.c_str());
  }

  // Check if lock is held by a live process
  if (mtimeMs > 0 && (NowUnixMs() - mtimeMs) < kHolderStaleMs) {
    if (holderPid > 0) {
      HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                             static_cast<DWORD>(holderPid));
      if (h != nullptr) {
        CloseHandle(h);
        return false;  // live holder, can't acquire
      }
    }
    // Dead PID or unparseable body: reclaim
  }

  // Write our PID
  EnsureDir(memoryIndex_->memoryDir());
  WriteFile(path, std::to_string(GetCurrentProcessId()));

  // Re-read to verify we won the race
  std::string verify = ReadFile(path);
  if (std::atoi(verify.c_str()) != static_cast<int>(GetCurrentProcessId()))
    return false;

  if (priorMtimeMs) *priorMtimeMs = mtimeMs;
  return true;
}

void AutoDreamEngine::ReleaseLock() {
  std::string path = LockFilePath();
  DeleteFileA(path.c_str());
}

// P0-03: Rollback lock mtime on failed fork (aligned with local-ace rollbackConsolidationLock)
void AutoDreamEngine::RollbackConsolidationLock(long long priorMtimeMs) {
  std::string path = LockFilePath();
  if (priorMtimeMs == 0) {
    DeleteFileA(path.c_str());
    return;
  }

  // Write empty body and set mtime back
  WriteFile(path, "");
  HANDLE h = CreateFileA(path.c_str(), FILE_WRITE_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    long long ft = (priorMtimeMs + 11644473600000LL) * 10000LL;
    FILETIME fileTime;
    fileTime.dwLowDateTime = static_cast<DWORD>(ft & 0xFFFFFFFF);
    fileTime.dwHighDateTime = static_cast<DWORD>(ft >> 32);
    SetFileTime(h, nullptr, nullptr, &fileTime);
    CloseHandle(h);
  }
}

// =============================================================================
// Session listing (P0-03: aligned with local-ace listSessionsTouchedSince)
// =============================================================================
std::vector<std::string> AutoDreamEngine::ListSessionsTouchedSince(
    long long sinceMs) const {
  std::vector<std::string> result;
  if (!memoryIndex_) return result;

  std::string searchPath = memoryIndex_->memoryDir() + "\\..\\transcripts\\*";
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(searchPath.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return result;

  do {
    if (fd.cFileName[0] == '.') continue;
    LARGE_INTEGER ft;
    ft.LowPart = fd.ftLastWriteTime.dwLowDateTime;
    ft.HighPart = fd.ftLastWriteTime.dwHighDateTime;
    long long fileTimeMs = ft.QuadPart / 10000LL - 11644473600000LL;
    if (fileTimeMs > sinceMs) {
      std::string name = fd.cFileName;
      // Strip extension for session ID
      size_t dotPos = name.rfind('.');
      if (dotPos != std::string::npos) name = name.substr(0, dotPos);
      result.push_back(name);
    }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  return result;
}

// =============================================================================
// Gate checks (P0-03: aligned with local-ace)
// =============================================================================
bool AutoDreamEngine::IsGateOpen() const {
  return state_.enabled && memoryIndex_ != nullptr;
}

bool AutoDreamEngine::IsTimeGatePassed() const {
  long long now = NowUnixMs();
  long long lastAt = state_.lastConsolidatedAtMs;
  // P0-03: Read from lock file mtime (aligns with local-ace readLastConsolidatedAt)
  long long lockMtime = ReadLastConsolidatedAt();
  if (lockMtime > lastAt) lastAt = lockMtime;
  long long hoursSince = (now - lastAt) / 3600000LL;
  return hoursSince >= config_.minHours;
}

bool AutoDreamEngine::IsSessionGatePassed() {
  long long now = NowUnixMs();
  long long sinceScan = now - state_.lastScanAtMs;
  if (sinceScan < kSessionScanIntervalMs && state_.lastScanAtMs > 0)
    return false;

  state_.lastScanAtMs = now;

  long long lastAt = state_.lastConsolidatedAtMs;
  long long lockMtime = ReadLastConsolidatedAt();
  if (lockMtime > lastAt) lastAt = lockMtime;

  auto sessions = ListSessionsTouchedSince(lastAt);
  return static_cast<int>(sessions.size()) >= config_.minSessions;
}

bool AutoDreamEngine::ShouldExecute() {
  if (!IsGateOpen()) return false;
  if (!IsTimeGatePassed()) return false;
  if (!IsSessionGatePassed()) return false;
  long long priorMtime;
  if (!TryAcquireLock(&priorMtime)) return false;
  return true;
}

// =============================================================================
// Dream task management (P0-03: aligned with local-ace DreamTask)
// =============================================================================
std::string AutoDreamEngine::RegisterDreamTask(const DreamTaskState& task) {
  std::string taskId = task.taskId.empty()
      ? "dream-" + std::to_string(NowUnixMs())
      : task.taskId;
  DreamTaskState st = task;
  st.taskId = taskId;
  st.status = DreamTaskStatus::Running;
  st.startedAtMs = NowUnixMs();
  dreamTasks_[taskId] = st;
  return taskId;
}

void AutoDreamEngine::CompleteDreamTask(const std::string& taskId) {
  auto it = dreamTasks_.find(taskId);
  if (it == dreamTasks_.end()) return;
  it->second.status = DreamTaskStatus::Completed;
}

void AutoDreamEngine::FailDreamTask(const std::string& taskId) {
  auto it = dreamTasks_.find(taskId);
  if (it == dreamTasks_.end()) return;
  it->second.status = DreamTaskStatus::Failed;
}

const DreamTaskState* AutoDreamEngine::GetDreamTask(
    const std::string& taskId) const {
  auto it = dreamTasks_.find(taskId);
  if (it == dreamTasks_.end()) return nullptr;
  return &it->second;
}

// =============================================================================
// Consolidation prompt (P0-03: aligned with local-ace buildConsolidationPrompt)
// =============================================================================
std::string AutoDreamEngine::BuildConsolidationPrompt(
    const std::string& extra) const {
  std::string prompt;
  prompt += "# Dream: Memory Consolidation\n\n";
  prompt += "You are performing a dream ? a reflective pass over your memory files. ";
  prompt += "Synthesize what you've learned recently into durable, well-organized ";
  prompt += "memories so that future sessions can orient quickly.\n\n";

  if (memoryIndex_) {
    prompt += "Memory directory: " + memoryIndex_->memoryDir() + "\n";
    prompt += "(If the directory doesn't exist yet, create it.)\n\n";
  }

  prompt += "---\n\n";

  // Phase 1 - Orient
  prompt += "## Phase 1 ? Orient\n\n";
  prompt += "- ls the memory directory to see what already exists\n";
  prompt += "- Read MEMORY.md to understand the current index\n";
  prompt += "- Skim existing topic files so you improve them rather than creating duplicates\n\n";

  // Phase 2 - Gather
  prompt += "## Phase 2 ? Gather recent signal\n\n";
  prompt += "Look for new information worth persisting. Sources in priority order:\n\n";
  prompt += "1. **Existing memories that drifted** ? facts that contradict the codebase now\n";
  prompt += "2. **Transcript search** ? grep JSONL transcripts for narrow terms\n";
  prompt += "3. **User feedback** ? corrections, preferences, behavior notes\n\n";
  prompt += "Don't exhaustively read transcripts. Look only for what you suspect matters.\n\n";

  // Phase 3 - Consolidate
  prompt += "## Phase 3 ? Consolidate\n\n";
  prompt += "For each thing worth remembering, write/update a memory file. ";
  prompt += "Use the memory file format from your system prompt.\n\n";
  prompt += "Focus on:\n";
  prompt += "- Merging new signal into existing topic files\n";
  prompt += "- Converting relative dates to absolute dates\n";
  prompt += "- Deleting contradicted facts ? latest evidence wins\n\n";

  // Phase 4 - Prune
  prompt += "## Phase 4 ? Prune and index\n\n";
  prompt += "Update MEMORY.md so it stays under " +
      std::to_string(MemoryIndex::kMaxEntrypointLines) +
      " lines and under ~" +
      std::to_string(MemoryIndex::kMaxEntrypointBytes / 1000) + "KB.\n";
  prompt += "Each entry: - [Title](file.md) ? one-line hook (under 150 chars).\n";
  prompt += "Never write memory content directly into MEMORY.md.\n\n";
  prompt += "- Remove stale pointers\n";
  prompt += "- Shorten verbose entries\n";
  prompt += "- Add pointers to new memories\n\n";

  prompt += "---\n\n";
  prompt += "Return a brief summary of what you consolidated, updated, or pruned. ";
  prompt += "If nothing changed, say so.\n";

  if (!extra.empty()) {
    prompt += "\n## Additional context\n\n" + extra + "\n";
  }

  return prompt;
}

// =============================================================================
// Orient phase
// =============================================================================
bool AutoDreamEngine::RunOrientPhase(std::string* context) {
  if (!memoryIndex_ || !context) return false;

  std::string prompt;
  prompt += "## Phase 1 ? Orient (??)\n\n";
  prompt += "### Current Memory Directory\n";
  prompt += "Path: " + memoryIndex_->memoryDir() + "\n\n";

  std::string entrypoint = memoryIndex_->ReadEntrypoint();
  if (!entrypoint.empty()) {
    prompt += "### MEMORY.md Index\n\n";
    prompt += entrypoint + "\n";
  } else {
    prompt += "### MEMORY.md\n(currently empty)\n\n";
  }

  prompt += "### Existing Topic Files\n";
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(
      (memoryIndex_->memoryDir() + "\\*.md").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    prompt += "(no topic files yet)\n";
  } else {
    do {
      if (fd.cFileName[0] == '.') continue;
      std::string name = fd.cFileName;
      if (name == "MEMORY.md") continue;
      prompt += "- " + name + "\n";
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }

  *context = prompt;
  return true;
}

// =============================================================================
// Gather phase
// =============================================================================
bool AutoDreamEngine::RunGatherPhase(std::string* context) {
  if (!memoryIndex_ || !context) return false;

  std::string prompt;
  prompt += "## Phase 2 ? Gather (??)\n\n";
  prompt += "### Review recent session transcripts for new signals\n";
  prompt += "Look for:\n";
  prompt += "- Explicit user requests to remember something\n";
  prompt += "- User corrections or feedback on your behavior\n";
  prompt += "- Project-specific decisions or constraints\n";
  prompt += "- References to external systems (JIRA, Slack, etc.)\n";
  prompt += "- Patterns that have changed since last consolidation\n\n";

  prompt += "Search transcript files for these signals.\n";
  long long lastAt = state_.lastConsolidatedAtMs;
  long long lockMtime = ReadLastConsolidatedAt();
  if (lockMtime > lastAt) lastAt = lockMtime;
  prompt += "Previous consolidation timestamp: " +
      std::to_string(lastAt) + "\n";
  prompt += "Sessions since: " +
      std::to_string(ListSessionsTouchedSince(lastAt).size()) + "\n\n";

  *context += prompt;
  return true;
}

// =============================================================================
// Consolidate phase
// =============================================================================
bool AutoDreamEngine::RunConsolidatePhase(const std::string& context) {
  if (!memoryIndex_ || context.empty()) return false;

  std::string prompt;
  prompt += "## Phase 3 ? Consolidate (??)\n\n";
  prompt += "Merge new signals with existing memory files:\n";
  prompt += "1. Update existing topic files when new info relates to them\n";
  prompt += "2. Create new topic files for entirely new information\n";
  prompt += "3. Convert relative dates (\"last week\") to absolute dates\n";
  prompt += "4. Remove contradictory facts ? latest evidence wins\n";
  prompt += "5. Each topic file MUST have frontmatter with type field:\n";
  prompt += "   type: user | feedback | project | reference\n\n";

  prompt += "## Phase 4 ? Prune (??)\n\n";
  prompt += "1. Rebuild MEMORY.md index ? one bullet per topic file, under 150 chars\n";
  prompt += "2. Remove pointers to deleted or empty topic files\n";
  prompt += "3. Keep MEMORY.md within " +
      std::to_string(MemoryIndex::kMaxEntrypointLines) +
      " lines / " +
      std::to_string(MemoryIndex::kMaxEntrypointBytes) + " bytes\n";
  prompt += "4. Shorten overly long entries\n";
  prompt += "5. Use format: - [Title](file.md) ? one-line hook\n\n";

  std::string fullPrompt = context + prompt;

  if (subAgentManager_) {
    // P0-03: Register dream task before starting (aligned with local-ace)
    DreamTaskState task;
    task.sessionsReviewing = static_cast<int>(
        ListSessionsTouchedSince(state_.lastConsolidatedAtMs).size());
    task.priorMtimeMs = ReadLastConsolidatedAt();

    agents::SubAgentTask subTask;
    subTask.prompt = fullPrompt;
    subTask.runInBackground = true;
    subTask.isolation = "dream";
    subTask.description = "Auto-Dream: consolidate " +
        std::to_string(config_.minSessions) + "+ sessions of memories";
    subTask.subagentType = "dream";
    subTask.priority = 10;
    subAgentManager_->StartSubTask(subTask);
  }

  return true;
}

// =============================================================================
// Prune phase
// =============================================================================
bool AutoDreamEngine::RunPrunePhase() {
  if (!memoryIndex_) return false;
  const std::string raw = memoryIndex_->ReadEntrypoint();
  auto trunc = memoryIndex_->TruncateEntrypointContent(raw);
  if (trunc.wasLineTruncated || trunc.wasByteTruncated) {
    memoryIndex_->WriteEntrypoint(trunc.content);
  }
  return true;
}

// =============================================================================
// P1-3: RunDistillPhase — skeleton for pattern extraction from history
// =============================================================================
// Aligned with MiMo Code's Distill mechanism. This phase runs on a separate,
// slower schedule (30 days / 50 sessions) and aims to extract reusable
// patterns from accumulated session history. Currently a skeleton — the
// actual pattern mining logic will be iterated in future sprints.
//
// The distill phase:
//   1. Checks scheduling gate (30 days OR 50 sessions since last distill)
//   2. Lists historical sessions since last distill
//   3. Placeholder: logs the number of sessions to review
//   4. Future: extract common patterns, anti-patterns, tool usage sequences
bool AutoDreamEngine::RunDistillPhase() {
  if (!state_.enabled) return true;  // skip silently when disabled

  // Scheduling gate
  long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  long long daysSinceLast = (nowMs - state_.lastDistilledAtMs) /
      (24LL * 60 * 60 * 1000);

  bool timeGatePassed = (daysSinceLast >= config_.distillMinDays);

  // P1-3: Compute session count since last distill for scheduling gate
  std::vector<std::string> sessionsSinceDistill =
      ListSessionsTouchedSince(state_.lastDistilledAtMs);
  state_.sessionsSinceDistill = static_cast<int>(sessionsSinceDistill.size());
  bool sessionGatePassed = (state_.sessionsSinceDistill >= config_.distillMinSessions);

  if (!timeGatePassed && !sessionGatePassed) {
    return true;  // not time yet, not a failure
  }

  if (sessionsSinceDistill.empty()) {
    return true;  // nothing to distill
  }

  // SKELETON: Log what would be processed. Pattern mining is future work.
  // In the future, this phase will:
  //   - Read session transcripts from the listed sessions
  //   - Identify recurring tool call patterns (e.g., Read→Edit→Bash cycles)
  //   - Detect common failure modes and recovery sequences
  //   - Extract reusable "recipes" that can be injected as system prompts
  //   - Write distilled patterns to the MemoryIndex with origin="dream"
  // sessionsSinceDistill contains the sessions to process

  state_.lastDistilledAtMs = nowMs;
  state_.sessionsSinceDistill = 0;

  return true;
}

// =============================================================================
// Execute (main entry point)
// =============================================================================
bool AutoDreamEngine::Execute() {
  if (!IsGateOpen()) return false;

  long long priorMtime;
  if (!TryAcquireLock(&priorMtime)) return false;

  // P0-03: Update state from lock file (aligned with local-ace)
  state_.lastConsolidatedAtMs = ReadLastConsolidatedAt();

  std::string context;
  if (!RunOrientPhase(&context)) { ReleaseLock(); return false; }
  if (!RunGatherPhase(&context))  { ReleaseLock(); return false; }
  if (!RunConsolidatePhase(context)) {
    // P0-03: Rollback lock mtime on failed fork (aligned with local-ace)
    RollbackConsolidationLock(priorMtime);
    return false;
  }
  if (!RunPrunePhase()) { ReleaseLock(); return false; }

  // P1-3: Distill phase — runs on a separate schedule from the regular
  // dream cycle. Currently a skeleton; pattern mining to be iterated later.
  RunDistillPhase();

  // P0-03: Release lock on success ? mtime stays at now (aligned with local-ace)
  ReleaseLock();
  return true;
}

}  // namespace memory
}  // namespace agent

