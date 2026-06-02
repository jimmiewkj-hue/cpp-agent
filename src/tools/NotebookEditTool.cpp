#include "tools/NotebookEditTool.h"
#include <sstream>
namespace agent { namespace tools {
const char* kNotebookEditToolName = "NotebookEdit";
const char* kNotebookEditToolDescription =
  "Edits a Jupyter notebook (.ipynb file) by adding or modifying cells.\n"
  "- Completely replaces the content of a specific cell in a Jupyter notebook\n"
  "- Jupyter notebooks are modified interactively\n"
  "- The notebook_path parameter must be an absolute path, not a relative path\n"
  "- The cell_number is 0-indexed\n"
  "- Use edit_mode=insert to add a new cell at the index specified by cell_number\n"
  "- Use edit_mode=delete to delete the cell at the index specified by cell_number\n"
  "- To create or overwrite a notebook file, prefer using the FileWrite tool\n\n"
  "Cell types and structure:\n"
  "- code cells: source is a single string (newlines in source are okay)\n"
  "  For code cells, use the \"source\" field with a string value\n"
  "- markdown cells: source is a single string (newlines in source are okay)\n"
  "  For markdown cells, use the \"source\" field with a string value\n\n"
  "Edits to notebooks must include minimally modified surrounding cell content\n"
  "as context to help the model locate the correct cell.";
std::string GetNotebookEditToolInputSchema() {
  return "{"
    "\"type\":\"object\","
    "\"properties\":{"
      "\"notebook_path\":{\"type\":\"string\",\"description\":\"Absolute path to the Jupyter notebook file\"},"
      "\"cell_number\":{\"type\":\"integer\",\"description\":\"0-indexed cell number to edit\"},"
      "\"new_source\":{\"type\":\"string\",\"description\":\"New source for the cell\"},"
      "\"cell_type\":{\"type\":\"string\",\"enum\":[\"code\",\"markdown\"],\"description\":\"Type of the cell\"},"
      "\"edit_mode\":{\"type\":\"string\",\"enum\":[\"replace\",\"insert\",\"delete\"],\"default\":\"replace\"}"
    "},"
    "\"required\":[\"notebook_path\",\"cell_number\",\"new_source\"]"
  "}";
}
std::string BuildNotebookEditToolPrompt() {
  std::ostringstream p; p << "- " << kNotebookEditToolName << ": " << kNotebookEditToolDescription; return p.str();
}
}}