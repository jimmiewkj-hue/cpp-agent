#include "memory/SessionMemory.h"
#include "third_party/nlohmann_json.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <windows.h>

namespace agent {
namespace memory {

using json = nlohmann::json;

namespace {
long long CurrentTimeMs() {
  auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count();
}

std::string ToLower(const std::string& s) {
  std::string r = s;
  std::transform(r.begin(), r.end(), r.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return r;
}

int SimpleWordMatch(const std::string& text, const std::string& query) {
  std::string lowerText = ToLower(text);
  std::string lowerQuery = ToLower(query);
  int score = 0;
  if (lowerText.find(lowerQuery) != std::string::npos) {
    score += 100;
    // Bonus for exact word match
    std::istringstream iss(lowerText);
    std::string word;
    while (iss >> word) {
      if (word == lowerQuery) { score += 50; break; }
    }
  }
  return score;
}
}  // namespace

// ============================================================================
// Constructor
// ============================================================================
SessionMemory::SessionMemory(const std::string& sessionDir)
    : sessionDir_(sessionDir) {
  Load();
}

// ============================================================================
// FilePath
// ============================================================================
std::string SessionMemory::FilePath() const {
  return sessionDir_ + "\\session-memory.json";
}

// ============================================================================
// AddMemory
// ============================================================================
std::string SessionMemory::AddMemory(const std::string& content,
                                      const std::string& type,
                                      const std::string& scope,
                                      int priority) {
  std::lock_guard<std::mutex> lock(mutex_);

  SessionMemoryEntry entry;
  entry.id = "mem-" + std::to_string(nextId_++);
  entry.content = content;
  entry.type = type;
  entry.scope = scope;
  entry.priority = priority;
  entry.createdAtMs = CurrentTimeMs();
  entry.updatedAtMs = entry.createdAtMs;
  entry.active = true;

  // Check for duplicates
  for (const auto& existing : entries_) {
    if (existing.active && existing.content == content) {
      return existing.id;  // Already exists
    }
  }

  entries_.push_back(entry);
  Save();

  if (onChange_) onChange_(entry.id, "add");
  return entry.id;
}

// ============================================================================
// UpdateMemory
// ============================================================================
bool SessionMemory::UpdateMemory(const std::string& id, const std::string& content) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& entry : entries_) {
    if (entry.id == id && entry.active) {
      entry.content = content;
      entry.updatedAtMs = CurrentTimeMs();
      Save();
      if (onChange_) onChange_(id, "update");
      return true;
    }
  }
  return false;
}

// ============================================================================
// DeactivateMemory
// ============================================================================
bool SessionMemory::DeactivateMemory(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& entry : entries_) {
    if (entry.id == id && entry.active) {
      entry.active = false;
      entry.updatedAtMs = CurrentTimeMs();
      Save();
      if (onChange_) onChange_(id, "deactivate");
      return true;
    }
  }
  return false;
}

// ============================================================================
// DeleteMemory
// ============================================================================
bool SessionMemory::DeleteMemory(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::remove_if(entries_.begin(), entries_.end(),
      [&id](const SessionMemoryEntry& e) { return e.id == id; });
  if (it != entries_.end()) {
    entries_.erase(it, entries_.end());
    Save();
    if (onChange_) onChange_(id, "delete");
    return true;
  }
  return false;
}

// ============================================================================
// ListMemories
// ============================================================================
std::vector<SessionMemoryEntry> SessionMemory::ListMemories(
    const std::string& type, const std::string& scope) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<SessionMemoryEntry> result;
  for (const auto& entry : entries_) {
    if (!entry.active) continue;
    if (!type.empty() && entry.type != type) continue;
    if (!scope.empty() && entry.scope != scope) continue;
    result.push_back(entry);
  }
  // Sort by priority (desc), then by time (desc)
  std::sort(result.begin(), result.end(),
      [](const SessionMemoryEntry& a, const SessionMemoryEntry& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.updatedAtMs > b.updatedAtMs;
      });
  return result;
}

// ============================================================================
// SearchMemories
// ============================================================================
std::vector<SessionMemoryEntry> SessionMemory::SearchMemories(
    const std::string& query, int maxResults) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::pair<int, SessionMemoryEntry>> scored;
  for (const auto& entry : entries_) {
    if (!entry.active) continue;
    int score = SimpleWordMatch(entry.content, query);
    if (score > 0) {
      scored.push_back({score, entry});
    }
  }
  std::sort(scored.begin(), scored.end(),
      [](const auto& a, const auto& b) { return a.first > b.first; });

  std::vector<SessionMemoryEntry> result;
  for (int i = 0; i < std::min(maxResults, static_cast<int>(scored.size())); ++i) {
    result.push_back(scored[i].second);
  }
  return result;
}

// ============================================================================
// BuildMemoryContextInjection (aligned with local-ace memory injection)
// ============================================================================
std::string SessionMemory::BuildMemoryContextInjection(int maxChars) const {
  auto allMemories = ListMemories();
  if (allMemories.empty()) return std::string();

  std::ostringstream out;
  out << "[Session Memories]\n";

  int usedChars = static_cast<int>(out.str().size());
  int count = 0;

  for (const auto& mem : allMemories) {
    std::string line = "- [" + mem.type + "] " + mem.content + "\n";
    if (usedChars + static_cast<int>(line.size()) > maxChars) break;
    out << line;
    usedChars += static_cast<int>(line.size());
    ++count;
  }

  if (count > 0) {
    out << "[End Memories — " << count << " records]\n";
  }
  return out.str();
}

// ============================================================================
// Consolidate — merges similar memories
// ============================================================================
void SessionMemory::Consolidate() {
  std::lock_guard<std::mutex> lock(mutex_);

  // Deactivate memories older than 30 days
  long long cutoff = CurrentTimeMs() - (30LL * 24 * 60 * 60 * 1000);
  int deactivated = 0;
  for (auto& entry : entries_) {
    if (entry.active && entry.updatedAtMs < cutoff && entry.scope == "session") {
      entry.active = false;
      ++deactivated;
    }
  }

  // Merge near-duplicate memories (same type + similar content)
  std::set<std::string> merged;
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (!entries_[i].active || merged.count(entries_[i].id)) continue;
    for (size_t j = i + 1; j < entries_.size(); ++j) {
      if (!entries_[j].active || merged.count(entries_[j].id)) continue;
      if (entries_[i].type != entries_[j].type) continue;

      // Simple similarity check
      const auto& a = entries_[i].content;
      const auto& b = entries_[j].content;
      int commonChars = 0;
      for (size_t k = 0; k < std::min(a.size(), b.size()); ++k) {
        if (a[k] == b[k]) ++commonChars;
      }
      double similarity = static_cast<double>(commonChars) /
          std::max(1.0, static_cast<double>(std::max(a.size(), b.size())));

      if (similarity > 0.8) {
        // Merge: keep the longer one, deactivate the shorter
        if (a.size() >= b.size()) {
          entries_[j].active = false;
          merged.insert(entries_[j].id);
        } else {
          entries_[i].active = false;
          merged.insert(entries_[i].id);
          break;  // Move to next i
        }
      }
    }
  }

  if (deactivated > 0 || !merged.empty()) {
    Save();
  }
}

// ============================================================================
// Load / Save (JSON persistence)
// ============================================================================
bool SessionMemory::Load() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string path = FilePath();

  std::ifstream in(path, std::ios::binary);
  if (!in) return false;

  try {
    json j = json::parse(in);
    entries_.clear();

    if (j.contains("entries") && j["entries"].is_array()) {
      for (const auto& je : j["entries"]) {
        SessionMemoryEntry entry;
        entry.id = je.value("id", "");
        entry.content = je.value("content", "");
        entry.type = je.value("type", "reference");
        entry.scope = je.value("scope", "session");
        entry.priority = je.value("priority", 0);
        entry.createdAtMs = je.value("created_at_ms", 0LL);
        entry.updatedAtMs = je.value("updated_at_ms", 0LL);
        entry.active = je.value("active", true);
        if (!entry.id.empty()) entries_.push_back(entry);
      }
    }

    if (j.contains("next_id") && j["next_id"].is_number()) {
      nextId_ = j["next_id"].get<long long>();
    }

    return true;
  } catch (...) {
    entries_.clear();
    return false;
  }
}

bool SessionMemory::Save() const {
  std::string path = FilePath();

  json j;
  j["next_id"] = nextId_;
  j["entries"] = json::array();

  for (const auto& entry : entries_) {
    json je;
    je["id"] = entry.id;
    je["content"] = entry.content;
    je["type"] = entry.type;
    je["scope"] = entry.scope;
    je["priority"] = entry.priority;
    je["created_at_ms"] = entry.createdAtMs;
    je["updated_at_ms"] = entry.updatedAtMs;
    je["active"] = entry.active;
    j["entries"].push_back(je);
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << j.dump(2, ' ', false, json::error_handler_t::replace);
  return out.good();
}

}  // namespace memory
}  // namespace agent