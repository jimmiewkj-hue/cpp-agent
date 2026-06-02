// FileEditTool.h — aligned with local-ace tools/FileEditTool/
#pragma once

#include <string>

namespace agent {
namespace tools {

extern const char* kFileEditToolName;
extern const char* kFileEditToolDescription;
std::string GetFileEditToolInputSchema();
std::string BuildFileEditToolPrompt();

}  // namespace tools
}  // namespace agent
