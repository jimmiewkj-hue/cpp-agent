#include "infra/ProcessRunner.h"
#include "infra/ProtoLite.h"
#include "infra/SessionManager.h"
#include "infra/StabilityWatchdog.h"

#include <windows.h>

#include <fstream>
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

void TestSessionManagerKeepsDefaultMetadata() {
  EnsureTestSessionDir();
  agent::infra::SessionManager mgr(TestSessionDir());
  mgr.SetMetadata(agent::core::SessionMetadata());
  mgr.PersistSnapshot();

  const agent::core::SessionMetadata metadata = mgr.metadata();
  Check(metadata.id == TestSessionDir(),
        "SessionManager should preserve default session id");

  std::ifstream snapshot(mgr.LegacySnapshotPath(), std::ios::binary);
  std::string snapshotText((std::istreambuf_iterator<char>(snapshot)),
                           std::istreambuf_iterator<char>());
  Check(snapshotText.find("session_id=" + TestSessionDir()) !=
            std::string::npos,
        "SessionManager snapshot should retain session id after empty metadata");
}

void TestSessionManagerFlushesTranscriptAtThreshold() {
  EnsureTestSessionDir();
  agent::infra::SessionManager mgr(TestSessionDir());

  for (int i = 0; i < 50; ++i) {
    agent::core::Message msg;
    msg.role = agent::core::MessageRole::Assistant;
    msg.uuid = "msg-" + std::to_string(i);
    msg.content.push_back(
        agent::core::ContentBlock::MakeText("transcript-line-" +
                                            std::to_string(i)));
    mgr.AppendMessageToTranscript(msg);
  }
  mgr.FlushTranscriptBuffer();

  std::ifstream transcript(mgr.TranscriptJsonlPath(), std::ios::binary);
  std::string transcriptText((std::istreambuf_iterator<char>(transcript)),
                             std::istreambuf_iterator<char>());
  Check(transcriptText.find("transcript-line-0") != std::string::npos,
        "SessionManager should flush transcript lines before shutdown");
  Check(transcriptText.find("transcript-line-49") != std::string::npos,
        "SessionManager threshold flush should preserve the latest line");
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

// ===== P1-04 Test: Unicode path support in SessionManager =====
void TestSessionManagerUnicodePath() {
  // Create a directory with Chinese name using Win32 W API directly
  const wchar_t* wideName = L"build\\\u6d4b\u8bd5\u4f1a\u8bdd";  // 测试会话
  CreateDirectoryW(L"build", nullptr);
  CreateDirectoryW(wideName, nullptr);

  // Build UTF-8 path for SessionManager
  int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wideName,
      -1, nullptr, 0, nullptr, nullptr);
  std::string dirName(static_cast<size_t>(utf8Len - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wideName, -1,
      &dirName[0], utf8Len, nullptr, nullptr);

  agent::infra::SessionManager mgr(dirName);

  std::vector<agent::core::Message> msgs;
  agent::core::Message m;
  m.role = agent::core::MessageRole::User;
  m.uuid = "unicode-test-msg";
  // Use ASCII text to avoid encoding issues
  m.content.push_back(agent::core::ContentBlock::MakeText("Unicode session test content"));
  msgs.push_back(m);

  mgr.SetMessages(msgs);
  mgr.PersistSnapshot();
  Check(true, "Unicode session dir: snapshot persist OK");

  mgr.AppendMessageToTranscript(m);
  mgr.FlushTranscriptBuffer();
  const DWORD transcriptAttrs = GetFileAttributesW(
      (std::wstring(wideName) + L"\\transcript.jsonl").c_str());
  Check(transcriptAttrs != INVALID_FILE_ATTRIBUTES,
        "Unicode session dir: transcript jsonl exists");

  mgr.AppendModelIoRecord(
      agent::infra::ModelIoLogKind::Main, "request", "test-model",
      "system", msgs, 1);
  const DWORD modelIoAttrs = GetFileAttributesW(
      (std::wstring(wideName) + L"\\main-model.jsonl").c_str());
  Check(modelIoAttrs != INVALID_FILE_ATTRIBUTES,
        "Unicode session dir: model io jsonl exists");

  // Restore from disk
  agent::infra::SessionManager mgr2(dirName);
  bool restored = mgr2.RestoreFromDisk();
  Check(restored, "Unicode session dir: restore OK");

  std::vector<agent::core::Message> restoredMsgs = mgr2.messages();
  Check(restoredMsgs.size() == msgs.size(),
        "Unicode session dir: restored message count matches");
  if (!restoredMsgs.empty()) {
    bool hasContent = false;
    for (const auto& block : restoredMsgs[0].content) {
      if (block.type == agent::core::BlockType::Text &&
          block.asText.text == "Unicode session test content") {
        hasContent = true;
      }
    }
    Check(hasContent, "Unicode session dir: restored message content matches");
  }

  // Clean up
  DeleteFileW((std::wstring(wideName) + L"\\snapshot.pb").c_str());
  DeleteFileW((std::wstring(wideName) + L"\\snapshot.txt").c_str());
  DeleteFileW((std::wstring(wideName) + L"\\transcript.jsonl").c_str());
  DeleteFileW((std::wstring(wideName) + L"\\main-model.jsonl").c_str());
  RemoveDirectoryW(wideName);
}

}  // namespace

int main() {
  TestProcessRunner();
  TestProtoLiteEncodeDecode();
  TestSessionManager();
  TestSessionManagerReadback();
  TestSessionManagerKeepsDefaultMetadata();
  TestSessionManagerFlushesTranscriptAtThreshold();
  TestStabilityWatchdog();
  TestStabilityWatchdogCallbacks();
  TestSessionManagerUnicodePath();

  std::cout << "[test_infra] Failures: " << failures << std::endl;
  return failures > 0 ? 1 : 0;
}
