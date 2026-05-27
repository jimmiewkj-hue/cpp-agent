// Extract pending tasks from the most recent TodoWrite tool call in messages
// Returns a summary string like "task1 (pending), task2 (in_progress), task3 (completed)"
std::string ExtractTodoWriteState(const std::vector<Message>& messages) {
  for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
    if (it->role != MessageRole::Assistant) continue;
    for (const auto& block : it->content) {
      if (block.type != BlockType::ToolUse) continue;
      if (block.asToolUse.name != "TodoWrite" && block.asToolUse.name != "TaskUpdate") continue;
      
      const std::string& json = block.asToolUse.inputJson;
      auto todosStart = json.find("\"todos\"");
      if (todosStart == std::string::npos) continue;
      
      auto arrStart = json.find('[', todosStart);
      if (arrStart == std::string::npos) continue;
      
      std::ostringstream summary;
      std::string pendingFirst;
      int totalCount = 0, doneCount = 0, pendingCount = 0;
      
      std::size_t pos = arrStart + 1;
      while (pos < json.size()) {
        auto objStart = json.find('{', pos);
        if (objStart == std::string::npos || objStart >= json.find(']', arrStart)) break;
        auto objEnd = json.find('}', objStart);
        if (objEnd == std::string::npos) break;
        
        std::string obj = json.substr(objStart, objEnd - objStart + 1);
        
        std::string status = "unknown";
        auto statusPos = obj.find("\"status\"");
        if (statusPos != std::string::npos) {
          auto valStart = obj.find('"', statusPos + 9);
          if (valStart != std::string::npos) {
            auto valEnd = obj.find('"', valStart + 1);
            if (valEnd != std::string::npos) {
              status = obj.substr(valStart + 1, valEnd - valStart - 1);
            }
          }
        }
        
        std::string taskContent = "";
        auto contentPos = obj.find("\"content\"");
        if (contentPos == std::string::npos) contentPos = obj.find("\"description\"");
        if (contentPos != std::string::npos) {
          auto valStart = obj.find('"', contentPos + 12);
          if (valStart != std::string::npos) {
            auto valEnd = obj.find('"', valStart + 1);
            if (valEnd != std::string::npos) {
              taskContent = obj.substr(valStart + 1, valEnd - valStart - 1);
            }
          }
        }
        
        ++totalCount;
        if (status == "completed" || status == "done") {
          ++doneCount;
        } else {
          ++pendingCount;
          if (pendingFirst.empty()) pendingFirst = taskContent;
        }
        
        pos = objEnd + 2;
      }
      
      if (totalCount == 0) continue;
      
      if (pendingCount > 0) {
        summary << pendingCount << " pending (next: " << pendingFirst << "), "
                << doneCount << " completed";
      } else if (doneCount == totalCount) {
        summary << "ALL " << totalCount << " tasks completed!";
      } else {
        summary << doneCount << " completed, " << pendingCount << " pending";
      }
      return summary.str();
    }
  }
  return "";
}

// Extract a list of files already created from Write tool calls in messages
std::string ExtractCreatedFiles(const std::vector<Message>& messages) {
  std::set<std::string> files;
  for (const auto& msg : messages) {
    if (msg.role != MessageRole::Assistant) continue;
    for (const auto& block : msg.content) {
      if (block.type != BlockType::ToolUse) continue;
      if (block.asToolUse.name != "Write" && block.asToolUse.name != "FileWrite") continue;
      const std::string& json = block.asToolUse.inputJson;
      const char* keys[] = {"file_path", "path", "filePath"};
      for (int ki = 0; ki < 3; ++ki) {
        std::string searchKey = std::string("\"") + keys[ki] + "\":\"";
        auto keyPos = json.find(searchKey);
        if (keyPos != std::string::npos) {
          auto valStart = keyPos + searchKey.size();
          auto valEnd = json.find('"', valStart);
          if (valEnd != std::string::npos) {
            std::string fpath = json.substr(valStart, valEnd - valStart);
            auto lastSlash = fpath.find_last_of("\\/");
            if (lastSlash != std::string::npos) fpath = fpath.substr(lastSlash + 1);
            files.insert(fpath);
          }
          break;
        }
      }
    }
  }
  if (files.empty()) return "";
  std::ostringstream out;
  for (auto it = files.begin(); it != files.end(); ++it) {
    if (it != files.begin()) out << ", ";
    out << *it;
  }
  return out.str();
}
