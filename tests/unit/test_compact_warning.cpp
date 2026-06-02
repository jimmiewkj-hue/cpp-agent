// Test CompactWarningState — aligned with local-ace compactWarningState.ts
#include "compact/CompactWarningState.h"

#include <cassert>
#include <iostream>

static int failures = 0;

static void Check(bool condition, const char* label) {
  if (!condition) {
    std::cerr << "FAIL: " << label << std::endl;
    ++failures;
  }
}

void TestDefaultState() {
  agent::compact::CompactWarningState state;
  Check(!state.IsSuppressed(), "Default: not suppressed");
}

void TestSuppressAndClear() {
  agent::compact::CompactWarningState state;
  
  state.Suppress();
  Check(state.IsSuppressed(), "After Suppress: is suppressed");
  
  state.Clear();
  Check(!state.IsSuppressed(), "After Clear: not suppressed");
}

void TestCheckAndClear() {
  agent::compact::CompactWarningState state;
  
  // Not suppressed yet
  Check(!state.CheckAndClear(), "CheckAndClear returns false when not suppressed");
  Check(!state.IsSuppressed(), "CheckAndClear clears false state");
  
  // Now suppress
  state.Suppress();
  Check(state.IsSuppressed(), "Suppressed state");
  
  // CheckAndClear should return true and clear
  Check(state.CheckAndClear(), "CheckAndClear returns true when suppressed");
  Check(!state.IsSuppressed(), "CheckAndClear clears the suppressed state");
}

void TestMultipleSuppressClear() {
  agent::compact::CompactWarningState state;
  
  // Round 1
  state.Suppress();
  Check(state.IsSuppressed(), "Round 1: suppressed");
  state.Clear();
  Check(!state.IsSuppressed(), "Round 1: cleared");
  
  // Round 2
  state.Suppress();
  Check(state.IsSuppressed(), "Round 2: suppressed");
  state.Suppress();  // Double suppress is idempotent
  Check(state.IsSuppressed(), "Round 2: still suppressed after double Suppress");
  state.Clear();
  Check(!state.IsSuppressed(), "Round 2: cleared");
  
  // Round 3: CheckAndClear
  state.Suppress();
  state.CheckAndClear();
  Check(!state.IsSuppressed(), "Round 3: cleared via CheckAndClear");
}

int main() {
  std::cout << "=== CompactWarningState Tests ===" << std::endl;
  
  TestDefaultState();
  TestSuppressAndClear();
  TestCheckAndClear();
  TestMultipleSuppressClear();
  
  std::cout << "=== " << failures << " failure(s) ===" << std::endl;
  return failures > 0 ? 1 : 0;
}
