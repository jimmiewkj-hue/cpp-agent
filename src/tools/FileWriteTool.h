// FileWriteTool.h — aligned with local-ace tools/FileWriteTool/
#pragma once

#include <string>

namespace agent {
namespace tools {

extern const char* kFileWriteToolName;
extern const char* kFileWriteToolDescription;
std::string GetFileWriteToolInputSchema();
std::string BuildFileWriteToolPrompt();

}  // namespace tools
}  // namespace agent
