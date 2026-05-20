#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace agent {
namespace api {

struct CostEntry {
  std::string model;
  int inputTokens = 0;
  int outputTokens = 0;
  int cacheReadInputTokens = 0;
  int cacheCreationInputTokens = 0;
  double estimatedCostUsd = 0.0;
  long long timestampMs = 0;
};

class CostTracker {
 public:
  static CostTracker& Instance();

  void RecordUsage(const std::string& model,
                   int inputTokens,
                   int outputTokens,
                   int cacheReadInputTokens,
                   int cacheCreationInputTokens);

  double TotalCostUsd() const;
  int TotalInputTokens() const;
  int TotalOutputTokens() const;
  std::vector<CostEntry> RecentEntries(int maxCount = 20) const;

  void Reset();

 private:
  CostTracker();

  double EstimateCost(const std::string& model,
                      int inputTokens,
                      int outputTokens,
                      int cacheReadTokens,
                      int cacheCreationTokens) const;

  mutable std::mutex mutex_;
  std::vector<CostEntry> entries_;
  std::atomic<double> totalCostUsd_{0.0};
  std::atomic<int> totalInputTokens_{0};
  std::atomic<int> totalOutputTokens_{0};
};

}  // namespace api
}  // namespace agent
