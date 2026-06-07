// TodoWriteTool.cpp
#include "tools/TodoWriteTool.h"
#include <sstream>
namespace agent { namespace tools {
const char* kTodoWriteToolName = "TodoWrite";
const char* kTodoWriteToolDescription =
  "Use this tool to create and manage a structured task list for your current coding session.\n"
  "- Use to track progress, organize complex tasks, and demonstrate thoroughness.\n"
  "- Update todos as you complete items or discover new tasks.\n"
  "- Each todo item MUST have both 'content' (imperative form) and 'activeForm' (present continuous form).\n"
  "- Each todo item SHOULD include 'acceptance_criteria' defining verifiable completion standards.\n"
  "- ONLY mark a task as completed when ALL acceptance criteria are met.\n"
  "- If a task has no acceptance_criteria, the default is: 'code runs without errors and produces expected output'.\n\n"
  "Task States:\n"
  "- pending: Task not yet started\n"
  "- in_progress: Currently working on (limit to ONE task at a time)\n"
  "- completed: Task finished AND verified against acceptance criteria\n"
  "- failed: Task could not be completed due to errors or blockers\n\n"
  "IMPORTANT: Do NOT mark a task as completed if:\n"
  "- Tests are failing\n"
  "- Implementation is partial\n"
  "- You encountered unresolved errors\n"
  "- Code has been written but not run/verified";
std::string GetTodoWriteToolInputSchema() {
  return "{"
    "\"type\":\"object\","
    "\"properties\":{"
      "\"todos\":{"
        "\"type\":\"array\","
        "\"items\":{"
          "\"type\":\"object\","
          "\"properties\":{"
            "\"id\":{\"type\":\"string\",\"description\":\"Unique identifier for the todo item\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"Imperative form describing what needs to be done (e.g., Run tests)\"},"
            "\"activeForm\":{\"type\":\"string\",\"description\":\"Present continuous form shown during execution (e.g., Running tests)\"},"
            "\"status\":{\"type\":\"string\",\"enum\":[\"pending\",\"in_progress\",\"completed\",\"failed\"],\"description\":\"Current status of the task\"},"
            "\"priority\":{\"type\":\"string\",\"enum\":[\"high\",\"medium\",\"low\"],\"description\":\"Priority level\"},"
            "\"acceptance_criteria\":{\"type\":\"string\",\"description\":\"Verifiable criteria that must ALL be met to mark this task as completed (e.g., 'Code compiles and runs without errors, output file generated with correct format')\"}"
          "},"
          "\"required\":[\"id\",\"content\",\"activeForm\",\"status\"]"
        "}"
      "},"
      "\"merge\":{\"type\":\"boolean\",\"description\":\"If true, merge with existing todos; if false, replace entirely\"}"
    "},"
    "\"required\":[\"todos\"]"
  "}";
}
std::string BuildTodoWriteToolPrompt() {
  std::ostringstream p; p << "- " << kTodoWriteToolName << ": " << kTodoWriteToolDescription; return p.str();
}
}}