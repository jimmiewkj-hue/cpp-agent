// NotebookEditTool.h - aligned with local-ace tools/NotebookEditTool/
#pragma once
#include <string>
namespace agent { namespace tools {
extern const char* kNotebookEditToolName;
extern const char* kNotebookEditToolDescription;
std::string GetNotebookEditToolInputSchema();
std::string BuildNotebookEditToolPrompt();
}}