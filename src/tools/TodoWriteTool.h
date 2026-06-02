// TodoWriteTool.h — aligned with local-ace tools/TodoWriteTool/
#pragma once

#include <string>

namespace agent {
namespace tools {

extern const char* kTodoWriteToolName;
extern const char* kTodoWriteToolDescription;
std::string GetTodoWriteToolInputSchema();
std::string BuildTodoWriteToolPrompt();

}  // namespace tools
}  // namespace agent
