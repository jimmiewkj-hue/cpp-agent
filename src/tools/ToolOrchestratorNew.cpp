#include "tools/ToolOrchestrator.h"
#include "hooks/HookExecutor.h"
#include "infra/Logger.h"
#include "infra/StringUtil.h"
#include "infra/ThreadPool.h"
#include "tools/BashHelpers.h"
#include "tools/FileHelpers.h"

#include <cstdlib>  // std::getenv, std::atoi

#include "agents/SubAgentManager.h"
#include "mcp/McpClientManager.h"
#include "tools/ToolRegistry.h"
#include "third_party/nlohmann_json.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>   // std::memcmp
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

using json = nlohmann::json;

namespace agent {
namespace tools {

namespace {

static const int kDefaultMaxResultChars = 100000;
static const int kMaxToolResultTruncation = 400000;
static const long long kMaxFullReadBytes = 256 * 1024;
static const wchar_t* kWebUserAgent = L"cpp-agent/1.0";

// ParsedUrl and HttpResponse are now in FileHelpers.h (detail namespace)
using detail::ParsedUrl;
using detail::HttpResponse;

// ShellToken, TokenizeShellCommand, NormalizeWindowsShellCommand,
// QuoteForPowerShellSingleQuoted, StartsWithCaseInsensitive
// are now in tools/BashHelpers.h (namespace agent::tools::detail)
using detail::ShellToken;
using detail::NormalizeWindowsShellCommand;
using detail::QuoteForPowerShellSingleQuoted;

std::wstring ToWide(const std::string& text) {
  return infra::Utf8ToWide(text);
}

std::string ToUtf8(const std::wstring& text) {
  return infra::WideToUtf8(text);
}

std::string Trim(const std::string& value) {
  return infra::Trim(value);
}

} // anonymous namespace

// File/path/glob/search/HTML/fuzzy-match helpers extracted to FileHelpers.h
using detail::ParentPath;
using detail::JoinPath;
using detail::IsAbsolutePath;
using detail::NormalizeSeparators;
using detail::ToLowerAscii;
using detail::EnsureDirectoryRecursive;
using detail::GetFullPathString;
using detail::EnsureTrailingSeparator;
using detail::IsPathWithinWorkspace;
using detail::ResolveToolPath;
using detail::ReadFileContent;
using detail::WriteFileContent;
using detail::FileEntry;
using detail::GlobFiles;
using detail::WildcardMatch;
using detail::GrepFile;
using detail::GrepDirectory;
using detail::StartsWithNoCase;
using detail::ReplaceAll;
using detail::StripTags;
using detail::HtmlToText;
using detail::DesanitizeXmlEntities;
using detail::IsMarkdownFile;
using detail::NormalizeCRLF;
using detail::NormalizeQuotes;
using detail::StripTrailingWhitespace;
using detail::MapNormalizedMatchToOriginal;
using detail::PreserveQuoteStyle;
using detail::NORM_CRLF;
using detail::NORM_QUOTES;
using detail::NORM_WS;

// JSON, task store, HTTP, and line-ending helpers
using detail::JsonGetString;
using detail::JsonGetStringMultiKey;
using detail::JsonGetBool;
using detail::JsonGetInt;
using detail::NormalizeLineEndings;
using detail::GetStateRootForTools;
using detail::GetTaskStorePath;
using detail::LoadTaskStore;
using detail::SaveTaskStore;
using detail::FindTaskIndex;
using detail::NextTaskId;
using detail::RenderTaskSummary;
using detail::UrlEncode;
using detail::BuildMarkdownFromHtml;
using detail::HttpGet;
using detail::ParseSearchResults;
using detail::ExtractHtmlTitle;
using detail::json;


// P0-03: Case-insensitive comparison for tool name matching (moved to agent::tools scope)
static bool CaseInsensitiveCompare(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}


ToolOrchestrator::ToolOrchestrator() {
  // P0-03: Read Bash timeout from environment variable (aligned with local-ace).
  const char* envTimeout = std::getenv("AGENT_BASH_TIMEOUT_MS");
  if (envTimeout) {
    int val = std::atoi(envTimeout);
    if (val > 0) bashTimeoutMs_ = val;
  }
}

void ToolOrchestrator::SetBashTimeoutMs(int timeoutMs) {
  if (timeoutMs > 0) bashTimeoutMs_ = timeoutMs;
}

void ToolOrchestrator::SetToolRegistry(const ToolRegistry* registry) {
  toolRegistry_ = registry;
}

void ToolOrchestrator::SetSubAgentManager(
    agents::SubAgentManager* subAgentManager) {
  subAgentManager_ = subAgentManager;
}

void ToolOrchestrator::SetMcpClientManager(
    mcp::McpClientManager* mcpClientManager) {
  mcpClientManager_ = mcpClientManager;
}

void ToolOrchestrator::SetWorkspaceRoot(const std::string& workspaceRoot) {
  workspaceRoot_ = GetFullPathString(workspaceRoot);
}

void ToolOrchestrator::SetHookExecutor(hooks::HookExecutor* hookExecutor) {
  hookExecutor_ = hookExecutor;
}

void ToolOrchestrator::SetToolCompletionCallback(ToolCompletionCallback cb) {
  toolCompletionCallback_ = std::move(cb);
}

std::vector<ToolBatch> ToolOrchestrator::PartitionToolCalls(
    const std::vector<core::ContentBlock>& toolUseBlocks) const {
  std::vector<ToolBatch> batches;

  for (const auto& block : toolUseBlocks) {
    bool concurrentSafe = false;
    if (toolRegistry_) {
      concurrentSafe =
          toolRegistry_->IsConcurrencySafe(block.asToolUse.name);
    }

    if (!batches.empty() && concurrentSafe && batches.back().concurrentSafe) {
      batches.back().blocks.push_back(block);
      continue;
    }

    ToolBatch batch;
    batch.concurrentSafe = concurrentSafe;
    batch.blocks.push_back(block);
    batches.push_back(batch);
  }

  return batches;
}

std::string ToolOrchestrator::TruncateResult(const std::string& result,
                                             int maxSize) {
  if (maxSize <= 0 || static_cast<int>(result.size()) <= maxSize) {
    return result;
  }

  std::size_t cutAt = static_cast<std::size_t>(maxSize);
  std::size_t lastNewline = result.find_last_of('\n', cutAt);
  if (lastNewline != std::string::npos && lastNewline > cutAt / 2) {
    cutAt = lastNewline;
  }

  std::ostringstream truncated;
  truncated << result.substr(0, cutAt);
  truncated << "\n... [truncated " << (result.size() - cutAt)
            << " bytes]";
  return truncated.str();
}

std::string ToolOrchestrator::ExecuteToolBlock(
    const core::ContentBlock& block,
    int maxResultSize,
    std::string* error) const {
  const std::string& name = block.asToolUse.name;
  const std::string& inputJson = block.asToolUse.inputJson;

  const auto CanonicalizeToolAlias = [](const std::string& toolName) {
    if (CaseInsensitiveCompare(toolName, "write_file") ||
        CaseInsensitiveCompare(toolName, "file_write")) {
      return std::string("Write");
    }
    if (CaseInsensitiveCompare(toolName, "read_file") ||
        CaseInsensitiveCompare(toolName, "file_read")) {
      return std::string("Read");
    }
    return toolName;
  };

  const std::string resolvedName = CanonicalizeToolAlias(name);

  if (CaseInsensitiveCompare(resolvedName, "Bash")) {
    return ExecuteBash(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "FileRead") ||
      CaseInsensitiveCompare(resolvedName, "Read")) {
    return ExecuteFileRead(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "FileWrite") ||
      CaseInsensitiveCompare(resolvedName, "Write")) {
    return ExecuteFileWrite(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "Grep")) {
    return ExecuteGrep(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "Glob")) {
    return ExecuteGlob(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "Agent")) {
    return ExecuteAgent(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "TodoWrite")) {
    return ExecuteTodoWrite(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "TaskCreate")) {
    return ExecuteTaskCreate(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "TaskGet")) {
    return ExecuteTaskGet(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "TaskUpdate")) {
    return ExecuteTaskUpdate(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "TaskList")) {
    return ExecuteTaskList(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "TaskStop")) {
    return ExecuteTaskStop(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "AskUserQuestion")) {
    return ExecuteAskUserQuestion(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "FileEdit")) {
    return ExecuteFileEdit(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "NotebookEdit")) {
    return ExecuteNotebookEdit(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "Skill")) {
    return ExecuteSkill(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "ListMcpResources")) {
    return ExecuteListMcpResources(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "ReadMcpResource")) {
    return ExecuteReadMcpResource(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "WebFetch")) {
    return ExecuteWebFetch(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(resolvedName, "WebSearch")) {
    return ExecuteWebSearch(inputJson, maxResultSize, error);
  }

  if (error) {
    *error = "unknown tool: " + resolvedName;
  }
  return std::string();
}

std::string ToolOrchestrator::ExecuteBash(const std::string& inputJson,
                                          int maxResultSize,
                                          std::string* error) const {
  std::string command = JsonGetStringMultiKey(inputJson, {"command", "cmd"});
  if (command.empty()) {
    if (error) *error = "Bash tool requires 'command' parameter";
    return std::string();
  }

  // Parse optional per-call timeout (ms) from LLM input, aligned with local-ace.
  // Falls back to the global bashTimeoutMs_ if not specified.
  int callTimeoutMs = JsonGetInt(inputJson, "timeout", 0);
  if (callTimeoutMs <= 0) {
    // Also try "is_background" boolean: background commands get a longer default timeout
    callTimeoutMs = bashTimeoutMs_;
  } else {
    // Clamp to [5s, 600s] to prevent abuse
    if (callTimeoutMs < 5000) callTimeoutMs = 5000;
    if (callTimeoutMs > 600000) callTimeoutMs = 600000;
  }

  const std::string normalizedCommand = NormalizeWindowsShellCommand(command);

  infra::ProcessRunOptions options;
  options.executable = "powershell.exe";
  options.arguments = {"-NoProfile", "-Command", normalizedCommand};
  if (!workspaceRoot_.empty()) {
    options.workingDirectory = workspaceRoot_;
  }
  options.timeoutMs = static_cast<unsigned long>(callTimeoutMs);

  infra::ProcessRunResult result = processRunner_.Run(options);

  std::ostringstream output;
  if (normalizedCommand != command) {
    output << "[normalized command] " << normalizedCommand << "\n";
  }
  if (result.spawnFailed) {
    if (error) *error = result.errorMessage;
    output << "Error: " << result.errorMessage;
  } else if (result.timedOut) {
    if (error) *error = "command timed out after " + std::to_string(callTimeoutMs / 1000) + "s";
    output << "Error: command timed out after " << (callTimeoutMs / 1000) << "s\n";
    output << result.stdoutText;
  } else {
    output << result.stdoutText;
    if (!result.stderrText.empty()) {
      if (!result.stdoutText.empty() && result.stdoutText.back() != '\n') {
        output << "\n";
      }
      output << result.stderrText;
    }
    if (result.exitCode != 0) {
      if (error) {
        *error = "command exited with code " + std::to_string(result.exitCode);
      }
      output << "\n[exit code: " << result.exitCode << "]";
    }
  }

  std::string finalOutput = output.str();

  // GEMMA-ENHANCE: Auto-append API discovery hints when Python library errors
  // are detected in Bash output. This gives the model immediate guidance to
  // check the library version and discover actual API signatures, instead of
  // blindly guessing parameter names (a major defect observed in Gemma-4-31B
  // logs with pyecharts).
  if (result.exitCode != 0 || !result.stderrText.empty()) {
    std::string lowerOutput = finalOutput;
    std::transform(lowerOutput.begin(), lowerOutput.end(),
                   lowerOutput.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    bool isPythonApiError = false;
    std::string libraryHint;
    if (lowerOutput.find("typeerror") != std::string::npos &&
        (lowerOutput.find("unexpected keyword argument") != std::string::npos ||
         lowerOutput.find("got an unexpected") != std::string::npos ||
         lowerOutput.find("takes") != std::string::npos)) {
      isPythonApiError = true;
      libraryHint = "TypeError with unexpected keyword argument";
    } else if (lowerOutput.find("attributeerror") != std::string::npos &&
               (lowerOutput.find("has no attribute") != std::string::npos ||
                lowerOutput.find("module") != std::string::npos)) {
      isPythonApiError = true;
      libraryHint = "AttributeError (missing method/attribute)";
    }
    if (isPythonApiError) {
      finalOutput +=
          "\n\n[API Discovery Hint] The error (" + libraryHint +
          ") suggests a library API version mismatch. Before fixing:\n"
          "1. Run: pip show <library_name>   (check installed version)\n"
          "2. Run: python -c \"import <lib>; help(<lib>.<class>)\" "
          "(discover actual API)\n"
          "3. READ the library source file to find correct parameter names\n"
          "Do NOT guess parameter names from memory.";
    }
  }

  return TruncateResult(finalOutput, maxResultSize > 0 ? maxResultSize
                                                        : kMaxToolResultTruncation);
}

std::string ToolOrchestrator::ExecuteFileRead(const std::string& inputJson,
                                              int maxResultSize,
                                              std::string* error) const {
  const std::string rawPath =
      JsonGetStringMultiKey(inputJson, {"file_path", "path"});
  if (rawPath.empty()) {
    if (error) *error = "FileRead tool requires 'file_path' parameter";
    return std::string();
  }

  std::string resolveError;
  const std::string filePath =
      ResolveToolPath(rawPath, workspaceRoot_, false, &resolveError);
  if (filePath.empty()) {
    if (error) *error = resolveError;
    return "Error: " + resolveError;
  }

  DWORD attrs = GetFileAttributesW(ToWide(filePath).c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    if (error) *error = "file not found: " + filePath;
    return "Error: file not found: " + filePath;
  }

  if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
    std::ostringstream listing;
    auto entries = GlobFiles(filePath, "*");
    std::sort(entries.begin(), entries.end(),
              [](const FileEntry& a, const FileEntry& b) {
                if (a.isDirectory != b.isDirectory)
                  return a.isDirectory;
                return a.name < b.name;
              });
    listing << "Directory listing for " << filePath << ":\n";
    for (const auto& entry : entries) {
      listing << (entry.isDirectory ? "[DIR]  " : "[FILE] ")
              << entry.name;
      if (!entry.isDirectory) {
        listing << " (" << entry.size << " bytes)";
      }
      listing << "\n";
    }
    return TruncateResult(listing.str(), maxResultSize);
  }

  const int offset = JsonGetInt(inputJson, "offset", 0);
  const int limit = JsonGetInt(inputJson, "limit", 0);
  if (offset < 0) {
    if (error) *error = "Read tool offset must be >= 0";
    return "Error: Read tool offset must be >= 0";
  }
  if (limit < 0) {
    if (error) *error = "Read tool limit must be >= 0";
    return "Error: Read tool limit must be >= 0";
  }
  if (offset == 0 && limit == 0 && attrs != INVALID_FILE_ATTRIBUTES) {
    LARGE_INTEGER size;
    size.QuadPart = 0;
    HANDLE sizeHandle = CreateFileW(ToWide(filePath).c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE |
                                        FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (sizeHandle != INVALID_HANDLE_VALUE) {
      if (GetFileSizeEx(sizeHandle, &size) && size.QuadPart > kMaxFullReadBytes) {
        CloseHandle(sizeHandle);
        std::ostringstream oversized;
        oversized << "Error: file too large to read in full (" << size.QuadPart
                  << " bytes): " << filePath
                  << ". Use Read with offset/limit to inspect a targeted line "
                     "range, or use Grep to search first.";
        if (error) *error = oversized.str();
        return oversized.str();
      }
      CloseHandle(sizeHandle);
    }
  }

  std::string readErr;
  std::string content = ReadFileContent(filePath, &readErr);
  if (content.empty() && !readErr.empty()) {
    if (error) *error = readErr;
    return "Error: " + readErr;
  }

  const std::string normalized = NormalizeLineEndings(content);
  if (offset > 0 || limit > 0) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= normalized.size()) {
      const std::size_t next = normalized.find('\n', start);
      if (next == std::string::npos) {
        lines.push_back(normalized.substr(start));
        break;
      }
      lines.push_back(normalized.substr(start, next - start));
      start = next + 1;
      if (start == normalized.size()) {
        lines.push_back(std::string());
        break;
      }
    }

    const int startLine = std::max(1, offset == 0 ? 1 : offset);
    const int lastLine = limit > 0
                             ? std::min(static_cast<int>(lines.size()),
                                        startLine + limit - 1)
                             : static_cast<int>(lines.size());

    std::ostringstream output;
    output << "<file path=\"" << filePath << "\"";
    output << " start_line=\"" << startLine << "\"";
    if (limit > 0) {
      output << " line_count=\"" << std::max(0, lastLine - startLine + 1)
             << "\"";
    }
    output << ">\n";
    if (startLine > static_cast<int>(lines.size())) {
      output << "(requested range is beyond end of file)\n";
    } else {
      for (int line = startLine; line <= lastLine; ++line) {
        output << line << "->" << lines[static_cast<std::size_t>(line - 1)]
               << "\n";
      }
    }
    output << "</file>";
    return TruncateResult(output.str(), maxResultSize);
  }

  std::ostringstream output;
  output << "<file path=\"" << filePath << "\">\n";
  output << normalized;
  if (!content.empty() && content.back() != '\n') {
    output << "\n";
  }
  output << "</file>";

  return TruncateResult(output.str(), maxResultSize);
}

std::string ToolOrchestrator::ExecuteFileWrite(const std::string& inputJson,
                                               int maxResultSize,
                                               std::string* error) const {
  const std::string rawPath =
      JsonGetStringMultiKey(inputJson, {"file_path", "path"});
  std::string content = JsonGetString(inputJson, "content");

  if (rawPath.empty()) {
    if (error) *error = "FileWrite tool requires 'file_path' parameter";
    return std::string();
  }
  if (content.empty()) {
    if (error) *error = "FileWrite tool requires 'content' parameter";
    return std::string();
  }

  std::string resolveError;
  const std::string filePath =
      ResolveToolPath(rawPath, workspaceRoot_, true, &resolveError);
  if (filePath.empty()) {
    if (error) *error = resolveError;
    return "Error: " + resolveError;
  }

  DWORD attrs = GetFileAttributesW(ToWide(filePath).c_str());
  bool existed = (attrs != INVALID_FILE_ATTRIBUTES);

  std::string writeErr;
  if (!WriteFileContent(filePath, content, &writeErr)) {
    if (error) *error = writeErr;
    return "Error: " + writeErr;
  }

  std::ostringstream output;
  output << (existed ? "Updated" : "Created") << " file: " << filePath
         << "\nWrote " << content.size() << " bytes.";
  return TruncateResult(output.str(), maxResultSize);
}

std::string ToolOrchestrator::ExecuteGrep(const std::string& inputJson,
                                          int maxResultSize,
                                          std::string* error) const {
  std::string pattern = JsonGetStringMultiKey(inputJson, {"pattern", "query"});
  std::string searchPath = JsonGetStringMultiKey(inputJson, {"path", "directory"});
  if (searchPath.empty()) {
    searchPath = workspaceRoot_.empty() ? "." : workspaceRoot_;
  } else {
    std::string resolveError;
    const std::string resolved =
        ResolveToolPath(searchPath, workspaceRoot_, false, &resolveError);
    if (resolved.empty()) {
      if (error) *error = resolveError;
      return "Error: " + resolveError;
    }
    searchPath = resolved;
  }

  if (pattern.empty()) {
    if (error) *error = "Grep tool requires 'pattern' parameter";
    return std::string();
  }

  const int kMaxGrepMatches = 100;
  std::ostringstream output;
  int matchCount = 0;

  DWORD attrs = GetFileAttributesW(ToWide(searchPath).c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    if (error) *error = "path not found: " + searchPath;
    return "Error: path not found: " + searchPath;
  }

  if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
    GrepDirectory(searchPath, pattern, kMaxGrepMatches, &matchCount, &output);
  } else {
    output << GrepFile(searchPath, pattern, kMaxGrepMatches);
  }

  std::string result = output.str();
  if (result.empty()) {
    result = "No matches found for pattern: " + pattern;
  }
  return TruncateResult(result, maxResultSize);
}

std::string ToolOrchestrator::ExecuteGlob(const std::string& inputJson,
                                          int maxResultSize,
                                          std::string* error) const {
  std::string pattern = JsonGetStringMultiKey(inputJson, {"pattern", "glob"});
  std::string directory = JsonGetString(inputJson, "path");
  if (directory.empty()) {
    directory = workspaceRoot_.empty() ? "." : workspaceRoot_;
  } else {
    std::string resolveError;
    const std::string resolved =
        ResolveToolPath(directory, workspaceRoot_, false, &resolveError);
    if (resolved.empty()) {
      if (error) *error = resolveError;
      return "Error: " + resolveError;
    }
    directory = resolved;
  }

  if (pattern.empty()) {
    if (error) *error = "Glob tool requires 'pattern' parameter";
    return std::string();
  }

  auto entries = GlobFiles(directory, pattern);
  std::sort(entries.begin(), entries.end(),
            [](const FileEntry& a, const FileEntry& b) {
              if (a.isDirectory != b.isDirectory)
                return a.isDirectory;
              return a.name < b.name;
            });

  std::ostringstream output;
  output << "Found " << entries.size() << " files matching '"
         << pattern << "' in " << directory << ":\n";
  for (const auto& entry : entries) {
    output << (entry.isDirectory ? "[DIR]  " : "[FILE] ")
           << entry.name;
    if (!entry.isDirectory) {
      output << " (" << entry.size << " bytes)";
    }
    output << "\n";
  }

  if (entries.empty()) {
    output << "(no matches)";
  }
  return TruncateResult(output.str(), maxResultSize);
}

std::string ToolOrchestrator::ExecuteAgent(const std::string& inputJson,
                                           int maxResultSize,
                                           std::string* error) const {
  if (!subAgentManager_) {
    if (error) *error = "SubAgentManager not set on ToolOrchestrator";
    return "Error: Agent tool requires SubAgentManager configuration";
  }

  std::string prompt = JsonGetStringMultiKey(inputJson, {"prompt", "description"});
  std::string subagentType = JsonGetString(inputJson, "subagent_type");
  std::string isolation = JsonGetString(inputJson, "isolation");

  bool runInBackground = JsonGetBool(inputJson, "run_in_background");

  if (prompt.empty()) {
    if (error) *error = "Agent tool requires 'prompt' parameter";
    return "Error: missing prompt";
  }

  std::string result =
      subAgentManager_->RunAsyncAgentLifecycle(
          prompt, inputJson, subagentType, runInBackground,
          isolation, {});

  return TruncateResult(result, maxResultSize > 0 ? maxResultSize : 400000);
}

ToolOrchestrator::ExecuteResult ToolOrchestrator::Execute(
    const std::vector<core::ContentBlock>& toolUseBlocks,
    core::CanUseTool canUseTool,
    const std::vector<core::Message>& messages) const {
  ExecuteResult result;
  const std::vector<ToolBatch> batches = PartitionToolCalls(toolUseBlocks);
  LOG_INFO(TOOL, "Execute tools",
           {{"toolBlocks", std::to_string(toolUseBlocks.size())},
            {"batches", std::to_string(batches.size())}});

  std::vector<core::Message> accumulatedMessages = messages;

  for (const auto& batch : batches) {
    // Parallel execution for concurrent-safe batches with multiple tools.
    if (batch.concurrentSafe && batch.blocks.size() > 1) {
      LOG_INFO(TOOL, "Parallel batch", {{"tools", std::to_string(batch.blocks.size())}});
      struct ToolResult_ {
        core::Message message;
        bool isError = false;
        bool denied = false;
        std::string toolName;
        std::string inputJson;
        std::string toolId;
        std::string error;
      };

      std::vector<std::future<ToolResult_>> futures;
      futures.reserve(batch.blocks.size());

      for (const auto& block : batch.blocks) {
        // Permission check on main thread (may involve UI).
        core::PermissionDecision decision = canUseTool(block, messages);
        if (decision.behavior != core::PermissionBehavior::Allow) {
          core::Message msg;
          msg.role = core::MessageRole::User;
          std::string reason = (decision.behavior == core::PermissionBehavior::Deny)
              ? "Tool denied: " + decision.reason
              : "Tool requires confirmation in non-interactive skeleton mode: " + decision.reason;
          msg.content.push_back(core::ContentBlock::MakeToolResult(
              block.asToolUse.id, reason, true));
          result.userMessages.push_back(msg);
          result.deniedCount++;
          if (decision.behavior == core::PermissionBehavior::Ask) result.errorCount++;
          accumulatedMessages.push_back(msg);
          continue;
        }

        // PreToolUse hooks: run before tool execution (aligned with local-ace)
        if (hookExecutor_) {
          auto preBatch = hookExecutor_->RunPreToolUseHooks(
              block.asToolUse.name, block.asToolUse.inputJson,
              block.asToolUse.id, 30000);
          bool preBlocked = false;
          for (const auto& hr : preBatch.results) {
            if (hr.outcome == hooks::HookOutcome::Blocking ||
                hr.decision == "deny") {
              preBlocked = true;
              core::Message denyMsg;
              denyMsg.role = core::MessageRole::User;
              std::string denyReason = hr.reason.empty()
                  ? ("PreToolUse hook blocked: " + block.asToolUse.name)
                  : hr.reason;
              denyMsg.content.push_back(core::ContentBlock::MakeToolResult(
                  block.asToolUse.id, denyReason, true));
              result.userMessages.push_back(denyMsg);
              result.deniedCount++;
              result.errorCount++;
              accumulatedMessages.push_back(denyMsg);
              break;
            }
          }
          if (preBlocked) continue;
        }

        int maxSize = kDefaultMaxResultChars;
        if (toolRegistry_) {
          maxSize = toolRegistry_->MaxResultSizeChars(block.asToolUse.name);
        }

        // Submit tool execution to thread pool.
        std::string toolName = block.asToolUse.name;
        std::string inputJson = block.asToolUse.inputJson;
        std::string toolId = block.asToolUse.id;
        futures.push_back(infra::ThreadPool::Global().Submit(
            [this, block, maxSize]() -> ToolResult_ {
              ToolResult_ r;
              r.toolName = block.asToolUse.name;
              r.inputJson = block.asToolUse.inputJson;
              r.toolId = block.asToolUse.id;
              std::string output = ExecuteToolBlock(block, maxSize, &r.error);
              r.isError = !r.error.empty();
              r.message.role = core::MessageRole::User;
              r.message.content.push_back(core::ContentBlock::MakeToolResult(
                  block.asToolUse.id, output, r.isError));
              return r;
            }, infra::TaskPriority::HIGH));
      }

      // Collect results from parallel execution.
      for (auto& future : futures) {
        ToolResult_ r = future.get();
        result.userMessages.push_back(r.message);
        if (r.isError) {
          result.errorCount++;
          if (hookExecutor_) {
            hookExecutor_->RunPostToolUseFailureHooks(
                r.toolName, r.inputJson, r.toolId, r.error, 1, 10000);
          }
        } else {
          // PostToolUse hooks on success (aligned with local-ace)
          if (hookExecutor_) {
            std::string toolOutput;
            if (!r.message.content.empty() &&
                r.message.content[0].type == core::BlockType::ToolResult) {
              toolOutput = r.message.content[0].asToolResult.content;
            }
            hookExecutor_->RunPostToolUseHooks(
                r.toolName, r.inputJson, r.toolId, toolOutput, 0, 10000);
          }
        }
        accumulatedMessages.push_back(r.message);
      }
      continue;
    }

    // Sequential execution for non-concurrent-safe batches.
    for (const auto& block : batch.blocks) {
      const std::vector<core::Message>& decisionMessages =
          batch.concurrentSafe ? messages : accumulatedMessages;

      core::PermissionDecision decision =
          canUseTool(block, decisionMessages);

      if (decision.behavior == core::PermissionBehavior::Deny) {
        core::Message deniedMsg;
        deniedMsg.role = core::MessageRole::User;
        deniedMsg.content.push_back(core::ContentBlock::MakeToolResult(
            block.asToolUse.id,
            "Tool denied: " + decision.reason,
            true));
        result.userMessages.push_back(deniedMsg);
        result.deniedCount++;
        accumulatedMessages.push_back(deniedMsg);
        continue;
      }

      if (decision.behavior == core::PermissionBehavior::Ask) {
        core::Message askMsg;
        askMsg.role = core::MessageRole::User;
        askMsg.content.push_back(core::ContentBlock::MakeToolResult(
            block.asToolUse.id,
            "Tool requires confirmation in non-interactive skeleton mode: " +
                decision.reason,
            true));
        result.userMessages.push_back(askMsg);
        result.deniedCount++;
        result.errorCount++;
        accumulatedMessages.push_back(askMsg);
        continue;
      }

      // PreToolUse hooks: run before tool execution (aligned with local-ace)
      if (hookExecutor_) {
        auto preBatch = hookExecutor_->RunPreToolUseHooks(
            block.asToolUse.name, block.asToolUse.inputJson,
            block.asToolUse.id, 30000);
        bool preBlocked = false;
        for (const auto& hr : preBatch.results) {
          if (hr.outcome == hooks::HookOutcome::Blocking ||
              hr.decision == "deny") {
            preBlocked = true;
            core::Message denyMsg;
            denyMsg.role = core::MessageRole::User;
            std::string denyReason = hr.reason.empty()
                ? ("PreToolUse hook blocked: " + block.asToolUse.name)
                : hr.reason;
            denyMsg.content.push_back(core::ContentBlock::MakeToolResult(
                block.asToolUse.id, denyReason, true));
            result.userMessages.push_back(denyMsg);
            result.deniedCount++;
            result.errorCount++;
            accumulatedMessages.push_back(denyMsg);
            break;
          }
        }
        if (preBlocked) continue;
      }

      int maxSize = kDefaultMaxResultChars;
      if (toolRegistry_) {
        maxSize = toolRegistry_->MaxResultSizeChars(block.asToolUse.name);
      }

      std::string execError;
      std::string output =
          ExecuteToolBlock(block, maxSize, &execError);

      bool isError = !execError.empty();

      core::Message toolMsg;
      toolMsg.role = core::MessageRole::User;
      toolMsg.content.push_back(core::ContentBlock::MakeToolResult(
          block.asToolUse.id, output, isError));
      result.userMessages.push_back(toolMsg);
      if (isError) {
        result.errorCount++;
        // P0-03: Post-tool-use failure hooks (aligned with local-ace)
        // Run hooks when a tool fails so that corrective actions can be taken.
        if (hookExecutor_) {
          hookExecutor_->RunPostToolUseFailureHooks(
              block.asToolUse.name,
              block.asToolUse.inputJson,
              block.asToolUse.id,
              execError,
              1,
              10000);
        }
      } else {
        // PostToolUse hooks on success (aligned with local-ace)
        if (hookExecutor_) {
          hookExecutor_->RunPostToolUseHooks(
              block.asToolUse.name,
              block.asToolUse.inputJson,
              block.asToolUse.id,
              output,
              0,
              10000);
        }
      }
      accumulatedMessages.push_back(toolMsg);
    }
  }

  return result;
}

std::string ToolOrchestrator::ExecuteTodoWrite(
    const std::string& inputJson,
    int maxResultSize,
    std::string* error) const {
  if (error) *error = "";
  json payload;
  try {
    payload = json::parse(inputJson.empty() ? "{}" : inputJson);
  } catch (...) {
    if (error) *error = "TodoWrite input must be valid JSON";
    return "Error: TodoWrite input must be valid JSON";
  }

  json incoming = payload.value("todos", json::array());
  if (!incoming.is_array()) incoming = json::array();

  json tasks = LoadTaskStore(workspaceRoot_);
  if (!payload.value("merge", false)) {
    tasks = incoming;
  } else {
    for (const auto& item : incoming) {
      const std::string id = item.value("id", std::string());
      if (id.empty()) continue;
      const int index = FindTaskIndex(tasks, id);
      if (index >= 0) {
        tasks[static_cast<std::size_t>(index)] = item;
      } else {
        tasks.push_back(item);
      }
    }
  }

  std::string writeError;
  if (!SaveTaskStore(workspaceRoot_, tasks, &writeError)) {
    if (error) *error = writeError;
    return "Error: " + writeError;
  }

  const std::string summary = payload.value("summary", std::string());

  // P0-1: Check for tasks being marked completed without verification.
  // If a task is being marked as completed and has acceptance_criteria,
  // append a reminder about the criteria to the tool result.
  std::string criteriaReminder;
  if (!payload.value("merge", false)) {
    // Full replace mode: check all tasks being set to completed
    for (const auto& item : tasks.is_array() ? tasks : json::array()) {
      if (item.value("status", std::string()) == "completed") {
        const std::string criteria = item.value("acceptance_criteria", std::string());
        const std::string content = item.value("content",
            item.value("subject", std::string()));
        if (!criteria.empty()) {
          criteriaReminder += "\n  - Task '" + content + "': " + criteria;
        }
      }
    }
  } else {
    // Merge mode: check only tasks being updated to completed
    for (const auto& item : incoming.is_array() ? incoming : json::array()) {
      if (item.value("status", std::string()) == "completed") {
        const std::string criteria = item.value("acceptance_criteria", std::string());
        const std::string content = item.value("content",
            item.value("subject", std::string()));
        if (!criteria.empty()) {
          criteriaReminder += "\n  - Task '" + content + "': " + criteria;
        }
      }
    }
  }

  // P1-3: Verification nudge - when all tasks are completed but none was
  // a verification step, remind the model to verify before finishing.
  // Aligned with local-ace's verificationNudgeNeeded mechanism.
  bool allDone = true;
  bool hasVerificationStep = false;
  int completedCount = 0;
  if (tasks.is_array()) {
    for (const auto& item : tasks) {
      const std::string status = item.value("status", std::string());
      const std::string content = item.value("content",
          item.value("subject", std::string()));
      if (status != "completed") {
        allDone = false;
      } else {
        ++completedCount;
      }
      // Check if any task looks like a verification step
      std::string lowerContent = content;
      std::transform(lowerContent.begin(), lowerContent.end(),
                     lowerContent.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (lowerContent.find("verif") != std::string::npos ||
          lowerContent.find("test") != std::string::npos ||
          lowerContent.find("run") != std::string::npos ||
          lowerContent.find("check") != std::string::npos ||
          lowerContent.find("build") != std::string::npos) {
        hasVerificationStep = true;
      }
    }
  }

  std::string verificationNudge;
  if (allDone && completedCount >= 3 && !hasVerificationStep) {
    verificationNudge =
        "\n\nNOTE: You just closed out " + std::to_string(completedCount) +
        "+ tasks and none of them was a verification step (run/test/check/build). "
        "Before writing your final summary, you should verify the implementation "
        "by running the code, executing tests, or checking the output. "
        "Do NOT self-assign completion without evidence - run a verification command.";
  }

  std::string result = summary.empty()
      ? "Todo list updated.\n" + RenderTaskSummary(tasks)
      : summary + "\n" + RenderTaskSummary(tasks);

  if (!criteriaReminder.empty()) {
    result += "\n\nREMINDER: You marked task(s) as completed. Please verify the following acceptance criteria were met:"
        + criteriaReminder
        + "\nIf any criteria were NOT verified (e.g., code not run, tests not executed),"
        " run the appropriate verification commands before considering the task done.";
  }

  result += verificationNudge;

  return TruncateResult(result, maxResultSize);
}

std::string ToolOrchestrator::ExecuteTaskCreate(const std::string& inputJson,
                                                int maxResultSize,
                                                std::string* error) const {
  json payload;
  try {
    payload = json::parse(inputJson.empty() ? "{}" : inputJson);
  } catch (...) {
    if (error) *error = "TaskCreate input must be valid JSON";
    return "Error: TaskCreate input must be valid JSON";
  }
  const std::string subject = payload.value("subject", std::string());
  const std::string description = payload.value("description", std::string());
  if (subject.empty() || description.empty()) {
    if (error) *error = "TaskCreate requires subject and description";
    return "Error: missing subject or description";
  }

  json tasks = LoadTaskStore(workspaceRoot_);
  json task = json::object();
  task["id"] = NextTaskId(tasks);
  task["subject"] = subject;
  task["description"] = description;
  task["activeForm"] = payload.value("activeForm", std::string());
  task["status"] = "pending";
  task["blockedBy"] = json::array();
  task["owner"] = "";
  task["metadata"] = payload.value("metadata", json::object());
  tasks.push_back(task);

  std::string writeError;
  if (!SaveTaskStore(workspaceRoot_, tasks, &writeError)) {
    if (error) *error = writeError;
    return "Error: " + writeError;
  }
  return TruncateResult("Task #" + task["id"].get<std::string>() +
                            " created successfully: " + subject,
                        maxResultSize);
}

std::string ToolOrchestrator::ExecuteTaskGet(const std::string& inputJson,
                                             int maxResultSize,
                                             std::string* error) const {
  const std::string taskId = JsonGetString(inputJson, "id");
  if (taskId.empty()) {
    if (error) *error = "TaskGet requires id";
    return "Error: missing id";
  }
  const json tasks = LoadTaskStore(workspaceRoot_);
  const int index = FindTaskIndex(tasks, taskId);
  if (index < 0) {
    if (error) *error = "task not found";
    return "Error: task not found";
  }
  return TruncateResult(tasks[static_cast<std::size_t>(index)].dump(2),
                        maxResultSize);
}

std::string ToolOrchestrator::ExecuteTaskUpdate(const std::string& inputJson,
                                                int maxResultSize,
                                                std::string* error) const {
  json payload;
  try {
    payload = json::parse(inputJson.empty() ? "{}" : inputJson);
  } catch (...) {
    if (error) *error = "TaskUpdate input must be valid JSON";
    return "Error: TaskUpdate input must be valid JSON";
  }
  const std::string taskId = payload.value("id", std::string());
  if (taskId.empty()) {
    if (error) *error = "TaskUpdate requires id";
    return "Error: missing id";
  }

  json tasks = LoadTaskStore(workspaceRoot_);
  const int index = FindTaskIndex(tasks, taskId);
  if (index < 0) {
    if (error) *error = "task not found";
    return "Error: task not found";
  }

  json& task = tasks[static_cast<std::size_t>(index)];
  const std::string oldStatus = task.value("status", std::string("pending"));
  const char* keys[] = {"subject", "description", "activeForm", "status",
                        "owner"};
  for (const char* key : keys) {
    if (payload.contains(key) && payload[key].is_string()) task[key] = payload[key];
  }
  if (payload.contains("blockedBy") && payload["blockedBy"].is_array()) {
    task["blockedBy"] = payload["blockedBy"];
  }
  if (payload.contains("metadata") && payload["metadata"].is_object()) {
    task["metadata"] = payload["metadata"];
  }
  // P1-1: Support acceptance_criteria and error_message fields
  if (payload.contains("acceptance_criteria") && payload["acceptance_criteria"].is_string()) {
    task["acceptance_criteria"] = payload["acceptance_criteria"];
  }
  if (payload.contains("error_message") && payload["error_message"].is_string()) {
    task["error_message"] = payload["error_message"];
  }

  // P1-1: State transition validation
  const std::string newStatus = task.value("status", oldStatus);
  if (newStatus != oldStatus) {
    // Valid transitions:
    // pending -> in_progress (start work)
    // in_progress -> completed (finish successfully)
    // in_progress -> failed (encounter error)
    // failed -> retrying (retry after error)
    // retrying -> in_progress (retry accepted)
    // retrying -> failed (retry failed)
    // failed -> completed (manual override after verification)
    // Any status -> pending (reset)
    static const std::map<std::string, std::set<std::string>> validTransitions = {
      {"pending", {"in_progress"}},
      {"in_progress", {"completed", "failed", "pending"}},
      {"failed", {"retrying", "completed", "pending"}},
      {"retrying", {"in_progress", "failed", "pending"}},
      {"completed", {"pending"}},  // Allow reopening completed tasks
    };
    auto it = validTransitions.find(oldStatus);
    if (it != validTransitions.end() &&
        it->second.find(newStatus) == it->second.end()) {
      // Invalid transition - still allow it but log a warning
      task["status_warning"] = "Transition from " + oldStatus + " to " +
          newStatus + " is unusual";
    }

    // Track retry count for failed -> retrying transitions
    if (newStatus == "retrying") {
      int retryCount = task.value("retry_count", 0);
      task["retry_count"] = retryCount + 1;
      task["last_retry_time"] = std::to_string(
          static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count()));
    }

    // Clear error_message when transitioning away from failed
    if (oldStatus == "failed" && newStatus != "failed") {
      task.erase("error_message");
    }
  }

  std::string writeError;
  if (!SaveTaskStore(workspaceRoot_, tasks, &writeError)) {
    if (error) *error = writeError;
    return "Error: " + writeError;
  }
  return TruncateResult("Task #" + taskId + " updated: " +
                            task.value("subject", std::string()),
                        maxResultSize);
}

std::string ToolOrchestrator::ExecuteTaskList(const std::string&,
                                              int maxResultSize,
                                              std::string* error) const {
  if (error) *error = "";
  const json tasks = LoadTaskStore(workspaceRoot_);
  return TruncateResult(RenderTaskSummary(tasks), maxResultSize);
}

std::string ToolOrchestrator::ExecuteTaskStop(const std::string& inputJson,
                                              int maxResultSize,
                                              std::string* error) const {
  json payload;
  try {
    payload = json::parse(inputJson.empty() ? "{}" : inputJson);
  } catch (...) {
    if (error) *error = "TaskStop input must be valid JSON";
    return "Error: TaskStop input must be valid JSON";
  }
  const std::string taskId = payload.value("id", std::string());
  if (taskId.empty()) {
    if (error) *error = "TaskStop requires id";
    return "Error: missing id";
  }

  json tasks = LoadTaskStore(workspaceRoot_);
  const int index = FindTaskIndex(tasks, taskId);
  if (index < 0) {
    if (error) *error = "task not found";
    return "Error: task not found";
  }
  tasks[static_cast<std::size_t>(index)]["status"] = "cancelled";
  tasks[static_cast<std::size_t>(index)]["stopReason"] =
      payload.value("reason", std::string("stopped by user"));

  std::string writeError;
  if (!SaveTaskStore(workspaceRoot_, tasks, &writeError)) {
    if (error) *error = writeError;
    return "Error: " + writeError;
  }
  return TruncateResult("Task #" + taskId + " stopped", maxResultSize);
}

std::string ToolOrchestrator::ExecuteAskUserQuestion(
    const std::string& inputJson,
    int maxResultSize,
    std::string* error) const {
  if (error) *error = "";
  std::string questions = JsonGetString(inputJson, "questions");
  if (questions.empty()) {
    return TruncateResult(
        "AskUserQuestion tool received. In interactive mode the user "
        "will be prompted; in non-interactive mode, proceed with best "
        "available information.",
        maxResultSize);
  }
  return TruncateResult(
      "Questions queued for user response: " + questions,
      maxResultSize);
}

std::string ToolOrchestrator::ExecuteFileEdit(
    const std::string& inputJson,
    int maxResultSize,
    std::string* error) const {
  const std::string rawPath =
      JsonGetStringMultiKey(inputJson, {"file_path", "path"});
  std::string oldStr = JsonGetString(inputJson, "old_string");
  std::string newStr = JsonGetString(inputJson, "new_string");
  bool replaceAll = JsonGetBool(inputJson, "replace_all");

  if (rawPath.empty() || oldStr.empty()) {
    if (error) *error = "FileEdit requires file_path and old_string";
    return "Error: missing required parameters";
  }

  std::string resolveError;
  const std::string filePath =
      ResolveToolPath(rawPath, workspaceRoot_, true, &resolveError);
  if (filePath.empty()) {
    if (error) *error = resolveError;
    return "Error: " + resolveError;
  }

  std::ifstream input(filePath, std::ios::binary);
  if (!input) {
    if (error) *error = "Cannot read file: " + filePath;
    return "Error: cannot read file " + filePath;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  std::string content = buffer.str();

  // Markdown files use two trailing spaces as hard line breaks — stripping
  // would silently change semantics. Skip stripTrailingWhitespace for .md/.mdx.
  const bool isMarkdown = IsMarkdownFile(filePath);

  // ---- Pre-processing: XML de-sanitization (Task 2) ----
  // Weaker LLMs (Qwen 3.6 35b) sometimes emit XML-escaped entities in
  // tool_call arguments. Reverse them before any matching.
  DesanitizeXmlEntities(oldStr);
  DesanitizeXmlEntities(newStr);

  // ---- Fuzzy matching engine (aligned with local-ace findActualString) ----
  // Multi-level fallback: exact → CRLF → quote+CRLF → whitespace+CRLF
  std::string actualOldStr = oldStr;
  std::string actualNewStr = newStr;
  bool fuzzyUsed = false;
  std::string fuzzyMethod;

  // ---- Diagnostic log: record each step's result ----
  std::ostringstream diagLog;
  diagLog << "[FileEdit] file=" << filePath
          << " old_string_len=" << oldStr.size()
          << " new_string_len=" << newStr.size()
          << " file_len=" << content.size()
          << " replace_all=" << (replaceAll ? "true" : "false")
          << " is_markdown=" << (isMarkdown ? "true" : "false") << "\n";

  // Step 1: Try exact byte-level match (fast path)
  {
    std::size_t pos = content.find(oldStr);
    if (pos != std::string::npos) {
      diagLog << "[FileEdit] Step 1 (exact match): SUCCESS at offset " << pos << "\n";
      // actualOldStr is already oldStr
    } else {
      diagLog << "[FileEdit] Step 1 (exact match): FAILED - old_string not found byte-for-byte in file\n";

      // Step 2: Normalize CRLF → LF
      if (!fuzzyUsed) {
        std::string normContent = content;
        std::string normOld = oldStr;
        NormalizeCRLF(normContent);
        NormalizeCRLF(normOld);
        int fileCrCount = 0, oldCrCount = 0;
        for (char c : content) if (c == '\r') ++fileCrCount;
        for (char c : oldStr) if (c == '\r') ++oldCrCount;
        diagLog << "[FileEdit] Step 2 (CRLF normalization): file_cr_count=" << fileCrCount
                << " old_string_cr_count=" << oldCrCount;

        std::size_t pos2 = normContent.find(normOld);
        if (pos2 != std::string::npos) {
          actualOldStr = MapNormalizedMatchToOriginal(
              content, normContent, pos2, normOld.size(), NORM_CRLF);
          actualNewStr = newStr;
          NormalizeCRLF(actualNewStr);
          fuzzyUsed = true;
          fuzzyMethod = "CRLF-normalization";
          diagLog << " -> SUCCESS at norm_offset=" << pos2 << "\n";
        } else {
          diagLog << " -> FAILED (normalized strings still don't match)\n";
        }
      }

      // Step 3: Normalize quotes (curly → straight) + CRLF
      if (!fuzzyUsed) {
        std::string normContent = content;
        std::string normOld = oldStr;
        NormalizeQuotes(normContent);
        NormalizeQuotes(normOld);
        NormalizeCRLF(normContent);
        NormalizeCRLF(normOld);

        bool fileHasCurly = content.find("\xE2\x80\x9C") != std::string::npos ||
                            content.find("\xE2\x80\x9D") != std::string::npos ||
                            content.find("\xE2\x80\x98") != std::string::npos ||
                            content.find("\xE2\x80\x99") != std::string::npos;
        bool oldHasStraight = oldStr.find('"') != std::string::npos ||
                              oldStr.find('\'') != std::string::npos;
        diagLog << "[FileEdit] Step 3 (quote+CRLF): file_has_curly="
                << (fileHasCurly ? "true" : "false")
                << " old_string_has_straight="
                << (oldHasStraight ? "true" : "false");

        std::size_t pos3 = normContent.find(normOld);
        if (pos3 != std::string::npos) {
          actualOldStr = MapNormalizedMatchToOriginal(
              content, normContent, pos3, normOld.size(),
              NORM_CRLF | NORM_QUOTES);
          actualNewStr = PreserveQuoteStyle(oldStr, actualOldStr, newStr);
          fuzzyUsed = true;
          fuzzyMethod = "quote+CRLF-normalization";
          diagLog << " -> SUCCESS\n";
        } else {
          diagLog << " -> FAILED\n";
        }
      }

      // Step 4: Strip trailing whitespace + CRLF (skip for Markdown files)
      if (!fuzzyUsed && !isMarkdown) {
        std::string strippedOld = StripTrailingWhitespace(oldStr);
        std::string strippedContent = StripTrailingWhitespace(content);
        NormalizeCRLF(strippedContent);
        NormalizeCRLF(strippedOld);

        bool oldHadTrailing = (strippedOld.size() != oldStr.size());
        diagLog << "[FileEdit] Step 4 (whitespace+CRLF): old_string_had_trailing_ws="
                << (oldHadTrailing ? "true" : "false")
                << " size_before=" << oldStr.size()
                << " size_after=" << strippedOld.size();

        std::size_t pos4 = strippedContent.find(strippedOld);
        if (pos4 != std::string::npos) {
          actualOldStr = MapNormalizedMatchToOriginal(
              content, strippedContent, pos4, strippedOld.size(),
              NORM_CRLF | NORM_WS);
          actualNewStr = newStr;
          NormalizeCRLF(actualNewStr);
          fuzzyUsed = true;
          fuzzyMethod = "whitespace+CRLF-normalization";
          diagLog << " -> SUCCESS\n";
        } else {
          diagLog << " -> FAILED\n";
        }
      } else if (!fuzzyUsed && isMarkdown) {
        diagLog << "[FileEdit] Step 4 (whitespace+CRLF): SKIPPED (.md/.mdx file)\n";
      }

      // Step 5: Full combination — quote + CRLF + whitespace
      if (!fuzzyUsed) {
        std::string normOld = oldStr;
        std::string normContent = content;

        NormalizeQuotes(normContent);
        NormalizeQuotes(normOld);

        if (!isMarkdown) {
          normOld = StripTrailingWhitespace(normOld);
          normContent = StripTrailingWhitespace(normContent);
        }

        NormalizeCRLF(normContent);
        NormalizeCRLF(normOld);

        diagLog << "[FileEdit] Step 5 (full normalization): norm_old_len="
                << normOld.size()
                << " norm_content_len=" << normContent.size();

        int fullFlags = NORM_CRLF | NORM_QUOTES;
        if (!isMarkdown) fullFlags |= NORM_WS;

        std::size_t pos5 = normContent.find(normOld);
        if (pos5 != std::string::npos) {
          actualOldStr = MapNormalizedMatchToOriginal(
              content, normContent, pos5, normOld.size(), fullFlags);
          actualNewStr = PreserveQuoteStyle(oldStr, actualOldStr, newStr);
          NormalizeCRLF(actualNewStr);
          fuzzyUsed = true;
          fuzzyMethod = "full-normalization";
          diagLog << " -> SUCCESS\n";
        } else {
          diagLog << " -> FAILED\n";
        }
      }

      // Step 6: Line-anchor-based fallback (Task 1)
      // When all fuzzy strategies fail, use the first and last non-empty
      // lines of old_string as anchors to locate the block in the file.
      // This handles cases where the LLM gets the structure right but has
      // minor whitespace/indentation discrepancies within the block.
      if (!fuzzyUsed) {
        auto splitLines = [](const std::string& s) -> std::vector<std::string> {
          std::vector<std::string> lines;
          std::istringstream iss(s);
          std::string line;
          while (std::getline(iss, line)) {
            // Normalize CRLF
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
          }
          return lines;
        };
        auto trimLine = [](const std::string& s) -> std::string {
          std::size_t start = 0;
          while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) ++start;
          std::size_t end = s.size();
          while (end > start && (s[end-1] == ' ' || s[end-1] == '\t')) --end;
          return s.substr(start, end - start);
        };

        std::vector<std::string> oldLines = splitLines(oldStr);
        std::vector<std::string> fileLines = splitLines(content);

        // Collect non-empty trimmed old lines for anchor matching
        std::vector<std::pair<int, std::string>> trimmedOldLines;
        for (int i = 0; i < static_cast<int>(oldLines.size()); ++i) {
          std::string trimmed = trimLine(oldLines[i]);
          if (!trimmed.empty()) {
            trimmedOldLines.push_back({i, trimmed});
          }
        }

        if (trimmedOldLines.size() >= 2 && fileLines.size() >= oldLines.size()) {
          // Use first and last non-empty lines as anchors
          const std::string& firstAnchor = trimmedOldLines.front().second;
          const std::string& lastAnchor = trimmedOldLines.back().second;
          int oldLineCount = static_cast<int>(oldLines.size());

          diagLog << "[FileEdit] Step 6 (line-anchor fallback): first_anchor_len="
                  << firstAnchor.size() << " last_anchor_len=" << lastAnchor.size()
                  << " old_line_count=" << oldLineCount;

          bool found = false;
          for (int fi = 0; fi <= static_cast<int>(fileLines.size()) - oldLineCount; ++fi) {
            std::string fileFirstTrimmed = trimLine(fileLines[fi]);
            if (fileFirstTrimmed != firstAnchor) continue;

            // Check last anchor
            int lastIdx = fi + oldLineCount - 1;
            if (lastIdx >= static_cast<int>(fileLines.size())) break;
            std::string fileLastTrimmed = trimLine(fileLines[lastIdx]);
            if (fileLastTrimmed != lastAnchor) continue;

            // Verify at least 50% of interior lines match (trimmed)
            int matchCount = 2; // first + last already matched
            for (std::size_t oi = 1; oi < trimmedOldLines.size() - 1; ++oi) {
              int mappedFileLine = fi + trimmedOldLines[oi].first;
              if (mappedFileLine < static_cast<int>(fileLines.size()) &&
                  trimLine(fileLines[mappedFileLine]) == trimmedOldLines[oi].second) {
                ++matchCount;
              }
            }
            int requiredMatches = std::max(2, static_cast<int>(trimmedOldLines.size()) / 2);
            if (matchCount >= requiredMatches) {
              // Found a match! Build actualOldStr from file content lines
              std::string matchedBlock;
              for (int li = fi; li <= lastIdx; ++li) {
                if (li > fi) matchedBlock += '\n';
                matchedBlock += fileLines[li];
              }
              actualOldStr = matchedBlock;
              actualNewStr = newStr;
              NormalizeCRLF(actualNewStr);
              fuzzyUsed = true;
              fuzzyMethod = "line-anchor-fallback";
              found = true;
              diagLog << " -> SUCCESS at file_line=" << fi
                      << " match_count=" << matchCount << "\n";
              break;
            }
          }
          if (!found) {
            diagLog << " -> FAILED (no anchor match found)\n";
          }
        } else {
          diagLog << "[FileEdit] Step 6 (line-anchor fallback): SKIPPED (too few lines)\n";
        }
      }
    }
  }

  // Perform the replacement using actualOldStr (which is an exact substring
  // of the original file content, found via fuzzy matching above).
  std::size_t pos = 0;
  int count = 0;
  while ((pos = content.find(actualOldStr, pos)) != std::string::npos) {
    content.replace(pos, actualOldStr.size(), actualNewStr);
    pos += actualNewStr.size();
    ++count;
    if (!replaceAll) break;
  }

  if (count == 0) {
    // ---- Task 3: Auto-recovery with file re-read ----
    // Re-read the file in case it was modified since the initial read.
    // Then retry exact matching with fresh content.
    std::ifstream reread(filePath, std::ios::binary);
    if (reread) {
      std::ostringstream freshBuf;
      freshBuf << reread.rdbuf();
      std::string freshContent = freshBuf.str();
      if (freshContent != content) {
        diagLog << "[FileEdit] Auto-recovery: file changed on disk, re-trying with fresh content\n";
        // Retry exact match with fresh content
        std::size_t retryPos = freshContent.find(oldStr);
        if (retryPos != std::string::npos) {
          // Found in fresh content! Apply replacement and write.
          if (replaceAll) {
            std::size_t rp = 0;
            int rc = 0;
            while ((rp = freshContent.find(oldStr, rp)) != std::string::npos) {
              freshContent.replace(rp, oldStr.size(), newStr);
              rp += newStr.size();
              ++rc;
            }
            std::ofstream retryOut(filePath, std::ios::binary | std::ios::trunc);
            if (retryOut) {
              retryOut << freshContent;
              std::string msg = "File edited (auto-recovery): " + filePath +
                  " (" + std::to_string(rc) + " occurrences replaced after re-read)";
              return TruncateResult(msg, maxResultSize);
            }
          } else {
            freshContent.replace(retryPos, oldStr.size(), newStr);
            std::ofstream retryOut(filePath, std::ios::binary | std::ios::trunc);
            if (retryOut) {
              retryOut << freshContent;
              std::string msg = "File edited (auto-recovery): " + filePath +
                  " (1 occurrence replaced after re-read)";
              return TruncateResult(msg, maxResultSize);
            }
          }
        }
        // Update content for error message snippet generation
        content = freshContent;
      }
    }

    // ---- Task 5: Improved error messages for LLM self-correction ----
    // Find the closest line-based match to give the LLM a useful snippet.
    std::string closestSnippet;
    {
      auto trimWs = [](const std::string& s) -> std::string {
        std::size_t a = 0;
        while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
        std::size_t b = s.size();
        while (b > a && (s[b-1] == ' ' || s[b-1] == '\t')) --b;
        return s.substr(a, b - a);
      };
      // Use the first non-empty line of old_string to find closest match
      std::istringstream oldIss(oldStr);
      std::string firstNonEmpty;
      {
        std::string line;
        while (std::getline(oldIss, line)) {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          std::string trimmed = trimWs(line);
          if (!trimmed.empty()) { firstNonEmpty = trimmed; break; }
        }
      }
      if (!firstNonEmpty.empty()) {
        std::istringstream fileIss(content);
        std::string fline;
        int lineNum = 0;
        int bestLine = -1;
        int bestDist = INT_MAX;
        while (std::getline(fileIss, fline)) {
          ++lineNum;
          if (!fline.empty() && fline.back() == '\r') fline.pop_back();
          std::string trimmed = trimWs(fline);
          // Simple distance: count of differing characters (bounded)
          int dist = 0;
          std::size_t minLen = std::min(trimmed.size(), firstNonEmpty.size());
          for (std::size_t ci = 0; ci < minLen; ++ci) {
            if (trimmed[ci] != firstNonEmpty[ci]) ++dist;
          }
          dist += static_cast<int>(std::abs(
              static_cast<int>(trimmed.size()) - static_cast<int>(firstNonEmpty.size())));
          if (dist < bestDist) {
            bestDist = dist;
            bestLine = lineNum;
          }
        }
        if (bestLine > 0) {
          // Extract a snippet around the best matching line
          std::istringstream fileIss2(content);
          std::string snippetLine;
          int curLine = 0;
          int contextBefore = 3;
          int contextAfter = 3;
          int startLine = std::max(1, bestLine - contextBefore);
          int endLine = bestLine + contextAfter;
          std::ostringstream snippetStream;
          while (std::getline(fileIss2, snippetLine)) {
            ++curLine;
            if (!snippetLine.empty() && snippetLine.back() == '\r') snippetLine.pop_back();
            if (curLine >= startLine && curLine <= endLine) {
              snippetStream << "  " << curLine << ": " << snippetLine << "\n";
            }
            if (curLine > endLine) break;
          }
          closestSnippet = snippetStream.str();
        }
      }
    }

    std::ostringstream hint;
    hint << "search string not found in " << filePath << "\n";
    hint << "\n--- Diagnostic Log ---\n";
    hint << diagLog.str();
    hint << "\n--- Attempted matching strategies ---\n";
    hint << "  1. Exact match (failed)\n";
    hint << "  2. CRLF normalization (failed)\n";
    hint << "  3. Quote + CRLF normalization (failed)\n";
    if (!isMarkdown) {
      hint << "  4. Whitespace + CRLF normalization (failed)\n";
    } else {
      hint << "  4. Whitespace + CRLF normalization (skipped: .md/.mdx file)\n";
    }
    hint << "  5. Full normalization (failed)\n";
    hint << "  6. Line-anchor fallback (failed)\n";
    hint << "\n--- Possible causes ---\n";
    hint << "  - old_string does not match any part of the file\n";
    hint << "  - Indentation mismatch (tabs vs spaces)\n";
    hint << "  - The file may have been modified since it was last read\n";
    hint << "  - old_string length: " << oldStr.size() << " bytes\n";
    hint << "  - file content length: " << content.size() << " bytes\n";
    if (!closestSnippet.empty()) {
      hint << "\n--- Closest matching region in file ---\n";
      hint << closestSnippet;
    }
    hint << "\n--- How to fix ---\n";
    hint << "  1. Use Read to get the CURRENT content of " << filePath << "\n";
    hint << "  2. Copy the EXACT text you want to replace from the Read output\n";
    hint << "  3. Retry Edit with the correct old_string matching the file exactly\n";
    hint << "  4. For large changes, consider using Write to overwrite the entire file\n";
    if (error) *error = hint.str();
    return "Error: " + hint.str();
  }

  std::ofstream output(filePath, std::ios::binary | std::ios::trunc);
  if (!output) {
    if (error) *error = "Cannot write file: " + filePath;
    return "Error: cannot write to " + filePath;
  }
  output << content;

  std::string resultMsg;
  if (replaceAll) {
    resultMsg = "File edited: " + filePath + " (" +
        std::to_string(count) + " occurrences replaced)";
  } else {
    resultMsg = "File edited: " + filePath + " (1 occurrence replaced)";
  }
  if (fuzzyUsed) {
    resultMsg += " [fuzzy-match: " + fuzzyMethod + "]";
    // Only include diagnostic log when fuzzy matching was used (avoid noise)
    resultMsg += "\n" + diagLog.str();
  }

  return TruncateResult(resultMsg, maxResultSize);
}

std::string ToolOrchestrator::ExecuteNotebookEdit(const std::string& inputJson,
                                                  int maxResultSize,
                                                  std::string* error) const {
  json payload;
  try {
    payload = json::parse(inputJson.empty() ? "{}" : inputJson);
  } catch (...) {
    if (error) *error = "NotebookEdit input must be valid JSON";
    return "Error: NotebookEdit input must be valid JSON";
  }

  const std::string rawPath = payload.value("notebook_path", std::string());
  if (rawPath.empty()) {
    if (error) *error = "NotebookEdit requires notebook_path";
    return "Error: missing notebook_path";
  }
  std::string resolveError;
  const std::string filePath =
      ResolveToolPath(rawPath, workspaceRoot_, true, &resolveError);
  if (filePath.empty()) {
    if (error) *error = resolveError;
    return "Error: " + resolveError;
  }

  std::string readError;
  const std::string raw = ReadFileContent(filePath, &readError);
  if (raw.empty() && !readError.empty()) {
    if (error) *error = readError;
    return "Error: " + readError;
  }

  json notebook;
  try {
    notebook = json::parse(raw);
  } catch (...) {
    if (error) *error = "Notebook is not valid JSON";
    return "Error: notebook is not valid JSON";
  }
  if (!notebook.contains("cells") || !notebook["cells"].is_array()) {
    if (error) *error = "Notebook does not contain a cells array";
    return "Error: notebook does not contain a cells array";
  }

  const std::string editMode = payload.value("edit_mode", std::string("replace"));
  const std::string cellId = payload.value("cell_id", std::string());
  const std::string newSource = payload.value("new_source", std::string());
  const std::string cellType = payload.value("cell_type", std::string("code"));

  json& cells = notebook["cells"];
  int cellIndex = -1;
  if (!cellId.empty()) {
    for (std::size_t i = 0; i < cells.size(); ++i) {
      if (cells[i].is_object() && cells[i].value("id", std::string()) == cellId) {
        cellIndex = static_cast<int>(i);
        break;
      }
    }
  }

  if (editMode == "replace") {
    if (cellIndex < 0) {
      if (error) *error = "replace requires an existing cell_id";
      return "Error: replace requires an existing cell_id";
    }
    cells[static_cast<std::size_t>(cellIndex)]["source"] = json::array({newSource});
    if (!cellType.empty()) {
      cells[static_cast<std::size_t>(cellIndex)]["cell_type"] = cellType;
    }
  } else if (editMode == "insert") {
    json newCell = json::object();
    newCell["id"] = cellId.empty()
        ? ("cell-" + std::to_string(cells.size() + 1))
        : ("cell-" + cellId + "-new");
    newCell["cell_type"] = cellType;
    newCell["metadata"] = json::object();
    newCell["source"] = json::array({newSource});
    if (cellType == "code") {
      newCell["execution_count"] = nullptr;
      newCell["outputs"] = json::array();
    }
    if (cellIndex < 0) {
      cells.insert(cells.begin(), newCell);
    } else {
      cells.insert(cells.begin() + cellIndex + 1, newCell);
    }
  } else if (editMode == "delete") {
    if (cellIndex < 0) {
      if (error) *error = "delete requires an existing cell_id";
      return "Error: delete requires an existing cell_id";
    }
    cells.erase(cells.begin() + cellIndex);
  } else {
    if (error) *error = "Unsupported notebook edit mode";
    return "Error: unsupported notebook edit mode";
  }

  std::string writeError;
  if (!WriteFileContent(filePath, notebook.dump(2), &writeError)) {
    if (error) *error = writeError;
    return "Error: " + writeError;
  }
  return TruncateResult("Notebook edited successfully: " + filePath,
                        maxResultSize);
}

std::string ToolOrchestrator::ExecuteSkill(const std::string& inputJson,
                                           int maxResultSize,
                                           std::string* error) const {
  const std::string command = JsonGetString(inputJson, "command");
  const std::string args = JsonGetString(inputJson, "args");
  if (command.empty()) {
    if (error) *error = "Skill requires command";
    return "Error: missing command";
  }
  json agentInput = json::object();
  agentInput["prompt"] = args.empty()
      ? ("Execute the skill `" + command + "` and complete the requested work.")
      : args;
  agentInput["description"] = "skill:" + command;
  const std::string lower = ToLowerAscii(command);
  if (lower == "plan" || lower == "explore" || lower == "verification" ||
      lower == "general-purpose" || lower == "claude-code-guide") {
    agentInput["subagent_type"] = lower;
  } else {
    agentInput["subagent_type"] = "general-purpose";
  }
  return ExecuteAgent(agentInput.dump(), maxResultSize, error);
}

std::string ToolOrchestrator::ExecuteListMcpResources(
    const std::string& inputJson,
    int maxResultSize,
    std::string* error) const {
  if (!mcpClientManager_) {
    if (error) *error = "McpClientManager not configured";
    return "Error: MCP resource tools require McpClientManager configuration";
  }
  const std::string targetServer = JsonGetString(inputJson, "server");
  std::vector<mcp::McpResourceSchema> resources;
  const std::vector<mcp::McpServerConnection> connections =
      mcpClientManager_->connections();
  for (const auto& connection : connections) {
    if (!targetServer.empty() && connection.name != targetServer) continue;
    mcpClientManager_->RefreshResourcesFromTransport(connection.name);
    std::vector<mcp::McpResourceSchema> current =
        mcpClientManager_->FetchResourcesForClient(connection.name);
    resources.insert(resources.end(), current.begin(), current.end());
  }
  if (resources.empty()) return "No resources found";
  std::ostringstream out;
  for (std::size_t i = 0; i < resources.size(); ++i) {
    const auto& resource = resources[i];
    out << resource.serverName << ": " << resource.name
        << " <" << resource.uri << ">";
    if (!resource.mimeType.empty()) out << " [" << resource.mimeType << "]";
    if (i + 1 < resources.size()) out << "\n";
  }
  return TruncateResult(out.str(), maxResultSize);
}

std::string ToolOrchestrator::ExecuteReadMcpResource(
    const std::string& inputJson,
    int maxResultSize,
    std::string* error) const {
  if (!mcpClientManager_) {
    if (error) *error = "McpClientManager not configured";
    return "Error: MCP resource tools require McpClientManager configuration";
  }
  const std::string server = JsonGetString(inputJson, "server");
  const std::string uri = JsonGetString(inputJson, "uri");
  if (server.empty() || uri.empty()) {
    if (error) *error = "ReadMcpResource requires server and uri";
    return "Error: missing server or uri";
  }
  std::string bodyJson;
  std::string readError;
  if (!mcpClientManager_->ReadResourceFromTransport(
          server, uri, &bodyJson, &readError)) {
    if (error) *error = readError;
    return "Error: " + readError;
  }
  return TruncateResult(bodyJson, maxResultSize);
}

std::string ToolOrchestrator::ExecuteWebFetch(
    const std::string& inputJson,
    int maxResultSize,
    std::string* error) const {
  std::string url = JsonGetString(inputJson, "url");
  if (url.empty()) {
    if (error) *error = "WebFetch requires 'url' parameter";
    return "Error: missing url";
  }
  if (!(StartsWithNoCase(url, "http://") || StartsWithNoCase(url, "https://"))) {
    if (error) *error = "WebFetch only supports http/https URLs";
    return "Error: unsupported URL scheme";
  }

  HttpResponse response;
  std::string requestError;
  if (!HttpGet(url, &response, &requestError)) {
    if (error) *error = requestError;
    return "Error: " + requestError;
  }
  if (response.statusCode >= 400) {
    if (error) *error = "HTTP " + std::to_string(response.statusCode);
    return "Error: HTTP " + std::to_string(response.statusCode);
  }

  std::string result;
  if (response.contentType.find("text/html") != std::string::npos ||
      response.contentType.empty()) {
    result = BuildMarkdownFromHtml(url, response.body);
  } else {
    result = "Source: " + url + "\n\n" + response.body;
  }
  if (error) *error = "";
  return TruncateResult(result, maxResultSize);
}

std::string ToolOrchestrator::ExecuteWebSearch(
    const std::string& inputJson,
    int maxResultSize,
    std::string* error) const {
  std::string query = JsonGetString(inputJson, "query");
  if (query.empty()) {
    if (error) *error = "WebSearch requires 'query' parameter";
    return "Error: missing query";
  }
  int num = 5;
  try {
    json parsed = json::parse(inputJson);
    if (parsed.contains("num") && parsed["num"].is_number_integer()) {
      num = parsed["num"].get<int>();
    }
  } catch (...) {
  }
  if (num < 1) num = 1;
  if (num > 10) num = 10;

  const std::string searchUrl =
      "https://www.bing.com/search?q=" + UrlEncode(query);
  HttpResponse response;
  std::string requestError;
  if (!HttpGet(searchUrl, &response, &requestError)) {
    if (error) *error = requestError;
    return "Error: " + requestError;
  }
  if (response.statusCode >= 400) {
    if (error) *error = "HTTP " + std::to_string(response.statusCode);
    return "Error: HTTP " + std::to_string(response.statusCode);
  }

  const std::vector<std::pair<std::string, std::string> > results =
      ParseSearchResults(response.body, num);
  if (results.empty()) {
    if (error) *error = "no search results parsed";
    return "Error: no search results parsed";
  }

  std::ostringstream output;
  output << "Search query: " << query << "\n";
  output << "Source: Bing HTML\n\n";
  for (std::size_t i = 0; i < results.size(); ++i) {
    output << (i + 1) << ". " << results[i].first << "\n";
    output << results[i].second << "\n\n";
  }

  if (error) *error = "";
  return TruncateResult(output.str(), maxResultSize);
}

}  // namespace tools
}  // namespace agent