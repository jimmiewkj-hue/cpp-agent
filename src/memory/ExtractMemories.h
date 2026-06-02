#pragma once

#include <string>
#include <vector>
#include <functional>

namespace agent {
namespace memory {

// P0-03: Memory extraction engine (aligned with local-ace extractMemories).
// Extracts structured memories from conversation messages for persistent storage.
// Used by AutoDream to build consolidated memory entries.

struct ExtractedMemory {
  std::string content;     // The extracted memory text
  std::string type;        // "user", "project", "feedback", "reference"
  std::string scope;       // "session", "project", "global"
  int priority = 0;        // 0-10, higher = more important
  double confidence = 1.0; // 0.0-1.0, extraction confidence
};

struct ExtractionConfig {
  int maxMemoriesPerPass = 20;
  int minContentLength = 10;
  int maxContentLength = 2000;
  bool extractUserPreferences = true;
  bool extractProjectContext = true;
  bool extractFeedback = true;
  bool extractReferences = true;
};

class ExtractMemories {
 public:
  explicit ExtractMemories(const ExtractionConfig& config = {});

  // Extract memories from a batch of conversation text.
  // Uses keyword heuristics to identify memory-worthy content.
  std::vector<ExtractedMemory> Extract(const std::vector<std::string>& conversationTexts);

  // Extract from a single message
  std::vector<ExtractedMemory> ExtractFromText(const std::string& text);

  // Configuration
  void SetConfig(const ExtractionConfig& config);
  const ExtractionConfig& Config() const { return config_; }

 private:
  // Heuristic classifiers
  bool IsUserPreference(const std::string& text) const;
  bool IsProjectContext(const std::string& text) const;
  bool IsFeedback(const std::string& text) const;
  bool IsReference(const std::string& text) const;

  // Priority scoring
  int ScorePriority(const std::string& text, const std::string& type) const;

  // Normalize extracted text
  std::string NormalizeMemory(const std::string& text) const;

  ExtractionConfig config_;
};

// P0-03: Memory directory paths (aligned with local-ace memdir).
// Manages filesystem paths for memory storage: session, project, team, global.
struct MemDirPaths {
  std::string sessionMemoryDir;   // Per-session memory storage
  std::string projectMemoryDir;   // Project-level shared memory
  std::string teamMemoryDir;      // Team-level shared memory
  std::string globalMemoryDir;    // Global/user-level memory
  std::string sessionMemoryFile;  // Session memory markdown file
};

class MemDir {
 public:
  explicit MemDir(const std::string& workspaceRoot);

  // Path resolution (aligned with local-ace memdir/paths.ts)
  MemDirPaths ResolvePaths() const;
  std::string GetSessionMemoryPath() const;
  std::string GetSessionMemoryDir() const;
  std::string GetProjectMemoryDir() const;
  std::string GetTeamMemoryDir() const;

  // Session memory markdown path (aligned with local-ace getSessionMemoryPath)
  std::string GetSessionMemoryMarkdownPath() const;

  // Ensure directories exist
  bool EnsureDirectories() const;

  // Config
  void SetWorkspaceRoot(const std::string& root);
  const std::string& WorkspaceRoot() const { return workspaceRoot_; }

 private:
  std::string workspaceRoot_;
  std::string ResolveBaseDir() const;
};

}  // namespace memory
}  // namespace agent
