#pragma once

#include <string>
#include <vector>

namespace agent {
namespace memory {

struct EntrypointTruncation {
  std::string content;
  int lineCount = 0;
  int byteCount = 0;
  bool wasLineTruncated = false;
  bool wasByteTruncated = false;
};

struct MemoryPointer {
  std::string title;
  std::string fileName;
  std::string hook;
};

struct TopicFileContent {
  std::string fileName;
  std::string title;
  std::string content;
  bool exists = false;
};

class MemoryIndex {
 public:
  static constexpr const char* kEntrypointName = "MEMORY.md";
  static constexpr int kMaxEntrypointLines = 200;
  static constexpr int kMaxEntrypointBytes = 25000;
  static constexpr int kMaxInjectedTopicFiles = 6;
  static constexpr int kMaxInjectedTopicBytes = 4000;

  explicit MemoryIndex(const std::string& memoryDir);

  const std::string& memoryDir() const;
  std::string ResolveActiveMemoryDir() const;
  std::string GetEntrypointPath() const;
  std::string GetTopicFilePath(const std::string& fileName) const;

  bool EnsureMemoryDirExists() const;
  std::string ReadEntrypoint() const;
  EntrypointTruncation TruncateEntrypointContent(
      const std::string& raw) const;
  bool WriteEntrypoint(const std::string& raw) const;
  std::string ReadTopicFile(const std::string& fileName) const;
  bool WriteTopicFile(const std::string& fileName, const std::string& raw) const;
  std::vector<TopicFileContent> LoadTopicFiles(
      const std::vector<std::string>& fileNames) const;
  std::string BuildSystemPromptInjection(
      const std::vector<std::string>& topicFileNames =
          std::vector<std::string>(),
      const std::vector<std::string>& extraGuidelines =
          std::vector<std::string>()) const;

  std::vector<MemoryPointer> ParsePointers(const std::string& content) const;
  std::string RenderPointers(
      const std::vector<MemoryPointer>& pointers) const;
  bool UpsertPointer(const MemoryPointer& pointer) const;

 private:
  std::string BuildWarningText(
      int lineCount,
      int byteCount,
      bool wasLineTruncated,
      bool wasByteTruncated) const;
  std::string NormalizeTopicFileName(const std::string& fileName) const;
  std::string TrimTopicForPrompt(const std::string& raw) const;

  std::string memoryDir_;
};

}  // namespace memory
}  // namespace agent
