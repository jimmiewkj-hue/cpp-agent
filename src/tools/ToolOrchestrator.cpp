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

std::string JoinPath(const std::string& lhs, const std::string& rhs);
bool EnsureDirectoryRecursive(const std::string& path);
std::string ReadFileContent(const std::string& path, std::string* error);
bool WriteFileContent(const std::string& path,
                      const std::string& content,
                      std::string* error);
std::string ToLowerAscii(std::string value);

struct ParsedUrl {
  bool secure = false;
  INTERNET_PORT port = 0;
  std::string host;
  std::string path;
};

struct HttpResponse {
  int statusCode = 0;
  std::string body;
  std::string contentType;
  std::string location;
};

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
        ToolResult_ r;
        // R4-2: future.get() rethrows any exception from the worker thread.
        // Catch it so one crashing tool doesn't kill the whole turn.
        try {
          r = future.get();
        } catch (const std::exception& e) {
          r.toolName = "unknown";
          r.isError = true;
          r.error = std::string("internal error: ") + e.what();
          r.message.role = core::MessageRole::User;
          r.message.content.push_back(core::ContentBlock::MakeToolResult(
              "", std::string("[Tool execution crashed] ") + r.error, true));
          LOG_ERROR(TOOL, "Parallel tool future threw exception",
                    {{"what", std::string(e.what())}});
        } catch (...) {
          r.toolName = "unknown";
          r.isError = true;
          r.error = "internal error: unknown exception in parallel tool";
          r.message.role = core::MessageRole::User;
          r.message.content.push_back(core::ContentBlock::MakeToolResult(
              "", std::string("[Tool execution crashed] ") + r.error, true));
          LOG_ERROR(TOOL, "Parallel tool future threw non-std exception", {});
        }
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
                r.message.content[0].type == core::ContentBlockType::ToolResult) {
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
      std::string output;
      // R4-2: Wrap tool execution in try-catch so a thrown exception (e.g.
      // malformed JSON args from the model, ProcessRunner resource cleanup
      // race) becomes an error result fed back to the model, instead of
      // crashing the entire turn via RunTurnWithRecovery's catch. Without
      // this, graph3 sessions died silently right after a tool call with no
      // Execute-tools log and no terminal reason.
      try {
        output = ExecuteToolBlock(block, maxSize, &execError);
      } catch (const std::exception& e) {
        execError = std::string("internal error: ") + e.what();
        output = "[Tool execution crashed] " + execError +
                 "\nTool: " + block.asToolUse.name +
                 "\nInput: " + block.asToolUse.inputJson;
        LOG_ERROR(TOOL, "ExecuteToolBlock threw exception",
                  {{"tool", block.asToolUse.name},
                   {"what", std::string(e.what())}});
      } catch (...) {
        execError = "internal error: unknown exception in tool execution";
        output = "[Tool execution crashed] " + execError +
                 "\nTool: " + block.asToolUse.name;
        LOG_ERROR(TOOL, "ExecuteToolBlock threw non-std exception",
                  {{"tool", block.asToolUse.name}});
      }

      bool isError = !execError.empty();

      core::Message toolMsg;
      toolMsg.role = core::MessageRole::User;
      toolMsg.content.push_back(core::ContentBlock::MakeToolResult(
          block.asToolUse.id, output, isError));
      result.userMessages.push_back(toolMsg);
      if (isError) {
        result.errorCount++;
        // Post-tool-use failure hooks (aligned with local-ace)
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
