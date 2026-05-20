#pragma once

#include <string>
#include <vector>

namespace agent {
namespace memory {

struct MemoryManifestEntry {
  std::string fileName;
  std::string type;
  std::string description;
  long long mtimeMs = 0;
};

struct RelevantMemory {
  std::string path;
  long long mtimeMs = 0;
};

class MemoryScanner {
 public:
  static constexpr int kMaxManifestFiles = 200;
  static constexpr int kMaxSelectedMemories = 5;

  explicit MemoryScanner(const std::string& memoryDir);

  std::vector<MemoryManifestEntry> ScanMemoryFiles() const;
  std::string FormatMemoryManifest(
      const std::vector<MemoryManifestEntry>& entries) const;
  std::string BuildSelectorPrompt(
      const std::string& userQuery,
      const std::string& manifest) const;

  std::vector<RelevantMemory> ParseSelectorResponse(
      const std::string& response) const;

 private:
  std::string ExtractFrontmatterField(const std::string& content,
                                      const std::string& field) const;
  std::string ReadFirstLines(const std::string& path, int maxLines) const;

  std::string memoryDir_;
};

}  // namespace memory
}  // namespace agent
