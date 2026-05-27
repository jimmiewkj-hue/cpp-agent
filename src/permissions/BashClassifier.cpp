#include "permissions/BashClassifier.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace agent {
namespace permissions {

const std::vector<std::string> BashClassifier::kReadOnlyCommands = {
    "ls", "cat", "head", "tail", "wc", "stat", "file",
    "echo", "printf", "date", "pwd", "whoami", "hostname",
    "env", "printenv", "which", "whereis", "type",
    "find", "grep", "egrep", "fgrep", "awk", "sed",
    "git log", "git status", "git diff", "git show",
    "git branch", "git tag", "git remote", "git config",
    "git stash list", "git ls-files", "git ls-tree",
    "python3 -c", "python -c", "node -e",
    "npm list", "npm view", "npm info",
};

const std::vector<std::string> BashClassifier::kDestructiveCommands = {
    "rm", "rmdir", "unlink", "dd",
    "mv", "cp -r", "cp -R",
    "git push", "git reset", "git clean",
    "chmod", "chown", "chgrp",
    "mkfs", "fdisk", "parted",
    "shutdown", "reboot", "halt",
    "kill", "killall", "pkill",
    "npm publish", "npm unpublish",
    "pip install", "pip uninstall",
};

const std::vector<std::string> BashClassifier::kReadOnlyPrefixes = {
    "ls ", "cat ", "head ", "tail ", "wc ", "stat ",
    "echo ", "printf ", "date ", "pwd", "whoami",
    "env ", "printenv ", "which ", "whereis ",
    "find ", "grep ", "git log", "git status",
    "git diff", "git show", "git branch",
    "npm list", "npm view", "npm info",
    "dir ", "type ", "tree ",
};

BashClassifier::BashClassifier() = default;

void BashClassifier::SetApiKey(const std::string& key) {
  apiKey_ = key;
}

bool BashClassifier::MatchesReadOnlyPattern(const std::string& command) const {
  std::string lower = command;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  lower.erase(lower.begin(),
              std::find_if(lower.begin(), lower.end(),
                           [](unsigned char ch) { return !std::isspace(ch); }));
  lower.erase(std::find_if(lower.rbegin(), lower.rend(),
                           [](unsigned char ch) { return !std::isspace(ch); })
                  .base(),
              lower.end());
  if (lower.empty()) return false;

  for (const auto& prefix : kReadOnlyPrefixes) {
    if (lower.find(prefix) == 0) return true;
  }

  for (const auto& destructive : kDestructiveCommands) {
    if (lower.find(destructive) == 0) return false;
  }

  std::istringstream stream(lower);
  std::string firstWord;
  stream >> firstWord;

  for (const auto& safe : kReadOnlyCommands) {
    if (firstWord == safe) return true;
    if (lower == safe) return true;
    if (lower.find(safe + " ") == 0) return true;
  }

  return false;
}

bool BashClassifier::IsReadOnlyCommand(const std::string& command) const {
  if (command.empty()) return false;
  return MatchesReadOnlyPattern(command);
}

std::string BashClassifier::BuildClassifierPrompt(
    const std::string& command,
    const std::vector<core::Message>& /*context*/) {
  std::ostringstream prompt;
  prompt << "You are a security classifier. Given a shell command, classify "
         << "it as SAFE or DANGEROUS.\n\n"
         << "Rules:\n"
         << "- SAFE commands: read-only operations (ls, cat, grep, find, "
         << "git log/status/diff, echo, etc.)\n"
         << "- DANGEROUS commands: file deletion (rm), destructive moves, "
         << "system modifications, data destruction\n"
         << "- UNCERTAIN commands: any command that could modify system state "
         << "should be classified as DANGEROUS\n\n"
         << "Response format:\n"
         << "<decision>SAFE</decision>\n"
         << "or\n"
         << "<decision>DANGEROUS</decision> <reason>...</reason>\n\n"
         << "Command to classify:\n```\n" << command << "\n```\n";

  return prompt.str();
}

BashSafetyDecision BashClassifier::Classify(
    const std::string& command,
    const std::vector<core::Message>& /*context*/) {
  BashSafetyDecision decision;

  if (command.empty()) {
    decision.allow = false;
    decision.reason = "empty command";
    return decision;
  }

  if (MatchesReadOnlyPattern(command)) {
    decision.allow = true;
    decision.reason = "read-only command pattern matched";
    return decision;
  }

  for (const auto& destructive : kDestructiveCommands) {
    std::string lower = command;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) {
                     return static_cast<char>(std::tolower(c));
                   });
    if (lower.find(destructive) == 0) {
      decision.allow = false;
      decision.reason = "matches destructive pattern: " + destructive;
      return decision;
    }
  }

  decision.allow = false;
  decision.reason = "command not in read-only allowlist";
  return decision;
}

}  // namespace permissions
}  // namespace agent
