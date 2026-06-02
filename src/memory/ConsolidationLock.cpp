// ConsolidationLock.cpp — aligned with local-ace services/autoDream/consolidationLock.ts
#include "memory/ConsolidationLock.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace agent {
namespace memory {

namespace {

long long NowUnixMs() {
  return static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

bool FileExists(const std::string& path) {
  DWORD attrs = GetFileAttributesA(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
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
          GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    }
    if (next == std::string::npos) break;
    cursor = next + 1;
  }
  return true;
}

long long FileMtimeToUnixMs(const FILETIME& ft) {
  LARGE_INTEGER li;
  li.LowPart = ft.dwLowDateTime;
  li.HighPart = ft.dwHighDateTime;
  // Convert from 100-nanosecond intervals since 1601-01-01 to ms since 1970-01-01
  return li.QuadPart / 10000LL - 11644473600000LL;
}

}  // namespace

// ============================================================================
// Lock path
// ============================================================================
std::string GetConsolidationLockPath(const std::string& memoryDir) {
  return memoryDir.empty() ? ".consolidate-lock"
                           : memoryDir + "\\.consolidate-lock";
}

// ============================================================================
// ReadLastConsolidatedAt — mirrors local-ace readLastConsolidatedAt
// ============================================================================
long long ReadLastConsolidatedAt(const std::string& memoryDir) {
  std::string path = GetConsolidationLockPath(memoryDir);
  if (!FileExists(path)) return 0;

  WIN32_FILE_ATTRIBUTE_DATA attr;
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attr))
    return 0;

  return FileMtimeToUnixMs(attr.ftLastWriteTime);
}

// ============================================================================
// IsConsolidationLockExpired — mirrors local-ace lock staleness check
// ============================================================================
bool IsConsolidationLockExpired(const std::string& memoryDir) {
  std::string path = GetConsolidationLockPath(memoryDir);
  if (!FileExists(path)) return true;

  long long mtimeMs = ReadLastConsolidatedAt(memoryDir);
  long long ageMs = NowUnixMs() - mtimeMs;
  if (ageMs >= kHolderStaleMs) return true;

  // Read PID from lock body
  std::string content = ReadFile(path);
  int pid = content.empty() ? 0 : std::atoi(content.c_str());
  if (pid > 0) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                           static_cast<DWORD>(pid));
    if (h != nullptr) {
      CloseHandle(h);
      return false;  // live holder
    }
  }
  return true;  // dead PID or stale
}

// ============================================================================
// TryAcquireConsolidationLock — mirrors local-ace tryAcquireConsolidationLock
// ============================================================================
bool TryAcquireConsolidationLock(const std::string& memoryDir,
                                 long long* priorMtimeMs) {
  std::string path = GetConsolidationLockPath(memoryDir);

  // Read prior state
  long long mtimeMs = 0;
  int holderPid = 0;
  if (FileExists(path)) {
    mtimeMs = ReadLastConsolidatedAt(memoryDir);
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
  EnsureDir(memoryDir);
  WriteFile(path, std::to_string(GetCurrentProcessId()));

  // Re-read to verify we won the race
  std::string verify = ReadFile(path);
  if (std::atoi(verify.c_str()) != static_cast<int>(GetCurrentProcessId()))
    return false;

  if (priorMtimeMs) *priorMtimeMs = mtimeMs;
  return true;
}

// ============================================================================
// ReleaseConsolidationLock
// ============================================================================
void ReleaseConsolidationLock(const std::string& memoryDir) {
  std::string path = GetConsolidationLockPath(memoryDir);
  DeleteFileA(path.c_str());
}

// ============================================================================
// RollbackConsolidationLock — mirrors local-ace rollbackConsolidationLock
// ============================================================================
void RollbackConsolidationLock(const std::string& memoryDir,
                               long long priorMtimeMs) {
  std::string path = GetConsolidationLockPath(memoryDir);
  if (priorMtimeMs == 0) {
    DeleteFileA(path.c_str());
    return;
  }

  // Write empty body and set mtime back
  WriteFile(path, "");

  // Convert ms to FILETIME
  long long ft = (priorMtimeMs + 11644473600000LL) * 10000LL;
  FILETIME fileTime;
  fileTime.dwLowDateTime = static_cast<DWORD>(ft & 0xFFFFFFFF);
  fileTime.dwHighDateTime = static_cast<DWORD>((ft >> 32) & 0xFFFFFFFF);

  HANDLE h = CreateFileA(path.c_str(), FILE_WRITE_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    SetFileTime(h, nullptr, nullptr, &fileTime);
    CloseHandle(h);
  }
}

// ============================================================================
// RecordConsolidation — mirrors local-ace recordConsolidation
// ============================================================================
void RecordConsolidation(const std::string& memoryDir) {
  try {
    EnsureDir(memoryDir);
    WriteFile(GetConsolidationLockPath(memoryDir),
              std::to_string(GetCurrentProcessId()));
  } catch (...) {
    // Best-effort
  }
}

// ============================================================================
// ListSessionsTouchedSince — mirrors local-ace listSessionsTouchedSince
// Simplified: returns empty (requires session storage access that's not
// available at this module level). Caller should use session storage directly.
// ============================================================================
std::vector<std::string> ListSessionsTouchedSince(long long /*sinceMs*/) {
  // Stub: session listing requires access to session storage paths.
  // The AutoDream engine should provide this from its own context.
  return {};
}

}  // namespace memory
}  // namespace agent
