// TodoWriteTool.cpp
#include "tools/TodoWriteTool.h"
#include <sstream>
namespace agent { namespace tools {
const char* kTodoWriteToolName = "TodoWrite";
const char* kTodoWriteToolDescription =
  "Use this tool to create and manage a structured task list for your current coding session.\n"
  "- Use to track progress, organize complex tasks, and demonstrate thoroughness.\n"
  "- Update todos as you complete items or discover new tasks.";
std::string GetTodoWriteToolInputSchema() {
  return "{\"type\":\"object\",\"properties\":{\"todos\":{\"type\":\"array\"},\"merge\":{\"type\":\"boolean\"}},\"required\":[\"todos\"]}";
}
std::string BuildTodoWriteToolPrompt() {
  std::ostringstream p; p << "- " << kTodoWriteToolName << ": " << kTodoWriteToolDescription; return p.str();
}
}}