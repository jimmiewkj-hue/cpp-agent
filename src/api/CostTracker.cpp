#include "api/CostTracker.h"

#include <algorithm>

namespace agent {
namespace api {

CostTracker& CostTracker::Instance() {
  static CostTracker instance;
  return instance;
}

CostTracker::CostTracker() = default;

void CostTracker::RecordUsage(const std::string& model,
                              int inputTokens,
                              int outputTokens,
                              int cacheReadInputTokens,
                              int cacheCreationInputTokens) {
  CostEntry entry;
  entry.model = model;
  entry.inputTokens = inputTokens;
  entry.outputTokens = outputTokens;
  entry.cacheReadInputTokens = cacheReadInputTokens;
  entry.cacheCreationInputTokens = cacheCreationInputTokens;
  entry.timestampMs = static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  entry.estimatedCostUsd = EstimateCost(model, inputTokens, outputTokens,
                                        cacheReadInputTokens, cacheCreationInputTokens);

  totalCostUsd_.store(totalCostUsd_.load() + entry.estimatedCostUsd);
  totalInputTokens_.store(totalInputTokens_.load() + inputTokens);
  totalOutputTokens_.store(totalOutputTokens_.load() + outputTokens);

  std::lock_guard<std::mutex> lock(mutex_);
  entries_.push_back(entry);
  if (entries_.size() > 200) entries_.erase(entries_.begin());
}

double CostTracker::TotalCostUsd() const {
  return totalCostUsd_.load();
}

int CostTracker::TotalInputTokens() const {
  return totalInputTokens_.load();
}

int CostTracker::TotalOutputTokens() const {
  return totalOutputTokens_.load();
}

std::vector<CostEntry> CostTracker::RecentEntries(int maxCount) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<CostEntry> result;
  int start = std::max(0, static_cast<int>(entries_.size()) - maxCount);
  for (int i = start; i < static_cast<int>(entries_.size()); ++i)
    result.push_back(entries_[i]);
  return result;
}

void CostTracker::Reset() {
  totalCostUsd_.store(0.0);
  totalInputTokens_.store(0);
  totalOutputTokens_.store(0);
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
}

double CostTracker::EstimateCost(const std::string& model,
                                 int inputTokens,
                                 int outputTokens,
                                 int cacheReadTokens,
                                 int) const {
  double inputPrice = 3.0;
  double outputPrice = 15.0;
  double cacheReadPrice = 0.30;

  if (model.find("sonnet") != std::string::npos ||
      model.find("Sonnet") != std::string::npos ||
      model.find("3.6") != std::string::npos) {
    inputPrice = 3.0;
    outputPrice = 15.0;
  } else if (model.find("opus") != std::string::npos ||
             model.find("Opus") != std::string::npos) {
    inputPrice = 15.0;
    outputPrice = 75.0;
  } else if (model.find("haiku") != std::string::npos ||
             model.find("Haiku") != std::string::npos) {
    inputPrice = 0.80;
    outputPrice = 4.0;
  }

  return (inputTokens * inputPrice + outputTokens * outputPrice +
          cacheReadTokens * cacheReadPrice) / 1000000.0;
}

}  // namespace api
}  // namespace agent
