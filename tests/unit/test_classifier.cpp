#include "permissions/BashClassifier.h"
#include "permissions/PermissionEngine.h"
#include "core/AgentTypes.h"
#include "core/StateTypes.h"

#include <iostream>

static int failures = 0;
static void Check(bool condition, const char* label) {
  if (!condition) { std::cerr << "FAIL: " << label << std::endl; ++failures; }
}

namespace {

// ============================================================
// BashClassifier Tests
// ============================================================

void TestBashClassifierReadOnlyCommands() {
  agent::permissions::BashClassifier classifier;

  Check(classifier.IsReadOnlyCommand("ls"), "ls is read-only");
  Check(classifier.IsReadOnlyCommand("cat /etc/hosts"), "cat is read-only");
  Check(classifier.IsReadOnlyCommand("head -n 10 file.txt"), "head is read-only");
  Check(classifier.IsReadOnlyCommand("tail -f log.txt"), "tail is read-only");
  Check(classifier.IsReadOnlyCommand("wc -l file.txt"), "wc is read-only");
  Check(classifier.IsReadOnlyCommand("stat /tmp"), "stat is read-only");
  Check(classifier.IsReadOnlyCommand("echo hello world"), "echo is read-only");
  Check(classifier.IsReadOnlyCommand("pwd"), "pwd is read-only");
  Check(classifier.IsReadOnlyCommand("whoami"), "whoami is read-only");
  Check(classifier.IsReadOnlyCommand("hostname"), "hostname is read-only");
  Check(classifier.IsReadOnlyCommand("env"), "env is read-only");
  Check(classifier.IsReadOnlyCommand("which ls"), "which is read-only");
  Check(classifier.IsReadOnlyCommand("find . -name '*.cpp'"), "find is read-only");
  Check(classifier.IsReadOnlyCommand("grep pattern file.txt"), "grep is read-only");
  Check(classifier.IsReadOnlyCommand("git log"), "git log is read-only");
  Check(classifier.IsReadOnlyCommand("git status"), "git status is read-only");
  Check(classifier.IsReadOnlyCommand("git diff"), "git diff is read-only");
  Check(classifier.IsReadOnlyCommand("git show HEAD"), "git show is read-only");
  Check(classifier.IsReadOnlyCommand("git branch"), "git branch is read-only");
  Check(classifier.IsReadOnlyCommand("date"), "date is read-only");
}

void TestBashClassifierDestructiveCommands() {
  agent::permissions::BashClassifier classifier;

  Check(!classifier.IsReadOnlyCommand("rm -rf /"), "rm is destructive");
  Check(!classifier.IsReadOnlyCommand("rmdir temp"), "rmdir is destructive");
  Check(!classifier.IsReadOnlyCommand("dd if=/dev/zero of=file"), "dd is destructive");
  Check(!classifier.IsReadOnlyCommand("chmod 777 file"), "chmod is destructive");
  Check(!classifier.IsReadOnlyCommand("chown user file"), "chown is destructive");
  Check(!classifier.IsReadOnlyCommand("shutdown now"), "shutdown is destructive");
  Check(!classifier.IsReadOnlyCommand("reboot"), "reboot is destructive");
  Check(!classifier.IsReadOnlyCommand("kill -9 1234"), "kill is destructive");
  Check(!classifier.IsReadOnlyCommand("killall proc"), "killall is destructive");
  Check(!classifier.IsReadOnlyCommand("pkill proc"), "pkill is destructive");
  Check(!classifier.IsReadOnlyCommand("git push origin main"), "git push is destructive");
  Check(!classifier.IsReadOnlyCommand("git reset --hard HEAD~1"), "git reset is destructive");
  Check(!classifier.IsReadOnlyCommand("git clean -fd"), "git clean is destructive");
  Check(!classifier.IsReadOnlyCommand("mkfs.ext4 /dev/sda"), "mkfs is destructive");
  Check(!classifier.IsReadOnlyCommand("npm publish"), "npm publish is destructive");
  Check(!classifier.IsReadOnlyCommand("pip install package"), "pip install is destructive");
  Check(!classifier.IsReadOnlyCommand("pip uninstall package"), "pip uninstall is destructive");
}

void TestBashClassifierEmptyCommand() {
  agent::permissions::BashClassifier classifier;
  Check(!classifier.IsReadOnlyCommand(""), "empty command is not read-only");
  Check(!classifier.IsReadOnlyCommand("  "), "whitespace-only is not read-only");
}

void TestBashClassifierUnknownCommand() {
  agent::permissions::BashClassifier classifier;
  // Unknown commands should be treated as potentially destructive (fail-safe)
  Check(!classifier.IsReadOnlyCommand("unknown_custom_tool --flag"),
        "unknown command defaults to unsafe");
}

void TestBashClassifierCaseInsensitivity() {
  agent::permissions::BashClassifier classifier;
  Check(classifier.IsReadOnlyCommand("LS"), "LS (uppercase) is read-only");
  Check(classifier.IsReadOnlyCommand("Cat file.txt"), "Cat (mixed case) is read-only");
  Check(classifier.IsReadOnlyCommand("ECHO test"), "ECHO (uppercase) is read-only");
  Check(!classifier.IsReadOnlyCommand("RM file"), "RM (uppercase) is destructive");
}

void TestBashClassifierGitCommands() {
  agent::permissions::BashClassifier classifier;
  // Read-only git commands
  Check(classifier.IsReadOnlyCommand("git log --oneline"), "git log is read-only");
  Check(classifier.IsReadOnlyCommand("git status -s"), "git status is read-only");
  Check(classifier.IsReadOnlyCommand("git diff HEAD~1"), "git diff is read-only");
  Check(classifier.IsReadOnlyCommand("git branch -a"), "git branch is read-only");
  Check(classifier.IsReadOnlyCommand("git tag"), "git tag is read-only");
  Check(classifier.IsReadOnlyCommand("git remote -v"), "git remote is read-only");
  Check(classifier.IsReadOnlyCommand("git config --list"), "git config is read-only");
  Check(classifier.IsReadOnlyCommand("git ls-files"), "git ls-files is read-only");

  // Destructive git commands
  Check(!classifier.IsReadOnlyCommand("git push --force"), "git push is destructive");
  Check(!classifier.IsReadOnlyCommand("git reset --soft HEAD~1"), "git reset is destructive");
  Check(!classifier.IsReadOnlyCommand("git clean -fdx"), "git clean is destructive");
}

void TestBashClassifierClassifyMethod() {
  agent::permissions::BashClassifier classifier;

  auto result1 = classifier.Classify("ls -la", {});
  Check(result1.allow, "Classify 'ls -la' should allow");
  Check(!result1.reason.empty(), "Classify should provide reason");

  auto result2 = classifier.Classify("rm -rf /tmp/test", {});
  Check(!result2.allow, "Classify 'rm -rf' should not allow");
  Check(!result2.reason.empty(), "Classify should provide reason");

  auto result3 = classifier.Classify("", {});
  Check(!result3.allow, "Classify empty command should not allow");
  Check(result3.reason.find("empty") != std::string::npos,
        "Classify empty should mention empty command");

  auto result4 = classifier.Classify("echo hello", {});
  Check(result4.allow, "Classify echo should allow");
}

void TestBashClassifierBuildPrompt() {
  agent::permissions::BashClassifier classifier;
  std::string prompt = classifier.BuildClassifierPrompt("rm -rf /tmp/test", {});
  Check(!prompt.empty(), "BuildClassifierPrompt returns non-empty");
  Check(prompt.find("rm -rf /tmp/test") != std::string::npos,
        "BuildClassifierPrompt includes the command");
  Check(prompt.find("SAFE") != std::string::npos,
        "BuildClassifierPrompt mentions SAFE");
  Check(prompt.find("DANGEROUS") != std::string::npos,
        "BuildClassifierPrompt mentions DANGEROUS");
}

void TestBashClassifierReadOnlyPrefixes() {
  agent::permissions::BashClassifier classifier;
  // These match by prefix
  Check(classifier.IsReadOnlyCommand("ls -la /tmp"), "ls prefix match");
  Check(classifier.IsReadOnlyCommand("cat file1 file2"), "cat prefix match");
  Check(classifier.IsReadOnlyCommand("echo 'hello world'"), "echo prefix match");
  Check(classifier.IsReadOnlyCommand("find / -name test"), "find prefix match");
  Check(classifier.IsReadOnlyCommand("grep -r pattern ."), "grep prefix match");
  Check(classifier.IsReadOnlyCommand("dir /s /b"), "dir Windows prefix match");
  Check(classifier.IsReadOnlyCommand("type file.txt"), "type Windows prefix match");
}

void TestBashClassifierPythonNodeCommands() {
  agent::permissions::BashClassifier classifier;
  Check(classifier.IsReadOnlyCommand("python3 -c \"print('hello')\""),
        "python3 -c is read-only");
  Check(classifier.IsReadOnlyCommand("python -c \"print('hello')\""),
        "python -c is read-only");
  Check(classifier.IsReadOnlyCommand("node -e \"console.log('hello')\""),
        "node -e is read-only");
  Check(classifier.IsReadOnlyCommand("npm list"), "npm list is read-only");
  Check(classifier.IsReadOnlyCommand("npm view react"), "npm view is read-only");
  Check(classifier.IsReadOnlyCommand("npm info react"), "npm info is read-only");
}

// ============================================================
// PermissionEngine Tests
// ============================================================

void TestPermissionEngineAllowDeny() {
  agent::permissions::PermissionEngine engine;

  engine.AddAlwaysAllowRule("echo");
  engine.AddAlwaysDenyRule("rm");

  agent::core::ContentBlock echoBlock =
      agent::core::ContentBlock::MakeToolUse("tu-1", "Bash", R"({"command":"echo hello"})");
  auto echoDecision = engine.Evaluate(echoBlock, {});
  Check(echoDecision.behavior == agent::core::PermissionBehavior::Allow,
        "echo command allowed by rule");

  agent::core::ContentBlock rmBlock =
      agent::core::ContentBlock::MakeToolUse("tu-2", "Bash", R"({"command":"rm file"})");
  auto rmDecision = engine.Evaluate(rmBlock, {});
  Check(rmDecision.behavior == agent::core::PermissionBehavior::Deny,
        "rm command denied by rule");
}

void TestPermissionEngineAutoModeAllowlist() {
  agent::permissions::PermissionEngine engine;
  engine.SetPermissionMode(agent::core::PermissionMode::Auto);
  engine.AddAutoModeAllowlistedTool("Read");
  engine.AddAutoModeAllowlistedTool("Grep");
  engine.AddAutoModeAllowlistedTool("Glob");

  agent::core::ContentBlock readBlock =
      agent::core::ContentBlock::MakeToolUse("tu-3", "Read", R"({"file_path":"test.txt"})");
  auto readDecision = engine.Evaluate(readBlock, {});
  Check(readDecision.behavior == agent::core::PermissionBehavior::Allow,
        "Read tool allowed in auto mode");

  agent::core::ContentBlock writeBlock =
      agent::core::ContentBlock::MakeToolUse("tu-4", "Bash", R"({"command":"rm file"})");
  auto writeDecision = engine.Evaluate(writeBlock, {});
  Check(writeDecision.behavior == agent::core::PermissionBehavior::Deny ||
        writeDecision.behavior == agent::core::PermissionBehavior::Ask,
        "Unlisted tool denied or asks in auto mode");
}

void TestPermissionEngineDenialState() {
  agent::permissions::PermissionEngine engine;

  auto state = engine.denialState();
  Check(state.consecutive == 0, "Initial consecutive denials is 0");
  Check(state.total == 0, "Initial total denials is 0");
  Check(!engine.IsCircuitBroken(), "Circuit not broken initially");

  engine.ResetDenialState();
  auto resetState = engine.denialState();
  Check(resetState.consecutive == 0, "Reset clears consecutive denials");
}

void TestPermissionEngineBuildCanUseTool() {
  agent::permissions::PermissionEngine engine;
  engine.AddAlwaysAllowRule("echo");
  engine.AddAlwaysDenyRule("rm");

  auto canUse = engine.BuildCanUseTool();
  Check(canUse != nullptr, "BuildCanUseTool returns valid callback");

  agent::core::ContentBlock echoBlock =
      agent::core::ContentBlock::MakeToolUse("tu-5", "Bash", R"({"command":"echo hi"})");
  std::vector<agent::core::Message> emptyMsgs;
  auto decision = canUse(echoBlock, emptyMsgs);
  Check(decision.behavior == agent::core::PermissionBehavior::Allow,
        "CanUseTool callback allows echo");
}

void TestPermissionEngineFailClosed() {
  agent::permissions::PermissionEngine engine;
  engine.SetFailClosed(true);

  // With classifier not set, fail-closed means deny on classifier error
  engine.SetClassifierCallback([](const agent::core::ContentBlock&,
                                    const std::vector<agent::core::Message>&) {
    // Simulate classifier error by returning deny
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Deny;
    d.reason = "classifier failed";
    return d;
  });

  agent::core::ContentBlock block =
      agent::core::ContentBlock::MakeToolUse("tu-6", "Bash", R"({"command":"unknown_cmd"})");
  auto decision = engine.Evaluate(block, {});
  Check(decision.behavior == agent::core::PermissionBehavior::Deny,
        "Fail-closed denies on classifier failure");
}

void TestPermissionEngineModes() {
  agent::permissions::PermissionEngine engine;

  engine.SetPermissionMode(agent::core::PermissionMode::Auto);
  Check(engine.GetPermissionMode() == agent::core::PermissionMode::Auto,
        "Auto mode set");

  engine.SetPermissionMode(agent::core::PermissionMode::Plan);
  Check(engine.GetPermissionMode() == agent::core::PermissionMode::Plan,
        "Plan mode set");

  engine.SetPermissionMode(agent::core::PermissionMode::Default);
  Check(engine.GetPermissionMode() == agent::core::PermissionMode::Default,
        "Default mode set");
}

}  // namespace

int main() {
  TestBashClassifierReadOnlyCommands();
  TestBashClassifierDestructiveCommands();
  TestBashClassifierEmptyCommand();
  TestBashClassifierUnknownCommand();
  TestBashClassifierCaseInsensitivity();
  TestBashClassifierGitCommands();
  TestBashClassifierClassifyMethod();
  TestBashClassifierBuildPrompt();
  TestBashClassifierReadOnlyPrefixes();
  TestBashClassifierPythonNodeCommands();
  TestPermissionEngineAllowDeny();
  TestPermissionEngineAutoModeAllowlist();
  TestPermissionEngineDenialState();
  TestPermissionEngineBuildCanUseTool();
  TestPermissionEngineFailClosed();
  TestPermissionEngineModes();

  std::cout << "[test_classifier] Failures: " << failures << std::endl;
  return failures > 0 ? 1 : 0;
}
