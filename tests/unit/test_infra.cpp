#include "infra/ProcessRunner.h"
#include "infra/ProtoLite.h"
#include "infra/SessionManager.h"
#include "infra/StabilityWatchdog.h"

#include <windows.h>

#include <iostream>

static int failures = 0;
static void Check(bool condition, const char* label) {
  if (!condition) { std::cerr << "FAIL: " << label << std::endl; ++failures; }
}

namespace {

std::string TestSessionDir() {
  return "build\\p3-test-session";
}

void EnsureTestSessionDir() {
  CreateDirectoryA("build", nullptr);
  CreateDirectoryA(TestSessionDir().c_str(), nullptr);
}

void TestProcessRunner() {
  agent::infra::ProcessRunOptions options;
  options.executable = "cmd.exe";
  options.arguments = {"/c", "echo P3-ok"};
  options.timeoutMs = 10000;

  agent::infra::ProcessRunner runner;
  agent::infra::ProcessRunResult result = runner.Run(options);

  Check(!result.spawnFailed, "ProcessRunner should spawn");
  Check(result.exitCode == 0, "ProcessRunner exit 0");
  Check(result.stdoutText.find("P3-ok") != std::string::npos,
        "ProcessRunner captures stdout");
}

void TestProtoLiteEncodeDecode() {
  std::string encoded;
  agent::infra::protolite::WriteString(&encoded, 1, "proto-test-data");
  agent::infra::protolite::WriteInt32(&encoded, 2, 2024);
  Check(!encoded.empty(), "ProtoLite encode produces output");

  agent::infra::protolite::Reader reader(encoded);
  agent::infra::protolite::Field field;
  int count = 0;
  while (reader.ReadField(&field)) {
    ++count;
    if (field.number == 1 &&
        field.wireType == agent::infra::protolite::WireType::LengthDelimited) {
      Check(field.lengthDelimitedValue == "proto-test-data", "ProtoLite str");
    }
    if (field.number == 2 &&
        field.wireType == agent::infra::protolite::WireType::Varint) {
      Check(field.varintValue == 2024, "ProtoLite int");
    }
  }
  Check(count >= 2, "ProtoLite roundtrip OK");
}

void TestSessionManager() {
  EnsureTestSessionDir();
  agent::infra::SessionManager mgr(TestSessionDir());

  std::vector<agent::core::Message> msgs;
  agent::core::Message m;
  m.role = agent::core::MessageRole::User;
  m.content.push_back(agent::core::ContentBlock::MakeText("P3 session test"));
  msgs.push_back(m);

  mgr.SetMessages(msgs);
  mgr.PersistSnapshot();
  Check(true, "SessionManager persist OK");
}

void TestSessionManagerReadback() {
  EnsureTestSessionDir();
  agent::infra::SessionManager mgr(TestSessionDir());
  bool restored = mgr.RestoreFromDisk();
  Check(restored, "SessionManager restore OK");
}

void TestStabilityWatchdog() {
  agent::infra::StabilityConfig cfg;
  cfg.heartbeatTimeoutMs = 30000;
  cfg.maxConsecutiveFailures = 3;

  agent::infra::StabilityWatchdog watchdog(cfg);
  watchdog.Heartbeat();
  auto metrics = watchdog.metrics();
  Check(metrics.healthy, "Watchdog healthy after heartbeat");
}

void TestStabilityWatchdogCallbacks() {
  agent::infra::StabilityConfig cfg;
  agent::infra::StabilityWatchdog watchdog(cfg);

  bool snapshotCalled = false;
  watchdog.SetSnapshotCallback([&]() { snapshotCalled = true; });
  Check(!snapshotCalled, "Snapshot callback not yet invoked");

  bool recoveryCalled = false;
  watchdog.SetCrashRecoveryCallback([&]() {
    recoveryCalled = true;
    return true;
  });
  bool resourceOk = false;
  watchdog.SetResourceCheckCallback([&]() {
    resourceOk = true;
    return true;
  });
  Check(true, "Watchdog callbacks set");
}

}  // namespace

int main() {
  TestProcessRunner();
  TestProtoLiteEncodeDecode();
  TestSessionManager();
  TestSessionManagerReadback();
  TestStabilityWatchdog();
  TestStabilityWatchdogCallbacks();

  std::cout << "[test_infra] Failures: " << failures << std::endl;
  return failures > 0 ? 1 : 0;
}
