#include "compact/AutoCompact.h"

#include <cassert>
#include <iostream>
#include <string>

static int failures = 0;

static void Check(bool condition, const char* label) {
  if (!condition) {
    std::cerr << "FAIL: " << label << std::endl;
    ++failures;
  }
}

// Thresholds for (200000 context, 16000 maxOutputTokens):
// effective        = 200000 - min(16000, 20000) = 184000
// autoCompactT     = 184000 - 13000 = 171000
// warningT         = 171000 - 20000 = 151000
// errorT           = 171000 - 20000 = 151000
// blockingLimit    = 184000 - 3000  = 181000

void TestGetEffectiveContextWindowSize() {
  using agent::compact::GetEffectiveContextWindowSize;
  
  int effective = GetEffectiveContextWindowSize(200000, 16000);
  int expected = 200000 - 16000;  // min(16000,20000) = 16000
  Check(effective == expected, "GetEffectiveContextWindowSize: 200K-16K=184K");
  
  effective = GetEffectiveContextWindowSize(100000, 8000);
  expected = 100000 - 8000;
  Check(effective == expected, "GetEffectiveContextWindowSize: 100K-8K=92K");
  
  effective = GetEffectiveContextWindowSize(200000, 100000);
  expected = 200000 - 20000;  // capped at kMaxOutputTokensForSummary
  Check(effective == expected, "GetEffectiveContextWindowSize: large output capped at 20K");
  
  effective = GetEffectiveContextWindowSize(0, 16000);
  expected = -16000;
  Check(effective == expected, "GetEffectiveContextWindowSize: zero context");
}

void TestGetAutoCompactThreshold() {
  using agent::compact::GetAutoCompactThreshold;
  
  int threshold = GetAutoCompactThreshold(184000);
  int expected = 184000 - 13000;
  Check(threshold == expected, "GetAutoCompactThreshold: 184K-13K=171K");
  
  threshold = GetAutoCompactThreshold(20000);
  expected = 20000 - 13000;
  Check(threshold == expected, "GetAutoCompactThreshold: 20K-13K=7K");
  
  threshold = GetAutoCompactThreshold(0);
  expected = -13000;
  Check(threshold == expected, "GetAutoCompactThreshold: zero window");
}

void TestCalculateTokenWarningState() {
  using agent::compact::CalculateTokenWarningState;
  
  // Safe: 50000 well below warning (151000)
  {
    auto state = CalculateTokenWarningState(50000, 200000, 16000);
    Check(!state.isAboveWarningThreshold, "Safe 50K: not above warning");
    Check(!state.isAboveErrorThreshold, "Safe 50K: not above error");
    Check(!state.isAboveAutoCompactThreshold, "Safe 50K: not above autocompact");
    Check(!state.isAtBlockingLimit, "Safe 50K: not at blocking");
    Check(state.percentLeft > 50, "Safe 50K: high percent");
  }
  
  // Above warning (151000) but below autoCompact (171000)
  {
    auto state = CalculateTokenWarningState(155000, 200000, 16000);
    Check(state.isAboveWarningThreshold, "155K: above warning (151K)");
    Check(!state.isAboveAutoCompactThreshold, "155K: below autocompact (171K)");
  }
  
  // Above autoCompact: 172000 >= 171000
  {
    auto state = CalculateTokenWarningState(172000, 200000, 16000);
    Check(state.isAboveAutoCompactThreshold, "172K: triggers autocompact");
    Check(state.isAboveWarningThreshold, "172K: also above warning");
    Check(state.isAboveErrorThreshold, "172K: also above error");
    Check(!state.isAtBlockingLimit, "172K: not at blocking (181K)");
  }
  
  // At blocking limit: 181000
  {
    auto state = CalculateTokenWarningState(181000, 200000, 16000);
    Check(state.isAtBlockingLimit, "181K: at blocking limit");
    Check(state.isAboveAutoCompactThreshold, "181K: triggers autocompact");
  }
  
  // Over blocking limit
  {
    auto state = CalculateTokenWarningState(190000, 200000, 16000);
    Check(state.isAtBlockingLimit, "190K: at blocking limit");
    Check(state.percentLeft == 0, "190K: 0% left");
  }
}

void TestIsAutoCompactEnabled() {
  using agent::compact::IsAutoCompactEnabled;
  
  bool enabled = IsAutoCompactEnabled();
  Check(enabled, "IsAutoCompactEnabled: default enabled");
}

void TestShouldAutoCompact() {
  using agent::compact::ShouldAutoCompact;
  
  // 50000 < 171000 -> false
  Check(!ShouldAutoCompact(50000, 200000, 16000),
        "ShouldAutoCompact: 50K -> false");
  
  // 172000 >= 171000 -> true
  Check(ShouldAutoCompact(172000, 200000, 16000),
        "ShouldAutoCompact: 172K -> true");
  
  // 171000 >= 171000 -> true
  Check(ShouldAutoCompact(171000, 200000, 16000),
        "ShouldAutoCompact: 171K (at threshold) -> true");
  
  // 170999 < 171000 -> false
  Check(!ShouldAutoCompact(170999, 200000, 16000),
        "ShouldAutoCompact: 170999 -> false");
}

void TestAutoCompactIfNeeded() {
  using agent::compact::AutoCompactIfNeeded;
  using agent::compact::AutoCompactTrackingState;
  
  // Below threshold
  {
    AutoCompactTrackingState tracking;
    auto decision = AutoCompactIfNeeded(50000, 200000, 16000, &tracking);
    Check(!decision.wasCompacted, "AICN 50K: not compacted");
    Check(!decision.circuitBreakerTripped, "AICN 50K: no breaker");
  }
  
  // Above threshold: 172000 >= 171000
  {
    AutoCompactTrackingState tracking;
    auto decision = AutoCompactIfNeeded(172000, 200000, 16000, &tracking);
    Check(decision.wasCompacted, "AICN 172K: compacted");
    Check(tracking.compacted, "AICN 172K: tracking.compacted=true");
    Check(tracking.consecutiveFailures == 0, "AICN 172K: failures reset");
  }
  
  // Circuit breaker at 3
  {
    AutoCompactTrackingState tracking;
    tracking.consecutiveFailures = 3;
    auto decision = AutoCompactIfNeeded(172000, 200000, 16000, &tracking);
    Check(decision.circuitBreakerTripped, "AICN: breaker at 3 failures");
    Check(!decision.wasCompacted, "AICN: no compaction when breaker");
  }
  
  // Circuit breaker above 3
  {
    AutoCompactTrackingState tracking;
    tracking.consecutiveFailures = 5;
    auto decision = AutoCompactIfNeeded(172000, 200000, 16000, &tracking);
    Check(decision.circuitBreakerTripped, "AICN: breaker at 5 failures");
  }
  
  // 2 failures: still allows compaction
  {
    AutoCompactTrackingState tracking;
    tracking.consecutiveFailures = 2;
    auto decision = AutoCompactIfNeeded(172000, 200000, 16000, &tracking);
    Check(decision.wasCompacted, "AICN: 2 failures still compacts");
    Check(!decision.circuitBreakerTripped, "AICN: no breaker at 2");
    Check(tracking.consecutiveFailures == 0, "AICN: failures reset to 0 on success");
  }
  
  // Null tracking: works
  {
    auto decision = AutoCompactIfNeeded(172000, 200000, 16000, nullptr);
    Check(decision.wasCompacted, "AICN: works with null tracking");
  }
  
  // Below threshold with tracking
  {
    AutoCompactTrackingState tracking;
    tracking.consecutiveFailures = 2;
    auto decision = AutoCompactIfNeeded(50000, 200000, 16000, &tracking);
    Check(!decision.wasCompacted, "AICN 50K tracking: not compacted");
    // Should keep failure count since no compaction happened
    Check(tracking.consecutiveFailures == 2, "AICN 50K tracking: failures unchanged");
  }
}

int main() {
  std::cout << "=== AutoCompact Tests ===" << std::endl;
  
  TestGetEffectiveContextWindowSize();
  TestGetAutoCompactThreshold();
  TestCalculateTokenWarningState();
  TestIsAutoCompactEnabled();
  TestShouldAutoCompact();
  TestAutoCompactIfNeeded();
  
  std::cout << "=== " << failures << " failure(s) ===" << std::endl;
  return failures > 0 ? 1 : 0;
}
