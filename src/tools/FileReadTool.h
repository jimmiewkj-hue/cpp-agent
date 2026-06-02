// FileReadTool.h — aligned with local-ace tools/FileReadTool/
#pragma once

#include <string>

namespace agent {
namespace tools {

extern const char* kFileReadToolName;
extern const char* kFileReadToolDescription;
std::string GetFileReadToolInputSchema();
std::string BuildFileReadToolPrompt();

}  // namespace tools
}  // namespace agent
