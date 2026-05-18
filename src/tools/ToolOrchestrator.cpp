#include "tools/ToolOrchestrator.h"

#include "agents/SubAgentManager.h"
#include "tools/ToolRegistry.h"
#include "third_party/nlohmann_json.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace agent {
namespace tools {

namespace {

static const int kDefaultMaxResultChars = 100000;
static const int kMaxToolResultTruncation = 400000;

std::string JsonGetString(const std::string& jsonStr,
                          const std::string& key,
                          const std::string& fallback = std::string()) {
  try {
    auto j = json::parse(jsonStr);
    if (j.contains(key) && j[key].is_string()) {
      return j[key].get<std::string>();
    }
  } catch (...) {
  }
  return fallback;
}

std::string JsonGetStringMultiKey(const std::string& jsonStr,
                                  const std::vector<std::string>& keys,
                                  const std::string& fallback = std::string()) {
  try {
    auto j = json::parse(jsonStr);
    for (const auto& key : keys) {
      if (j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
      }
    }
  } catch (...) {
  }
  return fallback;
}

bool JsonGetBool(const std::string& jsonStr,
                 const std::string& key,
                 bool fallback = false) {
  try {
    auto j = json::parse(jsonStr);
    if (j.contains(key) && j[key].is_boolean()) {
      return j[key].get<bool>();
    }
    if (j.contains(key) && j[key].is_string()) {
      const std::string val = j[key].get<std::string>();
      return val == "true" || val == "1";
    }
  } catch (...) {
  }
  return fallback;
}

bool CaseInsensitiveCompare(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

std::string ReadFileContent(const std::string& path, std::string* error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (error) *error = "failed to open file: " + path;
    return std::string();
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

bool WriteFileContent(const std::string& path,
                      const std::string& content,
                      std::string* error) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    if (error) *error = "failed to write file: " + path;
    return false;
  }
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!output.good()) {
    if (error) *error = "failed to flush file: " + path;
    return false;
  }
  return true;
}

std::string NormalizeLineEndings(const std::string& input) {
  std::string result;
  result.reserve(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '\r') {
      if (i + 1 < input.size() && input[i + 1] == '\n') {
        result.push_back('\n');
        ++i;
      } else {
        result.push_back('\n');
      }
    } else {
      result.push_back(input[i]);
    }
  }
  return result;
}

struct FileEntry {
  std::string name;
  bool isDirectory = false;
  long long size = 0;
};

std::vector<FileEntry> GlobFiles(const std::string& directory,
                                 const std::string& pattern) {
  std::vector<FileEntry> entries;
  std::string searchPath = directory;
  if (!searchPath.empty() && searchPath.back() != '\\') {
    searchPath.push_back('\\');
  }
  searchPath += pattern;

  WIN32_FIND_DATAA findData;
  HANDLE findHandle = FindFirstFileA(searchPath.c_str(), &findData);
  if (findHandle == INVALID_HANDLE_VALUE) {
    return entries;
  }

  do {
    if (std::strcmp(findData.cFileName, ".") == 0 ||
        std::strcmp(findData.cFileName, "..") == 0) {
      continue;
    }
    FileEntry entry;
    entry.name = findData.cFileName;
    entry.isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    LARGE_INTEGER size;
    size.LowPart = findData.nFileSizeLow;
    size.HighPart = findData.nFileSizeHigh;
    entry.size = size.QuadPart;
    entries.push_back(entry);
  } while (FindNextFileA(findHandle, &findData));

  FindClose(findHandle);
  return entries;
}

std::string GrepFile(const std::string& filePath,
                     const std::string& pattern,
                     int maxMatches) {
  std::ifstream input(filePath);
  if (!input) return std::string();

  std::ostringstream result;
  std::string line;
  int lineNumber = 0;
  int matches = 0;
  const bool caseInsensitive = true;

  auto matchLine = [&](const std::string& haystack, const std::string& needle) {
    if (caseInsensitive) {
      auto it = std::search(
          haystack.begin(), haystack.end(), needle.begin(), needle.end(),
          [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
          });
      return it != haystack.end();
    }
    return haystack.find(needle) != std::string::npos;
  };

  while (std::getline(input, line) && matches < maxMatches) {
    ++lineNumber;
    if (matchLine(line, pattern)) {
      result << filePath << ":" << lineNumber << ": " << line << "\n";
      ++matches;
    }
  }
  return result.str();
}

bool WildcardMatch(const std::string& text, const std::string& pattern) {
  std::size_t t = 0;
  std::size_t p = 0;
  std::size_t starIdx = std::string::npos;
  std::size_t matchIdx = 0;

  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' ||
        std::tolower(static_cast<unsigned char>(pattern[p])) ==
            std::tolower(static_cast<unsigned char>(text[t])))) {
      ++t;
      ++p;
    } else if (p < pattern.size() && pattern[p] == '*') {
      starIdx = p;
      matchIdx = t;
      ++p;
    } else if (starIdx != std::string::npos) {
      p = starIdx + 1;
      matchIdx++;
      t = matchIdx;
    } else {
      return false;
    }
  }

  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

void GrepDirectory(const std::string& dirPath,
                   const std::string& pattern,
                   int maxMatches,
                   int* matchCount,
                   std::ostringstream* output) {
  std::string searchPath = dirPath;
  if (!searchPath.empty() && searchPath.back() != '\\') {
    searchPath.push_back('\\');
  }
  searchPath += "*";

  WIN32_FIND_DATAA findData;
  HANDLE findHandle = FindFirstFileA(searchPath.c_str(), &findData);
  if (findHandle == INVALID_HANDLE_VALUE) return;

  do {
    if (*matchCount >= maxMatches) break;
    if (std::strcmp(findData.cFileName, ".") == 0 ||
        std::strcmp(findData.cFileName, "..") == 0) {
      continue;
    }
    std::string fullPath = dirPath;
    if (!fullPath.empty() && fullPath.back() != '\\') fullPath.push_back('\\');
    fullPath += findData.cFileName;

    if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      GrepDirectory(fullPath, pattern, maxMatches, matchCount, output);
    } else {
      std::string result = GrepFile(fullPath, pattern, maxMatches - *matchCount);
      if (!result.empty()) {
        *output << result;
        for (char ch : result) {
          if (ch == '\n') ++(*matchCount);
        }
      }
    }
  } while (FindNextFileA(findHandle, &findData));

  FindClose(findHandle);
}

}  // namespace

ToolOrchestrator::ToolOrchestrator() {}

void ToolOrchestrator::SetToolRegistry(const ToolRegistry* registry) {
  toolRegistry_ = registry;
}

void ToolOrchestrator::SetSubAgentManager(
    agents::SubAgentManager* subAgentManager) {
  subAgentManager_ = subAgentManager;
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

  if (CaseInsensitiveCompare(name, "Bash")) {
    return ExecuteBash(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "FileRead")) {
    return ExecuteFileRead(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "FileWrite")) {
    return ExecuteFileWrite(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "Grep")) {
    return ExecuteGrep(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "Glob")) {
    return ExecuteGlob(inputJson, maxResultSize, error);
  }
  if (CaseInsensitiveCompare(name, "Agent")) {
    return ExecuteAgent(inputJson, maxResultSize, error);
  }

  if (error) {
    *error = "unknown tool: " + name;
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

  infra::ProcessRunOptions options;
  options.executable = "cmd.exe";
  options.arguments = {"/c", command};
  options.timeoutMs = 120000;

  infra::ProcessRunResult result = processRunner_.Run(options);

  std::ostringstream output;
  if (result.spawnFailed) {
    if (error) *error = result.errorMessage;
    output << "Error: " << result.errorMessage;
  } else if (result.timedOut) {
    if (error) *error = "command timed out after 120s";
    output << "Error: command timed out after 120s\n";
    output << result.stdoutText;
  } else {
    output << result.stdoutText;
    if (result.exitCode != 0) {
      output << "\n[exit code: " << result.exitCode << "]";
    }
  }

  std::string finalOutput = output.str();
  return TruncateResult(finalOutput, maxResultSize > 0 ? maxResultSize
                                                        : kMaxToolResultTruncation);
}

std::string ToolOrchestrator::ExecuteFileRead(const std::string& inputJson,
                                              int maxResultSize,
                                              std::string* error) const {
  std::string filePath = JsonGetStringMultiKey(inputJson, {"file_path", "path"});
  if (filePath.empty()) {
    if (error) *error = "FileRead tool requires 'file_path' parameter";
    return std::string();
  }

  DWORD attrs = GetFileAttributesA(filePath.c_str());
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

  std::string readErr;
  std::string content = ReadFileContent(filePath, &readErr);
  if (content.empty() && !readErr.empty()) {
    if (error) *error = readErr;
    return "Error: " + readErr;
  }

  std::ostringstream output;
  output << "<file path=\"" << filePath << "\">\n";
  output << NormalizeLineEndings(content);
  if (!content.empty() && content.back() != '\n') {
    output << "\n";
  }
  output << "</file>";

  return TruncateResult(output.str(), maxResultSize);
}

std::string ToolOrchestrator::ExecuteFileWrite(const std::string& inputJson,
                                               int maxResultSize,
                                               std::string* error) const {
  std::string filePath = JsonGetStringMultiKey(inputJson, {"file_path", "path"});
  std::string content = JsonGetString(inputJson, "content");

  if (filePath.empty()) {
    if (error) *error = "FileWrite tool requires 'file_path' parameter";
    return std::string();
  }
  if (content.empty()) {
    if (error) *error = "FileWrite tool requires 'content' parameter";
    return std::string();
  }

  DWORD attrs = GetFileAttributesA(filePath.c_str());
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
    searchPath = ".";
  }

  if (pattern.empty()) {
    if (error) *error = "Grep tool requires 'pattern' parameter";
    return std::string();
  }

  const int kMaxGrepMatches = 100;
  std::ostringstream output;
  int matchCount = 0;

  DWORD attrs = GetFileAttributesA(searchPath.c_str());
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
    directory = ".";
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

  std::vector<core::Message> accumulatedMessages = messages;

  for (const auto& batch : batches) {
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
            false));
        result.userMessages.push_back(askMsg);
        accumulatedMessages.push_back(askMsg);
        continue;
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
      }
      accumulatedMessages.push_back(toolMsg);
    }
  }

  return result;
}

}  // namespace tools
}  // namespace agent
