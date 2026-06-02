// P0-03: Extract memories + MemDir tests

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#define TEST(name) void test_##name()
#define RUN(name) do { std::cout << "  " << #name << "... "; test_##name(); std::cout << "PASSED" << std::endl; } while(0)
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { std::cerr << "FAILED: " << #a << " == " << #b << " at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)

#include "memory/ExtractMemories.h"
using namespace agent::memory;

TEST(extract_user_preference) {
    ExtractMemories extractor;
    auto results = extractor.ExtractFromText("I prefer using Python for data analysis tasks");
    CHECK(results.size() >= 1);
    CHECK_EQ(results[0].type, "user");
}

TEST(extract_project_context) {
    ExtractMemories extractor;
    auto results = extractor.ExtractFromText("This project uses Python 3.11 with pandas and numpy dependencies");
    CHECK(results.size() >= 1);
    CHECK_EQ(results[0].type, "project");
}

TEST(extract_feedback) {
    ExtractMemories extractor;
    auto results = extractor.ExtractFromText("Good job on the fix, that works correctly now");
    CHECK(results.size() >= 1);
    CHECK_EQ(results[0].type, "feedback");
}

TEST(extract_reference) {
    ExtractMemories extractor;
    auto results = extractor.ExtractFromText("Important: always use PowerShell on this Windows machine");
    CHECK(results.size() >= 1);
    CHECK_EQ(results[0].type, "reference");
}

TEST(ignore_short_text) {
    ExtractMemories extractor;
    ExtractionConfig cfg;
    cfg.minContentLength = 50;
    extractor.SetConfig(cfg);
    auto results = extractor.ExtractFromText("short text");
    CHECK(results.empty());
}

TEST(priority_scoring) {
    ExtractMemories extractor;
    auto results = extractor.ExtractFromText("Critical: never use grep on Windows");
    CHECK(results.size() >= 1);
    CHECK(results[0].priority >= 5);
}

TEST(extract_multiple_from_text) {
    ExtractMemories extractor;
    std::string text = "I prefer concise answers.\n"
                       "The project database is PostgreSQL 15.\n"
                       "Important: the API key is stored in .env file.";
    auto results = extractor.ExtractFromText(text);
    CHECK(results.size() >= 3);
}

TEST(max_memories_per_pass) {
    ExtractMemories extractor;
    ExtractionConfig cfg;
    cfg.maxMemoriesPerPass = 2;
    extractor.SetConfig(cfg);
    std::vector<std::string> texts = {
        "I prefer Python", "Project uses Docker", "Good job", "Important note", "Critical warning"
    };
    auto results = extractor.Extract(texts);
    CHECK(results.size() <= 2);
}

TEST(memdir_resolve_paths) {
    MemDir memdir("G:\\test-project");
    auto paths = memdir.ResolvePaths();
    CHECK(paths.sessionMemoryDir.find(".cpp-agent") != std::string::npos);
    CHECK(paths.projectMemoryDir.find("memory") != std::string::npos);
    CHECK(paths.sessionMemoryFile.find("session-memory.md") != std::string::npos);
}

TEST(memdir_get_session_memory_path) {
    MemDir memdir("G:\\workspace");
    std::string path = memdir.GetSessionMemoryMarkdownPath();
    CHECK(path.find("session-memory.md") != std::string::npos);
    CHECK(path.find(".cpp-agent") != std::string::npos);
}

int main() {
    std::cout << "=== Extract Memories + MemDir Tests ===" << std::endl;
    RUN(extract_user_preference);
    RUN(extract_project_context);
    RUN(extract_feedback);
    RUN(extract_reference);
    RUN(ignore_short_text);
    RUN(priority_scoring);
    RUN(extract_multiple_from_text);
    RUN(max_memories_per_pass);
    RUN(memdir_resolve_paths);
    RUN(memdir_get_session_memory_path);
    std::cout << "\nAll extract memories + memdir tests PASSED" << std::endl;
    return 0;
}
