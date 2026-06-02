// GrepTool.h — aligned with local-ace tools/GrepTool/
#pragma once

#include <string>

namespace agent {
namespace tools {

extern const char* kGrepToolName;
extern const char* kGrepToolDescription;
std::string GetGrepToolInputSchema();
std::string BuildGrepToolPrompt();

}  // namespace tools
}  // namespace agent
