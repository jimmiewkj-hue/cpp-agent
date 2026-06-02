// ConsolidationLock.h — aligned with local-ace services/autoDream/consolidationLock.ts
// Lock file whose mtime IS lastConsolidatedAt. Body is the holder PID.
// Lives inside the memory dir so it keys on git-root like memory does.
#pragma once

#include <string>
#include <vector>

namespace agent {
namespace memory {

// Stale past this even if the PID is live (PID reuse guard).
// Mirrors local-ace HOLDER_STALE_MS.
constexpr long long kHolderStaleMs = 60 * 60 * 1000;  // 1 hour

// ============================================================================
// Lock path and file ops
// ============================================================================

// Get the lock file path
std::string GetConsolidationLockPath(const std::string& memoryDir);

// mtime of the lock file = lastConsolidatedAt. 0 if absent.
// Per-turn cost: one stat.
// Mirrors local-ace readLastConsolidatedAt
long long ReadLastConsolidatedAt(const std::string& memoryDir);

// Check if the lock is expired (stale or dead holder PID).
// Mirrors local-ace lock staleness check
bool IsConsolidationLockExpired(const std::string& memoryDir);

// Acquire: write PID → mtime = now. Returns true if acquired.
// Returns false if blocked by a live holder.
// Mirrors local-ace tryAcquireConsolidationLock
bool TryAcquireConsolidationLock(const std::string& memoryDir,
                                 long long* priorMtimeMs);

// Release: delete lock file.
void ReleaseConsolidationLock(const std::string& memoryDir);

// Rollback lock mtime on failed fork.
// priorMtime 0 → unlink (restore no-file).
// Mirrors local-ace rollbackConsolidationLock
void RollbackConsolidationLock(const std::string& memoryDir,
                               long long priorMtimeMs);

// Stamp from manual /dream. Optimistic — best-effort.
// Mirrors local-ace recordConsolidation
void RecordConsolidation(const std::string& memoryDir);

// ============================================================================
// Session listing
// ============================================================================

// Session IDs with mtime after sinceMs.
// Uses mtime (sessions TOUCHED since), not birthtime.
// Mirrors local-ace listSessionsTouchedSince
std::vector<std::string> ListSessionsTouchedSince(long long sinceMs);

}  // namespace memory
}  // namespace agent
