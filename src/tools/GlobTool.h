// GlobTool.h — aligned with local-ace tools/GlobTool/
#pragma once

#include <string>

namespace agent {
namespace tools {

extern const char* kGlobToolName;
extern const char* kGlobToolDescription;
std::string GetGlobToolInputSchema();
std::string BuildGlobToolPrompt();

}  // namespace tools
}  // namespace agent
