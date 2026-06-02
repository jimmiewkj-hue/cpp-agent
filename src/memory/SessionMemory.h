#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <functional>

namespace agent {
namespace memory {

// ============================================================================
// SessionMemoryEntry — a single memory record (aligned with local-ace)
// ============================================================================
struct SessionMemoryEntry {
  std::string id;
  std::string content;
  std::string type;       // "user", "project", "feedback", "reference"
  std::string scope;      // "session", "project", "global"
  int priority = 0;       // higher = more important
  long long createdAtMs = 0;
  long long updatedAtMs = 0;
  bool active = true;
};

// ============================================================================
// SessionMemory — per-session memory store (aligned with local-ace SessionMemory)
// ============================================================================
class SessionMemory {
 public:
  explicit SessionMemory(const std::string& sessionDir);
  ~SessionMemory() = default;

  // CRUD
  std::string AddMemory(const std::string& content, const std::string& type,
                        const std::string& scope = "session", int priority = 0);
  bool UpdateMemory(const std::string& id, const std::string& content);
  bool DeactivateMemory(const std::string& id);
  bool DeleteMemory(const std::string& id);

  // Query
  std::vector<SessionMemoryEntry> ListMemories(const std::string& type = "",
                                                const std::string& scope = "") const;
  std::vector<SessionMemoryEntry> SearchMemories(const std::string& query, int maxResults = 10) const;

  // System prompt injection (aligned with local-ace)
  std::string BuildMemoryContextInjection(int maxChars = 4000) const;

  // Consolidation — merges similar memories, removes stale ones
  void Consolidate();

  // Persistence
  bool Load();
  bool Save() const;

  // Callback for when memory is updated (for auto-consolidation triggers)
  using MemoryChangeCallback = std::function<void(const std::string& memoryId, const std::string& action)>;
  void SetChangeCallback(MemoryChangeCallback cb) { onChange_ = std::move(cb); }

 private:
  std::string FilePath() const;
  std::string sessionDir_;
  std::vector<SessionMemoryEntry> entries_;
  mutable std::mutex mutex_;
  MemoryChangeCallback onChange_;
  long long nextId_ = 1;
};

}  // namespace memory
}  // namespace agent