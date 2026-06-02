// PostCompactCleanup.cpp — aligned with local-ace services/compact/postCompactCleanup.ts
#include "compact/PostCompactCleanup.h"

namespace agent {
namespace compact {

// Track whether cleanup has been called since last compaction.
static bool g_cleanupCalled = false;
static int g_compactCount = 0;

void RunPostCompactCleanup(bool isMainThread) {
  g_cleanupCalled = true;
  ++g_compactCount;

  // In cpp-agent, most cache state lives in the QueryLoopContext
  // and is automatically invalidated when the loop ends.
  // This function exists as a hook point for:
  // 1. Resetting microcompact tracking state
  // 2. Clearing speculative check results
  // 3. Resetting classifier approvals accumulated during the conversation
  // 4. Session-level cache invalidation
  //
  // The actual reset logic is handled by the QueryLoop context cycle.
  // This module provides the interface alignment with local-ace.

  if (!isMainThread) {
    // Subagent: skip main-thread module-level state resets.
    // Only reset compact-level tracking.
    return;
  }

  // Main thread: full cleanup.
  // Future: integrate with SessionMemory cache invalidation,
  // classifier approval clearing, and speculative check reset.
}

bool IsPostCompactCleanupDone() {
  return g_cleanupCalled;
}

int GetCompactCount() {
  return g_compactCount;
}

}  // namespace compact
}  // namespace agent
