#include "core/MaxMode.h"

#include "api/SideQueryClient.h"
#include "infra/EnvUtil.h"
#include "infra/Logger.h"
#include "infra/ThreadPool.h"

#include <algorithm>
#include <future>
#include <sstream>

namespace agent {
namespace core {

bool ShouldActivateMaxMode(int turnCount, int modelCallCount) {
  if (!infra::GetEnvBool("CPP_AGENT_MAX_MODE", false)) return false;
  const int maxTurns = infra::GetEnvInt("CPP_AGENT_MAX_MODE_TURNS", 10);
  // Only activate during the first N model calls (decision-making phase)
  // and not too frequently (every 3rd call to avoid excessive cost)
  if (modelCallCount > maxTurns) return false;
  if (modelCallCount < 1) return false;
  // Fire on critical decision points: turns 1, 3, 5 (approximately)
  // This gives 2-3 max-mode activations per session for balanced cost
  return (modelCallCount == 1 || modelCallCount == 3 || modelCallCount == 5);
}

MaxModeResult RunMaxModeSampling(
    const std::string& userGoal,
    const std::string& currentContext,
    api::SideQueryClient& sideQueryClient,
    const std::string& model) {
  MaxModeResult result;
  result.activated = true;

  const int numSamples = std::min(3, infra::GetEnvInt("CPP_AGENT_MAX_MODE_SAMPLES", 2));
  const std::string sampleModel = model.empty() ? "default" : model;

  // Truncate context to keep side queries manageable
  std::string truncatedContext = currentContext;
  if (truncatedContext.size() > 3000) {
    truncatedContext = truncatedContext.substr(0, 3000);
  }
  std::string truncatedGoal = userGoal;
  if (truncatedGoal.size() > 1000) {
    truncatedGoal = truncatedGoal.substr(0, 1000);
  }

  // Fire N parallel side queries with temperature=1.0
  auto& pool = infra::ThreadPool::Global();
  std::vector<std::future<std::string>> futures;
  futures.reserve(numSamples);

  for (int i = 0; i < numSamples; ++i) {
    auto sampleFunc = [&, i]() -> std::string {
      api::SideQueryRequest request;
      request.querySource = "max-mode-sample";
      request.model = sampleModel;
      request.systemPrompt =
          "You are a creative problem solver for a coding task. Generate a "
          "concrete, actionable approach to solve the user's goal. Be specific "
          "about file paths, function names, and implementation steps. "
          "Think outside the box — consider alternative approaches that might "
          "be better than the obvious one. Be concise (under 300 words).";
      request.temperature = 1.0;
      request.maxTokens = 512;

      Message userMsg;
      userMsg.role = MessageRole::User;
      userMsg.content.push_back(ContentBlock::MakeText(
          "Goal: " + truncatedGoal +
          "\n\nCurrent context:\n" + truncatedContext));
      request.messages.push_back(userMsg);

      api::SideQueryResponse response = sideQueryClient.Query(request);
      if (!response.ok || response.messages.empty()) return "";

      std::string text;
      for (const auto& msg : response.messages) {
        for (const auto& b : msg.content) {
          if (b.type == BlockType::Text) text += b.asText.text;
        }
      }
      return text;
    };
    futures.push_back(pool.Submit(std::move(sampleFunc),
                                  infra::TaskPriority::HIGH));
  }

  // Collect results
  for (int i = 0; i < numSamples; ++i) {
    MaxModeCandidate candidate;
    candidate.sampleIndex = i;
    candidate.temperature = 1.0;
    try {
      candidate.response = futures[i].get();
    } catch (...) {
      candidate.response = "";
    }
    if (!candidate.response.empty()) {
      result.candidates.push_back(candidate);
    }
  }

  if (result.candidates.empty()) {
    result.activated = false;
    return result;
  }

  // Build judge prompt for the main model
  std::ostringstream judge;
  judge << "[Max Mode — " << result.candidates.size()
        << " alternative approaches generated]\n"
        << "Below are diverse approaches from parallel sampling. "
        << "Evaluate them and select the best one, or combine elements "
        << "from multiple approaches. You are the judge.\n\n";

  for (size_t i = 0; i < result.candidates.size(); ++i) {
    judge << "--- Approach " << (i + 1) << " ---\n"
          << result.candidates[i].response << "\n\n";
  }
  judge << "[End of approaches. Pick the best or synthesize a better plan.]";

  result.judgePrompt = judge.str();

  LOG_INFO(QUERY, "Max Mode: parallel sampling complete",
           {{"samples", std::to_string(result.candidates.size())},
            {"model", sampleModel}});

  return result;
}

}  // namespace core
}  // namespace agent
