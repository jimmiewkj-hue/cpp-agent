#include "memory/ExtractMemories.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <direct.h>  // _mkdir

namespace agent {
namespace memory {

// ============================================================================
// ExtractMemories
// ============================================================================

ExtractMemories::ExtractMemories(const ExtractionConfig& config)
    : config_(config) {}

void ExtractMemories::SetConfig(const ExtractionConfig& config) {
  config_ = config;
}

std::vector<ExtractedMemory> ExtractMemories::Extract(
    const std::vector<std::string>& conversationTexts) {
  std::vector<ExtractedMemory> results;
  for (const auto& text : conversationTexts) {
    auto extracted = ExtractFromText(text);
    for (auto& mem : extracted) {
      results.push_back(std::move(mem));
    }
    if (static_cast<int>(results.size()) >= config_.maxMemoriesPerPass) break;
  }
  return results;
}

std::vector<ExtractedMemory> ExtractMemories::ExtractFromText(
    const std::string& text) {
  std::vector<ExtractedMemory> results;

  if (static_cast<int>(text.size()) < config_.minContentLength) return results;

  // Split into sentences/segments for analysis
  std::vector<std::string> segments;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line, '\n')) {
    if (line.empty()) continue;
    // Split long lines into smaller segments
    if (line.size() > 500) {
      size_t pos = 0;
      while (pos < line.size()) {
        size_t end = std::min(pos + 400, line.size());
        // Try to break at sentence boundary
        size_t dot = line.find('.', std::max(pos, pos + 200));
        if (dot != std::string::npos && dot < end) {
          end = dot + 1;
        }
        segments.push_back(line.substr(pos, end - pos));
        pos = end;
        while (pos < line.size() && line[pos] == ' ') ++pos;
      }
    } else {
      segments.push_back(line);
    }
  }

  for (const auto& seg : segments) {
    if (static_cast<int>(seg.size()) < config_.minContentLength) continue;

    std::string type;
    // Check reference first ? "Important:", "Critical:" etc. are strong
    // reference signals that take priority over other classifications.
    if (config_.extractReferences && IsReference(seg)) {
      type = "reference";
    } else if (config_.extractFeedback && IsFeedback(seg)) {
      type = "feedback";
    } else if (config_.extractUserPreferences && IsUserPreference(seg)) {
      type = "user";
    } else if (config_.extractProjectContext && IsProjectContext(seg)) {
      type = "project";
    } else {
      continue;  // Not memory-worthy
    }

    ExtractedMemory mem;
    mem.content = NormalizeMemory(seg);
    if (static_cast<int>(mem.content.size()) > config_.maxContentLength) {
      mem.content = mem.content.substr(0, config_.maxContentLength - 3) + "...";
    }
    mem.type = type;
    mem.scope = (type == "project") ? "project" : "session";
    mem.priority = ScorePriority(seg, type);
    results.push_back(mem);
  }

  return results;
}

bool ExtractMemories::IsUserPreference(const std::string& text) const {
  std::string lower = text;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  // Heuristic patterns for user preferences
  static const std::vector<std::string> patterns = {
    "i prefer", "i want", "i like", "i don't like", "i hate",
    "use ", "don't use", "always", "never", "should use",
    "prefer ", "preference", "my style", "i work with"
  };

  for (const auto& p : patterns) {
    if (lower.find(p) != std::string::npos) return true;
  }
  return false;
}

bool ExtractMemories::IsProjectContext(const std::string& text) const {
  std::string lower = text;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  // Project context signals
  static const std::vector<std::string> patterns = {
    "project uses", "codebase", "workspace", "repository",
    "python version", "node version", "build system",
    "dependencies", "this project", "the project's",
    "database", "api key", "config", "environment"
  };

  for (const auto& p : patterns) {
    if (lower.find(p) != std::string::npos) return true;
  }
  return false;
}

bool ExtractMemories::IsFeedback(const std::string& text) const {
  std::string lower = text;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  static const std::vector<std::string> patterns = {
    "good job", "well done", "correct", "wrong", "error",
    "that's not right", "should be", "fix this", "mistake",
    "you missed", "this is wrong", "that works", "broken"
  };

  for (const auto& p : patterns) {
    if (lower.find(p) != std::string::npos) return true;
  }
  return false;
}

bool ExtractMemories::IsReference(const std::string& text) const {
  std::string lower = text;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  static const std::vector<std::string> patterns = {
    "note:", "reference:", "remember:", "important:",
    "critical:", "warning:", "todo:", "doc:",
    "documentation:", "spec:", "specification:"
  };

  for (const auto& p : patterns) {
    if (lower.find(p) != std::string::npos) return true;
  }
  return false;
}

int ExtractMemories::ScorePriority(const std::string& text,
                                    const std::string& type) const {
  int score = 3;  // Base score

  std::string lower = text;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  // Boost for high-signal keywords
  if (lower.find("critical") != std::string::npos) score += 5;
  if (lower.find("important") != std::string::npos) score += 3;
  if (lower.find("always") != std::string::npos) score += 2;
  if (lower.find("never") != std::string::npos) score += 2;

  // Type-specific boosts
  if (type == "user") score += 1;
  if (type == "reference") score += 1;

  // Clamp to 0-10
  if (score < 0) score = 0;
  if (score > 10) score = 10;
  return score;
}

std::string ExtractMemories::NormalizeMemory(const std::string& text) const {
  std::string result = text;

  // Trim whitespace
  while (!result.empty() && (result.front() == ' ' || result.front() == '\t'))
    result.erase(0, 1);
  while (!result.empty() && (result.back() == ' ' || result.back() == '\t'))
    result.pop_back();

  // Collapse multiple spaces
  std::string normalized;
  bool prevSpace = false;
  for (char c : result) {
    if (c == ' ' || c == '\t') {
      if (!prevSpace && !normalized.empty()) normalized += ' ';
      prevSpace = true;
    } else {
      normalized += c;
      prevSpace = false;
    }
  }

  return normalized;
}

// ============================================================================
// MemDir
// ============================================================================

MemDir::MemDir(const std::string& workspaceRoot)
    : workspaceRoot_(workspaceRoot) {}

void MemDir::SetWorkspaceRoot(const std::string& root) {
  workspaceRoot_ = root;
}

std::string MemDir::ResolveBaseDir() const {
  if (workspaceRoot_.empty()) return ".cpp-agent";
  return workspaceRoot_ + "\\.cpp-agent";
}

MemDirPaths MemDir::ResolvePaths() const {
  std::string base = ResolveBaseDir();
  MemDirPaths paths;
  paths.sessionMemoryDir = base + "\\session";
  paths.projectMemoryDir = base + "\\memory";
  paths.teamMemoryDir = base + "\\team-memory";
  paths.globalMemoryDir = base;
  paths.sessionMemoryFile = GetSessionMemoryMarkdownPath();
  return paths;
}

std::string MemDir::GetSessionMemoryPath() const {
  return ResolveBaseDir() + "\\session";
}

std::string MemDir::GetSessionMemoryDir() const {
  return GetSessionMemoryPath();
}

std::string MemDir::GetProjectMemoryDir() const {
  return ResolveBaseDir() + "\\memory";
}

std::string MemDir::GetTeamMemoryDir() const {
  return ResolveBaseDir() + "\\team-memory";
}

std::string MemDir::GetSessionMemoryMarkdownPath() const {
  return GetSessionMemoryPath() + "\\session-memory.md";
}

bool MemDir::EnsureDirectories() const {
  auto paths = ResolvePaths();
  _mkdir(paths.sessionMemoryDir.c_str());
  _mkdir(paths.projectMemoryDir.c_str());
  return true;
}

}  // namespace memory
}  // namespace agent
