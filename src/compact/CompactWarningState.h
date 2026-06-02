// CompactWarningState.h — aligned with local-ace services/compact/compactWarningState.ts
// Tracks whether the "context left until autocompact" warning should be suppressed.
// We suppress immediately after successful compaction since accurate token counts
// aren't available until the next API response.
#pragma once

namespace agent {
namespace compact {

class CompactWarningState {
 public:
  CompactWarningState() = default;

  // Whether the compact warning is currently suppressed
  bool IsSuppressed() const { return suppressed_; }

  // Suppress the compact warning. Call after successful compaction.
  // Mirrors local-ace suppressCompactWarning()
  void Suppress() { suppressed_ = true; }

  // Clear the compact warning suppression. Call at start of new compact attempt.
  // Mirrors local-ace clearCompactWarningSuppression()
  void Clear() { suppressed_ = false; }

  // Check and clear: returns true if suppressed, then clears it atomically.
  // Useful for one-shot suppression check.
  bool CheckAndClear() {
    bool was = suppressed_;
    suppressed_ = false;
    return was;
  }

 private:
  bool suppressed_ = false;
};

}  // namespace compact
}  // namespace agent
