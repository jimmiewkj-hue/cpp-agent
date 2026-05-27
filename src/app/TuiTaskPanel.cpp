#include "app/TuiTaskPanel.h"

#include "third_party/nlohmann_json.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

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

TuiTaskPanelData LoadTuiTaskPanelData(const std::string& taskStorePath) {
  TuiTaskPanelData data;
  if (taskStorePath.empty()) return data;

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
  return data;
}

std::vector<std::string> BuildTuiTaskPanelLines(
    const TuiTaskPanelData& data,
    int width,
    int maxTasks) {
  std::vector<std::string> lines;
  if (width < 16) return lines;

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
    std::ostringstream row;
    row << StatusIcon(task) << " ";
    if (!task.id.empty()) row << "#" << task.id << " ";
    row << task.subject;
    if (!task.owner.empty()) row << " @" << task.owner;
    if (!task.blockedBy.empty()) {
      row << " blocked:";
      for (std::size_t blocked = 0; blocked < task.blockedBy.size(); ++blocked) {
        if (blocked > 0) row << ",";
        row << task.blockedBy[blocked];
      }
    }
    lines.push_back(TruncateText(row.str(), taskWidth));
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
