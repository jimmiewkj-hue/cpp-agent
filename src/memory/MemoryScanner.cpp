#include "memory/MemoryScanner.h"
#include "memory/MemoryIndex.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <windows.h>

namespace agent {
namespace memory {

namespace {

std::string Trim(const std::string& value) {
  const std::string whitespace = " \t\r\n";
  const std::size_t start = value.find_first_not_of(whitespace);
  if (start == std::string::npos) return std::string();
  const std::size_t end = value.find_last_not_of(whitespace);
  return value.substr(start, end - start + 1);
}

std::string ExtractFrontmatterBlock(const std::string& content) {
  const std::size_t start = content.find("---\n");
  if (start != 0) {
    std::size_t alt = content.find("---\r\n");
    if (alt != 0) return std::string();
    return content.substr(0, content.find("---\r\n", 4));
  }
  const std::size_t end = content.find("\n---", 4);
  if (end == std::string::npos) {
    std::size_t alt = content.find("\r\n---", 4);
    if (alt == std::string::npos) return std::string();
    return content.substr(4, alt - 4);
  }
  return content.substr(4, end - 4);
}

}  // namespace

MemoryScanner::MemoryScanner(const std::string& memoryDir)
    : memoryDir_(memoryDir) {}

std::string MemoryScanner::ExtractFrontmatterField(
    const std::string& content,
    const std::string& field) const {
  const std::string key = field + ":";
  std::size_t pos = content.find(key);
  if (pos == std::string::npos) return std::string();
  pos += key.size();
  while (pos < content.size() &&
         (content[pos] == ' ' || content[pos] == '\t')) ++pos;
  if (pos >= content.size()) return std::string();
  std::size_t end = content.find('\n', pos);
  if (end == std::string::npos) end = content.size();
  return Trim(content.substr(pos, end - pos));
}

std::string MemoryScanner::ReadFirstLines(
    const std::string& path, int maxLines) const {
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::string();

  std::ostringstream buffer;
  std::string line;
  int count = 0;
  while (count < maxLines && std::getline(input, line)) {
    buffer << line << '\n';
    ++count;
  }
  return buffer.str();
}

std::vector<MemoryManifestEntry> MemoryScanner::ScanMemoryFiles() const {
  std::vector<MemoryManifestEntry> entries;

  std::string searchPath = memoryDir_ + "\\*.md";
  WIN32_FIND_DATAA findData;
  HANDLE findHandle = FindFirstFileA(searchPath.c_str(), &findData);
  if (findHandle == INVALID_HANDLE_VALUE) return entries;

  do {
    const std::string fileName = findData.cFileName;
    if (fileName.empty() || fileName[0] == '.') continue;
    if (fileName == MemoryIndex::kEntrypointName) continue;

    std::string fullPath = memoryDir_ + "\\" + fileName;
    const std::string preview = ReadFirstLines(fullPath, 30);
    if (preview.empty()) continue;

    const std::string frontmatter = ExtractFrontmatterBlock(preview);

    MemoryManifestEntry entry;
    entry.fileName = fileName;
    entry.description = ExtractFrontmatterField(
        frontmatter.empty() ? preview : frontmatter, "description");
    entry.type = ExtractFrontmatterField(
        frontmatter.empty() ? std::string() : frontmatter, "type");
    if (entry.type.empty()) entry.type = "unknown";

    ULARGE_INTEGER uli;
    uli.LowPart = findData.ftLastWriteTime.dwLowDateTime;
    uli.HighPart = findData.ftLastWriteTime.dwHighDateTime;
    entry.mtimeMs = static_cast<long long>(
        uli.QuadPart / 10000ULL - 11644473600000ULL);

    if (entry.description.empty()) {
      std::istringstream stream(preview);
      std::string firstLine;
      std::getline(stream, firstLine);
      firstLine = Trim(firstLine);
      if (!firstLine.empty() && firstLine.size() > 2 &&
          firstLine[0] == '#') {
        firstLine = firstLine.substr(firstLine[1] == '#' ? 2 : 1);
      }
      entry.description = Trim(firstLine);
    }

    entries.push_back(entry);
  } while (FindNextFileA(findHandle, &findData));

  FindClose(findHandle);

  std::sort(entries.begin(), entries.end(),
            [](const MemoryManifestEntry& a, const MemoryManifestEntry& b) {
              return a.mtimeMs > b.mtimeMs;
            });

  if (static_cast<int>(entries.size()) > kMaxManifestFiles)
    entries.resize(kMaxManifestFiles);

  return entries;
}

std::string MemoryScanner::FormatMemoryManifest(
    const std::vector<MemoryManifestEntry>& entries) const {
  std::ostringstream out;
  for (const auto& entry : entries) {
    out << "[" << entry.type << "] " << entry.fileName;
    if (!entry.description.empty())
      out << " -- " << entry.description;
    out << "\n";
  }
  return out.str();
}

std::string MemoryScanner::BuildSelectorPrompt(
    const std::string& userQuery,
    const std::string& manifest) const {
  std::ostringstream prompt;
  prompt << "You are a memory selector. Given a user query and a list of "
         << "available memory files, select up to "
         << kMaxSelectedMemories
         << " files that are definitely relevant to the query.\n\n"
         << "Rules:\n"
         << "- Only select files that are clearly relevant.\n"
         << "- If unsure, it's better to select fewer files.\n"
         << "- Do not select reference docs for recently used tools.\n\n"
         << "Response format:\n"
         << "<selected_memories>\n"
         << "filename1.md\n"
         << "filename2.md\n"
         << "</selected_memories>\n\n"
         << "User query: " << userQuery << "\n\n"
         << "Available memories:\n" << manifest;
  return prompt.str();
}

std::vector<RelevantMemory> MemoryScanner::ParseSelectorResponse(
    const std::string& response) const {
  std::vector<RelevantMemory> results;

  std::size_t start = response.find("<selected_memories>");
  std::size_t end = response.find("</selected_memories>");
  if (start == std::string::npos || end == std::string::npos)
    return results;

  start += 20;
  std::string block = response.substr(start, end - start);
  std::istringstream stream(block);
  std::string line;
  int count = 0;

  while (std::getline(stream, line) && count < kMaxSelectedMemories) {
    line = Trim(line);
    if (line.empty()) continue;
    if (line.find("..") != std::string::npos) continue;
    if (line.find(':') != std::string::npos) continue;

    std::string path = memoryDir_ + "\\" + line;
    RelevantMemory memory;
    memory.path = path;
    memory.mtimeMs = 0;

    WIN32_FILE_ATTRIBUTE_DATA attrs;
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attrs)) {
      ULARGE_INTEGER uli;
      uli.LowPart = attrs.ftLastWriteTime.dwLowDateTime;
      uli.HighPart = attrs.ftLastWriteTime.dwHighDateTime;
      memory.mtimeMs = static_cast<long long>(
          uli.QuadPart / 10000ULL - 11644473600000ULL);
    }

    results.push_back(memory);
    ++count;
  }

  return results;
}

}  // namespace memory
}  // namespace agent
