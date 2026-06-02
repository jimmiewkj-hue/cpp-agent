// TaskCreateTool.h - aligned with local-ace tools/TaskCreateTool/
#pragma once
#include <string>
namespace agent { namespace tools {
extern const char* kTaskCreateToolName;
extern const char* kTaskCreateToolDescription;
std::string GetTaskCreateToolInputSchema();
std::string BuildTaskCreateToolPrompt();
}}