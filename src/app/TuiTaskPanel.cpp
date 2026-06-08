#include "app/TuiTaskPanel.h"

#include "third_party/nlohmann_json.hpp"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

using json = nlohmann::json;

namespace agent {
namespace app {
namespace {

std::string TruncateText(const std::string& text, int width) {
  if (width <= 0) return std::string();
  if (static_cast<int>(text.size()) <= width) return text;
  if (width <= 3) return text.substr(0, static_cast<std::size_t>(width));
  return text.substr(0, static_cast<std::size_t>(width - 3)) + "...";
}

int StatusRank(const TuiTaskItem& item) {
  if (item.status == "in_progress") return 0;
  if (item.status == "pending" && item.blockedBy.empty()) return 1;
  if (item.status == "pending") return 2;
  if (item.status == "completed") return 3;
  return 4;
}

const char* StatusIcon(const TuiTaskItem& item) {
  if (item.status == "completed") return "[x]";
  if (item.status == "in_progress") return "[>]";
  if (!item.blockedBy.empty()) return "[!]";
  return "[ ]";
}

}  // namespace

namespace {

// Get file modification time as a 64-bit value for cache invalidation.
// Returns 0 if the file doesn't exist.
uint64_t GetFileModTime(const std::string& path) {
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad))
    return 0;
  return (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32)
      | fad.ftLastWriteTime.dwLowDateTime;
#else
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return 0;
  return static_cast<uint64_t>(st.st_mtime);
#endif
}

}  // namespace

TuiTaskPanelData LoadTuiTaskPanelData(const std::string& taskStorePath) {
  // Cache: avoid re-reading the file from disk on every refresh.
  // Only re-read when the file's modification time has changed.
  static std::mutex s_cacheMutex;
  static std::string s_cachedPath;
  static uint64_t s_cachedModTime = 0;
  static TuiTaskPanelData s_cachedData;

  if (taskStorePath.empty()) return {};

  {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    uint64_t modTime = GetFileModTime(taskStorePath);
    if (modTime != 0 && taskStorePath == s_cachedPath
        && modTime == s_cachedModTime) {
      return s_cachedData;
    }
  }

  TuiTaskPanelData data;

  std::ifstream input(taskStorePath, std::ios::binary);
  if (!input) return data;

  json root;
  try {
    input >> root;
  } catch (...) {
    return data;
  }

  if (!root.is_array()) return data;

  for (const auto& entry : root) {
    if (!entry.is_object()) continue;
    TuiTaskItem item;
    item.id = entry.value("id", std::string());
    item.subject = entry.value("subject", std::string());
    item.content = entry.value("content", std::string());
    item.activeForm = entry.value("activeForm", std::string());
    item.acceptance_criteria = entry.value("acceptance_criteria", std::string());
    item.status = entry.value("status", std::string("pending"));
    item.owner = entry.value("owner", std::string());
    if (entry.contains("blockedBy") && entry["blockedBy"].is_array()) {
      for (const auto& blocked : entry["blockedBy"]) {
        if (blocked.is_string()) item.blockedBy.push_back(blocked.get<std::string>());
      }
    }

    if (item.status == "completed") {
      ++data.completedCount;
    } else if (item.status == "in_progress") {
      ++data.inProgressCount;
    } else {
      ++data.pendingCount;
    }
    data.tasks.push_back(item);
  }

  std::sort(data.tasks.begin(), data.tasks.end(),
            [](const TuiTaskItem& lhs, const TuiTaskItem& rhs) {
              const int lhsRank = StatusRank(lhs);
              const int rhsRank = StatusRank(rhs);
              if (lhsRank != rhsRank) return lhsRank < rhsRank;
              return lhs.id < rhs.id;
            });

  // Update cache
  {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    s_cachedPath = taskStorePath;
    s_cachedModTime = GetFileModTime(taskStorePath);
    s_cachedData = data;
  }

  return data;
}

std::vector<std::string> BuildTuiTaskPanelLines(
    const TuiTaskPanelData& data,
    int width,
    int maxTasks) {
  std::vector<std::string> lines;
  if (width < 16) return lines;

  // Header summary line with counts
  std::ostringstream summary;
  summary << "Tasks: " << data.tasks.size()
          << " total | " << data.inProgressCount << " running | "
          << data.pendingCount << " pending | "
          << data.completedCount << " done";
  lines.push_back(TruncateText(summary.str(), width));

  const int visibleTasks = std::max(0, maxTasks);
  const int taskWidth = std::max(8, width - 2);
  const std::size_t count =
      std::min<std::size_t>(static_cast<std::size_t>(visibleTasks), data.tasks.size());
  for (std::size_t i = 0; i < count; ++i) {
    const TuiTaskItem& task = data.tasks[i];

    // Primary line: status icon + id + display name
    std::ostringstream row;
    row << StatusIcon(task) << " ";
    if (!task.id.empty()) row << "#" << task.id << " ";

    // Show activeForm for in_progress tasks, otherwise subject or content
    if (task.status == "in_progress" && !task.activeForm.empty()) {
      row << task.activeForm;
    } else if (!task.subject.empty()) {
      row << task.subject;
    } else {
      row << task.content;
    }
    if (!task.owner.empty()) row << " @" << task.owner;
    if (!task.blockedBy.empty()) {
      row << " blocked:";
      for (std::size_t blocked = 0; blocked < task.blockedBy.size(); ++blocked) {
        if (blocked > 0) row << ",";
        row << task.blockedBy[blocked];
      }
    }
    lines.push_back(TruncateText(row.str(), taskWidth));

    // Secondary line: content summary (dimmed via prefix)
    if (!task.content.empty() && task.content != task.subject &&
        task.content != task.activeForm) {
      std::string detail = "  " + task.content;
      lines.push_back(TruncateText(detail, taskWidth));
    }

    // Show acceptance_criteria for completed tasks
    if (task.status == "completed" && !task.acceptance_criteria.empty()) {
      std::string criteria = "  [criteria] " + task.acceptance_criteria;
      lines.push_back(TruncateText(criteria, taskWidth));
    }
  }

  if (data.tasks.size() > count) {
    std::ostringstream more;
    more << "... +" << (data.tasks.size() - count) << " more";
    lines.push_back(TruncateText(more.str(), width));
  }
  return lines;
}

}  // namespace app
}  // namespace agent
