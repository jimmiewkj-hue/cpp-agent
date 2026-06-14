#pragma once

// Shell command normalization helpers for ToolOrchestrator.
// Extracted from ToolOrchestrator.cpp anonymous namespace to reduce
// monolithic file size (Task 7.2 of optimization plan).
//
// Converts Unix/Linux shell commands to PowerShell equivalents on Windows
// (e.g., && -> ;, ls -> Get-ChildItem, grep -> Select-String).

#include <string>
#include <vector>

namespace agent {
namespace tools {
namespace detail {

struct ShellToken {
  std::string text;
  bool wasQuoted = false;
};

// Tokenize a shell command string respecting single/double quotes and backslash escapes.
std::vector<ShellToken> TokenizeShellCommand(const std::string& command);

// Case-insensitive prefix check.
bool StartsWithCaseInsensitive(const std::string& value, const std::string& prefix);

// Quote a value for PowerShell single-quoted strings.
std::string QuoteForPowerShellSingleQuoted(const std::string& value);

// Normalize a Unix-style shell command to PowerShell equivalents on Windows.
// Handles: &&, |, /dev/null, ls, cat, grep, head, tail, rm, cp, mv, mkdir, etc.
std::string NormalizeWindowsShellCommand(const std::string& command);

}  // namespace detail
}  // namespace tools
}  // namespace agent
