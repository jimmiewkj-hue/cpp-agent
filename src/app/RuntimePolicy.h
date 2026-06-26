#pragma once

#include "tools/ToolRegistry.h"

#include <cstddef>
#include <string>
#include <vector>

namespace agent {
namespace app {

std::vector<tools::ToolSchema> GetSessionBaseTools(bool interactiveSession);
void RegisterSessionBaseTools(tools::ToolRegistry* registry,
                              bool interactiveSession);

std::string BuildWorkspaceSystemPrompt(const std::string& workspaceRoot,
                                       bool workspaceTrusted);

// FIX-E2 (weak-model support): model-family-aware overload. Weak local models
// (Gemma/Qwen/MiMo) get an extra prescriptive prompt section with stronger
// instruction-following constraints (stay on the specified task/files, act
// don't narrate, don't retry denied tools). The 2-arg overload above delegates
// here with an empty model name (Claude-equivalent, unchanged behavior) so all
// existing callers keep working.
std::string BuildWorkspaceSystemPrompt(const std::string& workspaceRoot,
                                       bool workspaceTrusted,
                                       const std::string& modelName);

std::vector<std::string> BuildStartupMessages(bool interactiveSession,
                                              bool workspaceTrusted,
                                              const std::string& workspaceRoot,
                                              std::size_t loadedHookFileCount);

}  // namespace app
}  // namespace agent
