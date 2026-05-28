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

std::vector<std::string> BuildStartupMessages(bool interactiveSession,
                                              bool workspaceTrusted,
                                              const std::string& workspaceRoot,
                                              std::size_t loadedHookFileCount);

}  // namespace app
}  // namespace agent
