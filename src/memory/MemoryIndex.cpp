#include "memory/MemoryIndex.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <windows.h>

namespace agent {
namespace memory {

namespace {

std::string Trim(const std::string& value) {
  const std::string whitespace = " \t\r\n";
  const std::size_t start = value.find_first_not_of(whitespace);
  if (start == std::string::npos) {
    return std::string();
  }
  const std::size_t end = value.find_last_not_of(whitespace);
  return value.substr(start, end - start + 1);
}

std::vector<std::string> SplitLines(const std::string& value) {
  std::vector<std::string> lines;
  std::stringstream stream(value);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(line);
  }
  if (!value.empty() && value.back() == '\n') {
    lines.push_back(std::string());
  }
  return lines;
}

std::string ReadWholeFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::string();
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

bool WriteWholeFile(const std::string& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  output << content;
  return output.good();
}

bool EnsureDirectoryRecursive(const std::string& path) {
  if (path.empty()) {
    return false;
  }

  std::string normalized = path;
  std::replace(normalized.begin(), normalized.end(), '/', '\\');

  std::size_t cursor = 0;
  if (normalized.size() >= 2 && normalized[1] == ':') {
    cursor = 3;
  }

  while (cursor <= normalized.size()) {
    const std::size_t next = normalized.find('\\', cursor);
    const std::string current =
        next == std::string::npos ? normalized : normalized.substr(0, next);

    if (!current.empty()) {
      const DWORD attrs = GetFileAttributesA(current.c_str());
      if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryA(current.c_str(), nullptr)) {
          const DWORD error = GetLastError();
          if (error != ERROR_ALREADY_EXISTS) {
            return false;
          }
        }
      } else if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return false;
      }
    }

    if (next == std::string::npos) {
      break;
    }
    cursor = next + 1;
  }

  return true;
}

}  // namespace

MemoryIndex::MemoryIndex(const std::string& memoryDir)
    : memoryDir_(memoryDir) {}

const std::string& MemoryIndex::memoryDir() const {
  return memoryDir_;
}

std::string MemoryIndex::ResolveActiveMemoryDir() const {
  char* overridePath = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&overridePath, &length, "AGENT_SHARED_MEMORY_PATH_OVERRIDE") !=
          0 ||
      overridePath == nullptr) {
    return memoryDir_;
  }

  const std::string trimmed = Trim(overridePath);
  std::free(overridePath);
  return trimmed.empty() ? memoryDir_ : trimmed;
}

std::string MemoryIndex::GetEntrypointPath() const {
  return ResolveActiveMemoryDir() + "\\" + kEntrypointName;
}

std::string MemoryIndex::GetTopicFilePath(const std::string& fileName) const {
  return ResolveActiveMemoryDir() + "\\" + NormalizeTopicFileName(fileName);
}

bool MemoryIndex::EnsureMemoryDirExists() const {
  return EnsureDirectoryRecursive(ResolveActiveMemoryDir());
}

std::string MemoryIndex::ReadEntrypoint() const {
  return ReadWholeFile(GetEntrypointPath());
}

EntrypointTruncation MemoryIndex::TruncateEntrypointContent(
    const std::string& raw) const {
  const std::string trimmed = Trim(raw);
  const std::vector<std::string> lines = SplitLines(trimmed);

  EntrypointTruncation truncation;
  truncation.lineCount = static_cast<int>(lines.size());
  truncation.byteCount = static_cast<int>(trimmed.size());
  truncation.wasLineTruncated = truncation.lineCount > kMaxEntrypointLines;
  truncation.wasByteTruncated = truncation.byteCount > kMaxEntrypointBytes;

  if (!truncation.wasLineTruncated && !truncation.wasByteTruncated) {
    truncation.content = trimmed;
    return truncation;
  }

  std::string truncated = trimmed;
  if (truncation.wasLineTruncated) {
    std::ostringstream joined;
    for (int i = 0; i < kMaxEntrypointLines && i < truncation.lineCount; ++i) {
      if (i > 0) {
        joined << '\n';
      }
      joined << lines[static_cast<std::size_t>(i)];
    }
    truncated = joined.str();
  }

  if (static_cast<int>(truncated.size()) > kMaxEntrypointBytes) {
    const std::size_t cutAt = truncated.rfind(
        '\n', static_cast<std::size_t>(kMaxEntrypointBytes));
    truncated = truncated.substr(
        0,
        cutAt == std::string::npos ? static_cast<std::size_t>(kMaxEntrypointBytes)
                                   : cutAt);
  }

  truncation.content = truncated + BuildWarningText(
      truncation.lineCount,
      truncation.byteCount,
      truncation.wasLineTruncated,
      truncation.wasByteTruncated);
  return truncation;
}

bool MemoryIndex::WriteEntrypoint(const std::string& raw) const {
  if (!EnsureMemoryDirExists()) {
    return false;
  }

  const EntrypointTruncation truncation = TruncateEntrypointContent(raw);
  return WriteWholeFile(GetEntrypointPath(), truncation.content);
}

std::string MemoryIndex::ReadTopicFile(const std::string& fileName) const {
  return ReadWholeFile(GetTopicFilePath(fileName));
}

bool MemoryIndex::WriteTopicFile(
    const std::string& fileName,
    const std::string& raw) const {
  if (!EnsureMemoryDirExists()) {
    return false;
  }
  return WriteWholeFile(GetTopicFilePath(fileName), raw);
}

std::vector<TopicFileContent> MemoryIndex::LoadTopicFiles(
    const std::vector<std::string>& fileNames) const {
  std::vector<TopicFileContent> topicFiles;
  const std::vector<MemoryPointer> pointers = ParsePointers(ReadEntrypoint());

  for (const auto& fileName : fileNames) {
    TopicFileContent topic;
    topic.fileName = NormalizeTopicFileName(fileName);
    topic.title = topic.fileName;
    for (const auto& pointer : pointers) {
      if (NormalizeTopicFileName(pointer.fileName) == topic.fileName &&
          !pointer.title.empty()) {
        topic.title = pointer.title;
        break;
      }
    }
    topic.content = ReadTopicFile(topic.fileName);
    topic.exists = !topic.content.empty();
    topicFiles.push_back(topic);
  }

  return topicFiles;
}

std::string MemoryIndex::BuildSystemPromptInjection(
    const std::vector<std::string>& topicFileNames,
    const std::vector<std::string>& extraGuidelines) const {
  std::ostringstream prompt;
  const std::string activeDir = ResolveActiveMemoryDir();
  const bool usingSharedOverride = activeDir != memoryDir_;

  prompt << "# memory\n\n";
  prompt << "You have a persistent, file-based memory system at `"
         << activeDir << "`. This directory already exists \u2014 write to it "
            "directly with the Write tool (do not run mkdir or check for its "
            "existence).\n\n";
  prompt << "You should build up this memory system over time so that future "
            "conversations can have a complete picture of who the user is, "
            "how they would like to collaborate with you, what behaviors to "
            "avoid or repeat, and the context behind the work the user gives "
            "you.\n\n";
  prompt << "If the user explicitly asks you to remember something, save it "
            "immediately. If they ask you to forget something, find and "
            "remove the relevant entry.\n\n";

  if (usingSharedOverride) {
    prompt << "Shared memory override is active via "
              "`AGENT_SHARED_MEMORY_PATH_OVERRIDE`; prefer that location over "
              "the local default path.\n\n";
  }

  prompt << "## Memory types\n"
            "Save information that is NOT derivable from the current project "
            "state:\n"
            "- User: personal preferences, collaboration style, role, goals\n"
            "- Feedback: corrections the user gave, behaviors to repeat/avoid\n"
            "- Project: context not in code \u2014 deadlines, incidents, "
            "decisions, rationale\n"
            "- Reference: pointers to external systems, dashboards, docs\n\n";

  for (const auto& line : extraGuidelines) {
    if (!Trim(line).empty()) {
      prompt << line << "\n";
    }
  }

  prompt << "## How to save memories\n\n"
            "Saving a memory is a two-step process:\n\n"
            "**Step 1** \u2014 write the memory to its own file (e.g., "
            "`user_role.md`, `feedback_testing.md`) using frontmatter:\n"
            "```\n"
            "---\n"
            "type: user | feedback | project | reference\n"
            "---\n"
            "```\n\n"
            "**Step 2** \u2014 add a pointer to that file in `"
         << kEntrypointName
         << "`. `" << kEntrypointName
         << "` is an index, not a memory \u2014 each entry should be one line, "
            "under ~150 characters: `- [Title](file.md) \u2014 one-line hook`. "
            "It has no frontmatter. Never write memory content directly into `"
         << kEntrypointName << "`.\n\n"
            "- `" << kEntrypointName
         << "` is always loaded into your context; lines after "
         << kMaxEntrypointLines
         << " will be truncated, so keep the index concise\n"
            "- Keep the name, description, and type fields up-to-date\n"
            "- Update or remove memories that turn out to be wrong\n"
            "- Do not write duplicate memories; update existing ones first\n\n";

  prompt << "## When to access memory\n"
            "- Read `" << kEntrypointName
         << "` at session start for orientation\n"
            "- Search topic files before starting related work\n"
            "- Use grep on the memory directory to find specific information\n"
            "- Transcript logs are a last resort (large files, slow)\n\n";

  prompt << "## Memory and other forms of persistence\n"
            "Memory is for information useful across future conversations. "
            "For current-conversation persistence, use tasks or plans instead "
            "of saving to memory.\n\n";

  prompt << "## Searching past context\n"
            "When looking for past context:\n"
            "1. Search topic files in your memory directory\n"
            "2. Session transcript logs (last resort \u2014 large, slow)\n"
            "Use narrow search terms (error messages, file paths, function "
            "names) rather than broad keywords.\n\n";

  const std::string entrypoint = ReadEntrypoint();
  if (Trim(entrypoint).empty()) {
    prompt << "## " << kEntrypointName << "\n\n";
    prompt << "Your " << kEntrypointName
           << " is currently empty. When you save new memories, they will "
              "appear here.\n";
  } else {
    const EntrypointTruncation truncation = TruncateEntrypointContent(entrypoint);
    prompt << "## " << kEntrypointName << "\n\n";
    prompt << truncation.content << "\n";
  }

  std::vector<std::string> filesToInject = topicFileNames;
  if (filesToInject.empty()) {
    const std::vector<MemoryPointer> pointers = ParsePointers(entrypoint);
    std::set<std::string> seen;
    for (const auto& pointer : pointers) {
      const std::string normalized = NormalizeTopicFileName(pointer.fileName);
      if (normalized.empty() || seen.find(normalized) != seen.end()) {
        continue;
      }
      seen.insert(normalized);
      filesToInject.push_back(normalized);
      if (static_cast<int>(filesToInject.size()) >= kMaxInjectedTopicFiles) {
        break;
      }
    }
  }

  if (!filesToInject.empty()) {
    prompt << "\n## Topic Files\n";
    const std::vector<TopicFileContent> topics = LoadTopicFiles(filesToInject);
    for (const auto& topic : topics) {
      prompt << "\n### " << topic.title << " (" << topic.fileName << ")\n";
      if (!topic.exists) {
        prompt << "Topic file not found yet.\n";
      } else {
        prompt << TrimTopicForPrompt(topic.content) << "\n";
      }
    }
  }

  return prompt.str();
}

std::vector<MemoryPointer> MemoryIndex::ParsePointers(
    const std::string& content) const {
  std::vector<MemoryPointer> pointers;
  const std::vector<std::string> lines = SplitLines(content);

  for (const auto& line : lines) {
    const std::string trimmed = Trim(line);
    if (trimmed.rfind("- [", 0) != 0) {
      continue;
    }

    const std::size_t titleStart = 3;
    const std::size_t titleEnd = trimmed.find(']', titleStart);
    const std::size_t pathStart = trimmed.find('(', titleEnd);
    const std::size_t pathEnd = trimmed.find(')', pathStart);
    const std::size_t hookSep = trimmed.find(" - ", pathEnd);

    if (titleEnd == std::string::npos || pathStart == std::string::npos ||
        pathEnd == std::string::npos) {
      continue;
    }

    MemoryPointer pointer;
    pointer.title = trimmed.substr(titleStart, titleEnd - titleStart);
    pointer.fileName =
        trimmed.substr(pathStart + 1, pathEnd - pathStart - 1);
    pointer.hook = hookSep == std::string::npos
                       ? std::string()
                       : trimmed.substr(hookSep + 3);
    pointers.push_back(pointer);
  }

  return pointers;
}

std::string MemoryIndex::RenderPointers(
    const std::vector<MemoryPointer>& pointers) const {
  std::vector<MemoryPointer> sorted = pointers;
  std::sort(
      sorted.begin(),
      sorted.end(),
      [](const MemoryPointer& lhs, const MemoryPointer& rhs) {
        if (lhs.title != rhs.title) {
          return lhs.title < rhs.title;
        }
        return lhs.fileName < rhs.fileName;
      });

  std::ostringstream output;
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    const MemoryPointer& pointer = sorted[i];
    output << "- [" << pointer.title << "](" << pointer.fileName << ")";
    if (!pointer.hook.empty()) {
      output << " - " << pointer.hook;
    }
    if (i + 1 < sorted.size()) {
      output << '\n';
    }
  }
  return output.str();
}

bool MemoryIndex::UpsertPointer(const MemoryPointer& pointer) const {
  std::vector<MemoryPointer> pointers = ParsePointers(ReadEntrypoint());
  bool replaced = false;

  for (auto& current : pointers) {
    if (NormalizeTopicFileName(current.fileName) ==
            NormalizeTopicFileName(pointer.fileName) ||
        current.title == pointer.title) {
      current = pointer;
      current.fileName = NormalizeTopicFileName(current.fileName);
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    MemoryPointer normalized = pointer;
    normalized.fileName = NormalizeTopicFileName(normalized.fileName);
    pointers.push_back(normalized);
  }

  return WriteEntrypoint(RenderPointers(pointers));
}

std::string MemoryIndex::BuildWarningText(
    int lineCount,
    int byteCount,
    bool wasLineTruncated,
    bool wasByteTruncated) const {
  std::ostringstream reason;
  if (wasLineTruncated && wasByteTruncated) {
    reason << lineCount << " lines and " << byteCount << " bytes";
  } else if (wasLineTruncated) {
    reason << lineCount << " lines (limit: " << kMaxEntrypointLines << ")";
  } else {
    reason << byteCount << " bytes (limit: " << kMaxEntrypointBytes << ")";
  }

  std::ostringstream warning;
  warning << "\n\n> WARNING: " << kEntrypointName << " is " << reason.str()
          << ". Only part of it was loaded. Keep index entries to one line "
             "under ~200 chars; move detail into topic files.";
  return warning.str();
}

std::string MemoryIndex::NormalizeTopicFileName(const std::string& fileName) const {
  std::string normalized = Trim(fileName);
  std::replace(normalized.begin(), normalized.end(), '/', '\\');
  while (!normalized.empty() && normalized.front() == '\\') {
    normalized.erase(normalized.begin());
  }
  if (normalized.find(':') != std::string::npos) {
    return std::string();
  }
  if (normalized.empty()) {
    return std::string();
  }
  return normalized;
}

std::string MemoryIndex::TrimTopicForPrompt(const std::string& raw) const {
  const std::string trimmed = Trim(raw);
  if (static_cast<int>(trimmed.size()) <= kMaxInjectedTopicBytes) {
    return trimmed;
  }
  return trimmed.substr(0, static_cast<std::size_t>(kMaxInjectedTopicBytes)) +
         "\n\n[topic truncated for prompt budget]";
}

}  // namespace memory
}  // namespace agent
