#include "core/QueryEngine.h"
#include "core/StateTypes.h"
#include "api/ModelClient.h"
#include "api/SideQueryClient.h"
#include "agents/SubAgentManager.h"
#include "infra/SessionManager.h"
#include "infra/StabilityWatchdog.h"
#include "memory/MemoryIndex.h"
#include "permissions/PermissionEngine.h"
#include "tools/ToolOrchestrator.h"
#include "tools/ToolRegistry.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string GetEnvOrDefault(const char* name, const std::string& fallback) {
  char* value = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&value, &len, name) != 0 || value == nullptr || value[0] == '\0') {
    if (value != nullptr) std::free(value);
    return fallback;
  }
  std::string result(value);
  std::free(value);
  return result;
}

std::string ParentPath(const std::string& path) {
  const std::size_t pos = path.find_last_of("\\/");
  if (pos == std::string::npos) return std::string();
  if (pos == 0) return path.substr(0, 1);
  if (pos == 2 && path.size() >= 3 && path[1] == ':')
    return path.substr(0, 3);
  return path.substr(0, pos);
}

std::string JoinPath(const std::string& lhs, const std::string& rhs) {
  if (lhs.empty()) return rhs;
  if (rhs.empty()) return lhs;
  const char last = lhs[lhs.size() - 1];
  if (last == '\\' || last == '/') return lhs + rhs;
  return lhs + "\\" + rhs;
}

bool FileExists(const std::string& path) {
  return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool DirectoryExists(const std::string& path) {
  DWORD attrs = GetFileAttributesA(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool EnsureDirectoryRecursive(const std::string& path) {
  if (path.empty()) return false;
  if (DirectoryExists(path)) return true;
  const std::string parent = ParentPath(path);
  if (!parent.empty() && parent != path && !DirectoryExists(parent)) {
    if (!EnsureDirectoryRecursive(parent)) return false;
  }
  if (CreateDirectoryA(path.c_str(), nullptr)) return true;
  return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string GetCurrentDirectoryString() {
  char buffer[MAX_PATH] = {0};
  DWORD length = GetCurrentDirectoryA(MAX_PATH, buffer);
  if (length == 0 || length >= MAX_PATH) return ".";
  return std::string(buffer, length);
}

std::string GetExecutableDirectory() {
  char buffer[MAX_PATH] = {0};
  DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return GetCurrentDirectoryString();
  return ParentPath(std::string(buffer, length));
}

std::string DiscoverProjectRoot() {
  std::vector<std::string> candidates;
  candidates.push_back(GetCurrentDirectoryString());
  candidates.push_back(GetExecutableDirectory());
  for (const auto& candidate : candidates) {
    std::string cursor = candidate;
    while (!cursor.empty()) {
      if (FileExists(JoinPath(cursor, "CMakeLists.txt")) &&
          FileExists(JoinPath(JoinPath(JoinPath(cursor, "src"), "app"), "main.cpp")))
        return cursor;
      const std::string parent = ParentPath(cursor);
      if (parent.empty() || parent == cursor) break;
      cursor = parent;
    }
  }
  return GetCurrentDirectoryString();
}

std::string Trim(const std::string& value) {
  const std::string whitespace = " \t\r\n";
  std::size_t begin = value.find_first_not_of(whitespace);
  if (begin == std::string::npos) return std::string();
  if (value.size() >= begin + 3 &&
      static_cast<unsigned char>(value[begin]) == 0xEF &&
      static_cast<unsigned char>(value[begin + 1]) == 0xBB &&
      static_cast<unsigned char>(value[begin + 2]) == 0xBF) {
    begin += 3;
    begin = value.find_first_not_of(whitespace, begin);
    if (begin == std::string::npos) return std::string();
  }
  std::size_t end = value.find_last_not_of(whitespace);
  return value.substr(begin, end - begin + 1);
}

std::string Shorten(const std::string& value, std::size_t maxLength) {
  if (value.size() <= maxLength) return value;
  if (maxLength <= 3) return value.substr(0, maxLength);
  return value.substr(0, maxLength - 3) + "...";
}

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int size = MultiByteToWideChar(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return std::wstring();
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &wide[0], size);
  return wide;
}

std::string WideToUtf8(const std::wstring& text) {
  if (text.empty()) return std::string();
  const int size = WideCharToMultiByte(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (size <= 0) return std::string();
  std::string utf8(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
      &utf8[0], size, nullptr, nullptr);
  return utf8;
}

void EraseLastWideCodepoint(std::wstring* text) {
  if (text == nullptr || text->empty()) return;
  text->pop_back();
  if (!text->empty()) {
    wchar_t tail = (*text)[text->size() - 1];
    if (tail >= 0xD800 && tail <= 0xDBFF)
      text->pop_back();
  }
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(line);
  }
  if (lines.empty() && !text.empty())
    lines.push_back(text);
  return lines;
}

std::string JoinToolNames(const std::vector<agent::tools::ToolSchema>& tools) {
  std::string result;
  for (std::size_t i = 0; i < tools.size(); ++i) {
    if (i != 0) result += ", ";
    result += tools[i].name;
  }
  return result;
}

std::string ExtractText(const agent::core::Message& message) {
  std::string combined;
  for (const auto& block : message.content) {
    if (block.type != agent::core::BlockType::Text) continue;
    if (!combined.empty()) combined += "\n";
    combined += block.asText.text;
  }
  return combined;
}

struct ConsoleSize {
  int width = 80;
  int height = 24;
};

ConsoleSize GetConsoleSize() {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  ConsoleSize sz;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
    sz.width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    sz.height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
  }
  return sz;
}

void SetCursorPosition(int x, int y) {
  COORD coord = {static_cast<SHORT>(x), static_cast<SHORT>(y)};
  SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), coord);
}

void HideCursor() {
  CONSOLE_CURSOR_INFO info;
  GetConsoleCursorInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info);
  info.bVisible = FALSE;
  SetConsoleCursorInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info);
}

void ShowCursor() {
  CONSOLE_CURSOR_INFO info;
  GetConsoleCursorInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info);
  info.bVisible = TRUE;
  SetConsoleCursorInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info);
}

void WriteAnsi(const std::string& seq) {
  std::cout << seq << std::flush;
}

void ClearScreen() {
  WriteAnsi("\x1b[2J\x1b[H");
}

void SetColorFg(int r, int g, int b) {
  WriteAnsi("\x1b[38;2;" + std::to_string(r) + ";" +
            std::to_string(g) + ";" + std::to_string(b) + "m");
}

void SetColorReset() {
  WriteAnsi("\x1b[0m");
}

void SetBold() {
  WriteAnsi("\x1b[1m");
}

void SetDim() {
  WriteAnsi("\x1b[2m");
}

std::string PadRight(const std::string& s, int width) {
  if (static_cast<int>(s.size()) >= width) return s;
  return s + std::string(static_cast<std::size_t>(width) - s.size(), ' ');
}

std::string TruncateVisually(const std::string& s, int maxWidth) {
  if (maxWidth <= 0) return "";
  int visWidth = 0;
  std::size_t i = 0;
  for (; i < s.size(); ++i) {
    if (s[i] == '\x1b') {
      while (i < s.size() && s[i] != 'm') ++i;
      continue;
    }
    if (s[i] == '\n') break;
    ++visWidth;
    if (visWidth >= maxWidth) {
      ++i;
      break;
    }
  }
  return s.substr(0, i);
}

struct TuiState {
  std::mutex mutex;
  std::deque<std::string> logLines;
  std::string projectRoot;
  std::string modelName;
  std::string endpoint;
  std::string protocol;
  std::string toolsList;
  std::string statusText;
  std::atomic<bool> running{false};
  std::atomic<bool> quitRequested{false};
};

class AnsiTui {
 public:
  AnsiTui(TuiState& state) : state_(state) {}

  void Init() {
    HideCursor();
    ClearScreen();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    HANDLE stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD consoleMode = 0;
    if (stdoutHandle != INVALID_HANDLE_VALUE &&
        GetConsoleMode(stdoutHandle, &consoleMode)) {
      SetConsoleMode(stdoutHandle,
                     consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                         ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT);
    }
    WriteAnsi("\x1b[?1000h");
    WriteAnsi("\x1b[?1006h");
    RedrawAll();
  }

  void Shutdown() {
    ShowCursor();
    WriteAnsi("\x1b[?1000l");
    WriteAnsi("\x1b[?1006l");
    WriteAnsi("\x1b[0m");
    WriteAnsi("\x1b[?25h");
    std::cout << std::endl << "Session saved. Bye." << std::endl;
  }

  void RedrawAll() {
    ClearScreen();
    int y = 0;
    const auto sz = GetConsoleSize();
    const int w = sz.width;
    const int h = sz.height;
    welcomeLines_ = static_cast<int>(RenderWelcome().size());
    RenderWelcome(y);
    y += welcomeLines_;
    msgTop_ = y;
    msgHeight_ = h - y - 4;
    if (msgHeight_ < 3) msgHeight_ = 3;
    RenderMessageArea(y, msgHeight_, w);
    y += msgHeight_;
    RenderStatusBar(y, w);
    RenderInputLine(h - 1, w);
    SetCursorPosition(2, h - 1);
  }

  void AppendMessage(const std::string& line) {
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      state_.logLines.push_back(line);
      while (static_cast<int>(state_.logLines.size()) > 200)
        state_.logLines.pop_front();
    }
    scrollOffset_ = 999999;
    RefreshMessages();
  }

  void RefreshMessages() {
    const auto sz = GetConsoleSize();
    int totalLines = 0;
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      totalLines = static_cast<int>(state_.logLines.size());
    }
    int maxVisible = msgHeight_ - 2;
    if (maxVisible < 1) maxVisible = 1;
    if (scrollOffset_ < 0) scrollOffset_ = 0;
    int maxOffset = totalLines - maxVisible;
    if (maxOffset < 0) maxOffset = 0;
    if (scrollOffset_ > maxOffset) scrollOffset_ = maxOffset;
    RenderMessageArea(msgTop_, msgHeight_, sz.width);
    SetCursorPosition(2, sz.height - 1);
  }

  void ScrollBy(int delta) {
    scrollOffset_ += delta;
    RefreshMessages();
  }

  void RefreshStatus() {
    const auto sz = GetConsoleSize();
    RenderStatusBar(msgTop_ + msgHeight_, sz.width);
    SetCursorPosition(2, sz.height - 1);
  }

  std::string GetInput() {
    const auto sz = GetConsoleSize();
    const int inputY = sz.height - 1;
    WriteAnsi("\x1b[s");
    SetCursorPosition(2, inputY);
    WriteAnsi("\x1b[0K");
    std::cout << "\x1b[37m> \x1b[0m" << std::flush;
    ShowCursor();

    std::string line;
    std::wstring wideLine;
    HANDLE inHandle = GetStdHandle(STD_INPUT_HANDLE);

    DWORD consoleMode = 0;
    bool isConsole = GetConsoleMode(inHandle, &consoleMode) != 0;

    if (!isConsole) {
      if (!std::getline(std::cin, line)) {
        HideCursor();
        return "";
      }
      HideCursor();
      SetCursorPosition(2, inputY);
      WriteAnsi("\x1b[0K");
      std::cout << "> " << std::flush;
      return line;
    }

    auto redrawInput = [&]() {
      SetCursorPosition(2, inputY);
      WriteAnsi("\x1b[0K");
      std::cout << "\x1b[37m> \x1b[0m" << WideToUtf8(wideLine) << std::flush;
    };

    while (true) {
      INPUT_RECORD record;
      DWORD eventsRead = 0;
      if (!ReadConsoleInputW(inHandle, &record, 1, &eventsRead) || eventsRead == 0)
        break;

      if (record.EventType == MOUSE_EVENT) {
        MOUSE_EVENT_RECORD& mouse = record.Event.MouseEvent;
        if (mouse.dwEventFlags & MOUSE_WHEELED) {
          int delta = static_cast<int>(mouse.dwButtonState >> 16);
          int scrollDelta = (delta > 0) ? -3 : 3;
          ScrollBy(scrollDelta);
          continue;
        }
        continue;
      }

      if (record.EventType != KEY_EVENT) continue;
      KEY_EVENT_RECORD& key = record.Event.KeyEvent;
      if (!key.bKeyDown) continue;

      if (key.wVirtualKeyCode == VK_PRIOR) {
        ScrollBy(-(msgHeight_ - 4));
        continue;
      }
      if (key.wVirtualKeyCode == VK_NEXT) {
        ScrollBy(msgHeight_ - 4);
        continue;
      }
      if (key.wVirtualKeyCode == VK_UP) {
        ScrollBy(-1);
        continue;
      }
      if (key.wVirtualKeyCode == VK_HOME) {
        scrollOffset_ = 0;
        RefreshMessages();
        continue;
      }
      if (key.wVirtualKeyCode == VK_END) {
        scrollOffset_ = 999999;
        RefreshMessages();
        continue;
      }
      if (key.wVirtualKeyCode == VK_DOWN) {
        ScrollBy(1);
        continue;
      }
      if (key.wVirtualKeyCode == VK_ESCAPE) {
        state_.quitRequested.store(true);
        line = "/exit";
        break;
      }
      if (key.wVirtualKeyCode == VK_RETURN) {
        line = WideToUtf8(wideLine);
        if (!line.empty()) break;
        continue;
      }

      if (key.wVirtualKeyCode == VK_BACK) {
        EraseLastWideCodepoint(&wideLine);
        redrawInput();
        continue;
      }

      const wchar_t wideChar = key.uChar.UnicodeChar;
      if (wideChar == 0 || wideChar == L'\r' || wideChar == L'\n')
        continue;

      for (int i = 0; i < key.wRepeatCount; ++i) {
        wideLine.push_back(wideChar);
      }
      redrawInput();
    }

    line = WideToUtf8(wideLine);
    HideCursor();
    SetCursorPosition(2, inputY);
    WriteAnsi("\x1b[0K");
    std::cout << "> " << std::flush;
    return line;
  }

 private:
  std::vector<std::string> RenderWelcome() {
    std::vector<std::string> lines;
    std::string title, body;
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      title = "cpp-agent  |  C++ Native Agent Kernel  |  " + state_.modelName;
      body = state_.projectRoot + "\n"
           + state_.endpoint + "  (" + state_.protocol + ")\n"
           + "Tools: " + state_.toolsList + "\n"
           + "/help  /status  /tools  /history  /clear  /exit  |  Esc to quit";
    }
    lines.push_back(title);
    lines.push_back("");
    lines.push_back(body);
    return lines;
  }

  void RenderWelcome(int startY) {
    const auto sz = GetConsoleSize();
    const int w = sz.width;
    const auto lines = RenderWelcome();
    const int boxW = w - 2;
    if (boxW < 10) return;

    std::ostringstream ss;
    ss << "\x1b[" << startY << ";" << 0 << "H";
    ss << "\x1b[36;1m";
    ss << (startY == 0 ? "\xe2\x95\xad" : "\xe2\x94\x8c");
    for (int i = 0; i < boxW; ++i) ss << "\xe2\x94\x80";
    ss << "\xe2\x94\x90\x1b[0m";
    ss << "\x1b[" << (startY + 1) << ";" << 0 << "H";
    ss << "\x1b[36m\xe2\x94\x82\x1b[0m "
       << "\x1b[36;1m" << PadRight(TruncateVisually(lines[0], boxW - 2), boxW - 2) << "\x1b[0m"
       << "\x1b[36m\xe2\x94\x82\x1b[0m";
    std::cout << ss.str();

    for (std::size_t li = 1; li < lines.size(); ++li) {
      SetCursorPosition(0, startY + static_cast<int>(li) + 1);
      std::cout << "\x1b[36m\xe2\x94\x82\x1b[0m ";
      std::string display = TruncateVisually(lines[li], boxW - 2);
      std::cout << PadRight(display, boxW - 2);
      std::cout << "\x1b[36m\xe2\x94\x82\x1b[0m";
    }

    int bottomY = startY + static_cast<int>(lines.size());
    SetCursorPosition(0, bottomY);
    std::cout << "\x1b[36m";
    std::cout << (startY == 0 ? "\xe2\x95\xb0" : "\xe2\x94\x98");
    for (int i = 0; i < boxW; ++i) std::cout << "\xe2\x94\x80";
    std::cout << (startY == 0 ? "\xe2\x95\xaf" : "\xe2\x94\x98");
    std::cout << "\x1b[0m";
  }

  void RenderMessageArea(int startY, int height, int width) {
    const int boxW = width - 2;
    if (boxW < 10 || height < 2) return;

    SetCursorPosition(0, startY);
    std::cout << "\x1b[32m\xe2\x94\x8c\xe2\x94\x80 Messages ";
    for (int i = 0; i < boxW - 12; ++i) std::cout << "\xe2\x94\x80";
    std::cout << "\xe2\x94\x90\x1b[0m";

    std::deque<std::string> logLines;
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      logLines = state_.logLines;
    }

    int maxVisible = height - 2;
    if (maxVisible < 1) maxVisible = 1;
    int totalLines = static_cast<int>(logLines.size());
    int maxOffset = totalLines - maxVisible;
    if (maxOffset < 0) maxOffset = 0;
    if (scrollOffset_ > maxOffset) scrollOffset_ = maxOffset;
    std::size_t startIdx = static_cast<std::size_t>(scrollOffset_);

    for (int row = 1; row < height - 2; ++row) {
      SetCursorPosition(0, startY + row);
      std::cout << "\x1b[32m\xe2\x94\x82\x1b[0m ";
      std::size_t idx = startIdx + row - 1;
      if (idx < logLines.size()) {
        std::string line = TruncateVisually(logLines[idx], boxW - 2);
        std::cout << PadRight(line, boxW - 2);
      } else {
        std::cout << PadRight("", boxW - 2);
      }
      std::cout << "\x1b[32m\xe2\x94\x82\x1b[0m";
    }

    SetCursorPosition(0, startY + height - 2);
    std::cout << "\x1b[32m\xe2\x94\x82\x1b[0m ";
    {
       std::ostringstream hint;
       int visibleEnd = scrollOffset_ + maxVisible;
       if (visibleEnd > totalLines) visibleEnd = totalLines;
       hint << "[PgUp/PgDn or mouse]  " << visibleEnd << "/" << totalLines;
      std::string hintStr = hint.str();
      if (static_cast<int>(hintStr.size()) > boxW - 2)
        hintStr = hintStr.substr(0, boxW - 2);
      std::cout << PadRight(hintStr, boxW - 2);
    }
    std::cout << "\x1b[32m\xe2\x94\x82\x1b[0m";

    SetCursorPosition(0, startY + height - 1);
    std::cout << "\x1b[32m\xe2\x94\x94";
    for (int i = 0; i < boxW; ++i) std::cout << "\xe2\x94\x80";
    std::cout << "\xe2\x94\x98\x1b[0m";
  }

  void RenderStatusBar(int y, int width) {
    SetCursorPosition(0, y);
    std::string status;
    bool running;
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      status = state_.statusText;
      running = state_.running.load();
    }
    if (status.empty()) status = "Ready";

    std::ostringstream ss;
    ss << "\x1b[7m"
       << PadRight(" " + status + " ", width - 10)
       << (running ? "\x1b[43m\x1b[30m BUSY \x1b[0m"
                   : "\x1b[42m\x1b[30m IDLE \x1b[0m")
       << "\x1b[27m";
    std::cout << ss.str();
  }

  void RenderInputLine(int y, int width) {
    SetCursorPosition(0, y);
    std::ostringstream ss;
    ss << "\x1b[37m";
    for (int i = 0; i < width; ++i) ss << "\xe2\x94\x80";
    ss << "\x1b[0m";
    std::cout << ss.str();
  }

  TuiState& state_;
  int msgTop_ = 0;
  int msgHeight_ = 0;
  int welcomeLines_ = 0;
  int scrollOffset_ = 0;
};

}  // namespace

int main() {
  const std::string projectRoot = DiscoverProjectRoot();
  const std::string buildDir = JoinPath(projectRoot, "build");
  const std::string sessionDir = JoinPath(buildDir, "session");
  const std::string memoryDir = JoinPath(buildDir, "session-memory");
  EnsureDirectoryRecursive(sessionDir);
  EnsureDirectoryRecursive(memoryDir);

  agent::core::LlmConfig llmCfg;
  llmCfg.apiEndpoint = GetEnvOrDefault(
      "CPP_AGENT_API_ENDPOINT", "http://127.0.0.1:8080/v1/chat/completions");
  llmCfg.mainModel = GetEnvOrDefault(
      "CPP_AGENT_MAIN_MODEL", "Qwen3.6-35B-A3B-UD-Q6_K");
  llmCfg.validatorModel = GetEnvOrDefault(
      "CPP_AGENT_VALIDATOR_MODEL", "gemma-4-31B-it-Q8_0");
  llmCfg.fallbackModel = GetEnvOrDefault(
      "CPP_AGENT_FALLBACK_MODEL", llmCfg.validatorModel);
  llmCfg.connectTimeoutMs = 30000;
  llmCfg.requestTimeoutMs = 120000;

  agent::api::HttpLlmClient httpClient(llmCfg);
  agent::api::SideQueryClient sideQueryClient(httpClient);
  agent::agents::SubAgentManager subAgentManager;
  agent::memory::MemoryIndex memoryIndex(memoryDir);
  agent::tools::ToolOrchestrator toolOrchestrator;
  agent::tools::ToolRegistry toolRegistry;
  agent::permissions::PermissionEngine permissionEngine;
  agent::infra::SessionManager sessionManager(sessionDir);
  agent::infra::StabilityWatchdog watchdog(agent::infra::StabilityConfig{});

  const std::vector<agent::tools::ToolSchema> baseTools =
      agent::tools::ToolRegistry::GetAllBaseTools();
  for (const auto& tool : baseTools)
    toolRegistry.RegisterTool(tool);

  permissionEngine.AddAlwaysAllowRule("FileRead");
  permissionEngine.AddAlwaysAllowRule("Grep");
  permissionEngine.AddAlwaysAllowRule("Glob");
  permissionEngine.AddAutoModeAllowlistedTool("FileRead");
  permissionEngine.AddAutoModeAllowlistedTool("Grep");
  permissionEngine.AddAutoModeAllowlistedTool("Glob");

  toolOrchestrator.SetToolRegistry(&toolRegistry);
  toolOrchestrator.SetSubAgentManager(&subAgentManager);
  subAgentManager.SetMemoryIndex(&memoryIndex);

  agent::core::AgentConfig config = agent::core::AgentConfig::FromDefaults();
  config.systemPrompt =
      "You are a helpful coding agent. Use the available tools to inspect "
      "code, explain findings, and make careful changes when requested.";
  config.defaultModel = llmCfg.mainModel;
  config.memoryRoot = memoryDir;
  config.sessionDir = sessionDir;

  agent::core::QueryEngine engine(
      toolOrchestrator, permissionEngine, httpClient, sideQueryClient,
      toolRegistry, sessionManager);
  engine.SetConfig(config);
  engine.SetModel(llmCfg.mainModel);
  engine.SetFallbackModel(llmCfg.fallbackModel);
  engine.SetValidatorModel(llmCfg.validatorModel);
  engine.SetMemoryIndex(&memoryIndex);
  engine.SetSubAgentManager(&subAgentManager);
  engine.SetSessionDir(sessionDir);

  watchdog.Start();
  engine.SetStabilityWatchdog(&watchdog);

  TuiState tuiState;
  tuiState.projectRoot = projectRoot;
  tuiState.modelName = llmCfg.mainModel;
  tuiState.endpoint = llmCfg.apiEndpoint;
  tuiState.protocol =
      agent::api::HttpLlmClient::IsNativeAnthropicEndpoint(llmCfg.apiEndpoint)
          ? "Anthropic" : "OpenAI-compatible";
  tuiState.toolsList = JoinToolNames(toolRegistry.ListTools());
  tuiState.statusText = "Press /help for commands | Esc to quit";

  AnsiTui tui(tuiState);
  tui.Init();

  tui.AppendMessage("Ready. Type a prompt or command.");

  while (!tuiState.quitRequested.load()) {
    tuiState.statusText = "Ready  |  /help for commands  |  Esc to quit";
    tui.RefreshStatus();

    std::string rawLine = tui.GetInput();
    if (rawLine.empty()) break;

    std::string line = Trim(rawLine);

    if (line == "/exit" || line == "/quit") break;

    if (line == "/help") {
      tui.AppendMessage("/help    Show available commands");
      tui.AppendMessage("/status  Show runtime status");
      tui.AppendMessage("/tools   List registered tools");
      tui.AppendMessage("/history Show recent messages");
      tui.AppendMessage("/clear   Clear message log");
      tui.AppendMessage("/exit    Save session and quit");
      continue;
    }
    if (line == "/status") {
      std::ostringstream ss;
      ss << "Messages: " << engine.messages().size()
         << "  Turns: " << watchdog.metrics().totalTurns
         << "  Healthy: " << (watchdog.IsHealthy() ? "yes" : "no");
      tui.AppendMessage(ss.str());
      ss.str(""); ss.clear();
      ss << "Session: " << sessionDir;
      tui.AppendMessage(ss.str());
      ss.str(""); ss.clear();
      ss << "Memory:  " << memoryDir;
      tui.AppendMessage(ss.str());
      continue;
    }
    if (line == "/tools") {
      for (const auto& tool : toolRegistry.ListTools()) {
        std::ostringstream ss;
        ss << "- " << tool.name
           << "  ro=" << (tool.readOnlyHint ? "Y" : "N")
           << "  " << tool.description;
        tui.AppendMessage(ss.str());
      }
      continue;
    }
    if (line == "/history") {
      const auto& msgs = engine.messages();
      std::size_t start = msgs.size() > 10 ? msgs.size() - 10 : 0;
      for (std::size_t i = start; i < msgs.size(); ++i) {
        tui.AppendMessage("[" + std::to_string(i) + "] " +
                          Shorten(Trim(ExtractText(msgs[i])), 100));
      }
      if (msgs.empty())
        tui.AppendMessage("(no messages yet)");
      continue;
    }
    if (line == "/clear") {
      {
        std::lock_guard<std::mutex> lock(tuiState.mutex);
        tuiState.logLines.clear();
      }
      tui.RefreshMessages();
      continue;
    }

    tui.AppendMessage("\x1b[33m> " + line + "\x1b[0m");
    tuiState.running.store(true);
    tuiState.statusText = "Running: " + Shorten(line, 50);
    tui.RefreshStatus();

    const std::size_t prevCount = engine.messages().size();
    engine.SubmitUserPrompt(line);
    const bool ok = engine.RunTurnWithRecovery();
    sessionManager.PersistSnapshot();

    const auto& msgs = engine.messages();
    for (std::size_t i = prevCount; i < msgs.size(); ++i) {
      std::string text = Trim(ExtractText(msgs[i]));
      if (text.empty()) continue;
      const std::string role =
          msgs[i].role == agent::core::MessageRole::Assistant
              ? "\x1b[36masst\x1b[0m"
          : msgs[i].role == agent::core::MessageRole::System
              ? "\x1b[35msys\x1b[0m"
              : "user";
      for (const auto& lineText : SplitLines(text))
        tui.AppendMessage(role + ": " + lineText);
    }

    if (!ok)
      tui.AppendMessage("\x1b[31m[error] Turn failed.\x1b[0m");

    tuiState.running.store(false);
  }

  tui.Shutdown();
  sessionManager.PersistSnapshot();
  watchdog.Stop();

  return 0;
}
