#pragma once

#include <string>
#include <vector>

namespace agent {
namespace app {

struct TuiTaskItem {
  std::string id;
  std::string subject;
  std::string content;          // Task description/content (from TodoWrite)
  std::string activeForm;       // Active form shown for in_progress tasks
  std::string acceptance_criteria;  // Criteria for completion
  std::string status;
  std::string owner;
  std::vector<std::string> blockedBy;
};

struct TuiTaskPanelData {
  std::vector<TuiTaskItem> tasks;
  int pendingCount = 0;
  int inProgressCount = 0;
  int completedCount = 0;
};

TuiTaskPanelData LoadTuiTaskPanelData(const std::string& taskStorePath);

std::vector<std::string> BuildTuiTaskPanelLines(
    const TuiTaskPanelData& data,
    int width,
    int maxTasks);

}  // namespace app
}  // namespace agent
