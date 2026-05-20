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
#include <cstring>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
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

std::string NormalizeSeparators(std::string path) {
  std::replace(path.begin(), path.end(), '/', '\\');
  return path;
}

bool IsAbsolutePath(const std::string& path) {
  if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) &&
      path[1] == ':') {
    return true;
  }
  return path.size() >= 2 &&
         ((path[0] == '\\' && path[1] == '\\') ||
          (path[0] == '/' && path[1] == '/'));
}

std::string GetFullPathString(const std::string& path) {
  if (path.empty()) return std::string();
  char buffer[MAX_PATH] = {0};
  DWORD length = GetFullPathNameA(path.c_str(), MAX_PATH, buffer, nullptr);
  if (length == 0 || length >= MAX_PATH) return std::string();
  return NormalizeSeparators(std::string(buffer, length));
}

void TryMarkDirectoryHidden(const std::string& path) {
  if (path.empty()) return;
  DWORD attrs = GetFileAttributesA(path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) return;
  if ((attrs & FILE_ATTRIBUTE_HIDDEN) != 0) return;
  SetFileAttributesA(path.c_str(), attrs | FILE_ATTRIBUTE_HIDDEN);
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

std::string TrimWhitespace(const std::string& value) {
  const std::string whitespace = " \t\r\n";
  const std::size_t begin = value.find_first_not_of(whitespace);
  if (begin == std::string::npos) return std::string();
  const std::size_t end = value.find_last_not_of(whitespace);
  return value.substr(begin, end - begin + 1);
}

struct WorkspaceSelection {
  std::string launchDir;
  std::string candidateRoot;
  std::string trustedRoot;
  bool trusted = false;
};

bool IsYesValue(const std::string& value) {
  const std::string lowered = TrimWhitespace(value);
  return lowered == "y" || lowered == "Y" || lowered == "yes" ||
         lowered == "YES" || lowered == "Yes" || lowered == "1" ||
         lowered == "true" || lowered == "TRUE" || lowered == "True";
}

bool IsNoValue(const std::string& value) {
  const std::string lowered = TrimWhitespace(value);
  return lowered == "n" || lowered == "N" || lowered == "no" ||
         lowered == "NO" || lowered == "No" || lowered == "0" ||
         lowered == "false" || lowered == "FALSE" || lowered == "False";
}

WorkspaceSelection ResolveWorkspaceSelection() {
  WorkspaceSelection selection;
  selection.launchDir = GetFullPathString(GetCurrentDirectoryString());

  std::string overrideRoot =
      TrimWhitespace(GetEnvOrDefault("CPP_AGENT_WORKSPACE_ROOT", ""));
  if (!overrideRoot.empty() && !IsAbsolutePath(overrideRoot)) {
    overrideRoot = JoinPath(selection.launchDir, overrideRoot);
  }
  selection.candidateRoot = overrideRoot.empty()
                                ? selection.launchDir
                                : GetFullPathString(overrideRoot);
  if (selection.candidateRoot.empty()) {
    selection.candidateRoot = selection.launchDir;
  }

  const std::string trustOverride =
      TrimWhitespace(GetEnvOrDefault("CPP_AGENT_TRUST_WORKSPACE", ""));
  if (!trustOverride.empty()) {
    selection.trusted = !IsNoValue(trustOverride);
    selection.trustedRoot = selection.trusted ? selection.candidateRoot : "";
    return selection;
  }

  DWORD consoleMode = 0;
  const bool interactive =
      GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &consoleMode) != 0;
  if (!interactive) {
    selection.trusted = true;
    selection.trustedRoot = selection.candidateRoot;
    return selection;
  }

  std::cout << "Trust this folder as the project workspace?\n"
            << selection.candidateRoot << "\n"
            << "[Y/n]: " << std::flush;

  std::string answer;
  if (!std::getline(std::cin, answer) || answer.empty() || IsYesValue(answer)) {
    selection.trusted = true;
    selection.trustedRoot = selection.candidateRoot;
    return selection;
  }

  if (IsNoValue(answer)) {
    selection.trusted = false;
    selection.trustedRoot.clear();
    return selection;
  }

  selection.trusted = true;
  selection.trustedRoot = selection.candidateRoot;
  return selection;
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

int CodepointDisplayWidth(wchar_t ch) {
  if (ch == 0) return 0;
  if (ch < 0x20 || (ch >= 0x7F && ch < 0xA0)) return 0;
  if ((ch >= 0x1100 && ch <= 0x115F) ||
      (ch >= 0x2E80 && ch <= 0xA4CF) ||
      (ch >= 0xAC00 && ch <= 0xD7A3) ||
      (ch >= 0xF900 && ch <= 0xFAFF) ||
      (ch >= 0xFE10 && ch <= 0xFE19) ||
      (ch >= 0xFE30 && ch <= 0xFE6F) ||
      (ch >= 0xFF00 && ch <= 0xFF60) ||
      (ch >= 0xFFE0 && ch <= 0xFFE6))
    return 2;
  return 1;
}

std::size_t Utf8CodepointLength(unsigned char ch) {
  if ((ch & 0x80) == 0) return 1;
  if ((ch & 0xE0) == 0xC0) return 2;
  if ((ch & 0xF0) == 0xE0) return 3;
  if ((ch & 0xF8) == 0xF0) return 4;
  return 1;
}

std::size_t ConsumeAnsiSequence(const std::string& s, std::size_t start) {
  std::size_t i = start;
  if (i >= s.size() || s[i] != '\x1b') return start;
  ++i;
  if (i < s.size() && s[i] == '[') {
    ++i;
    while (i < s.size()) {
      const unsigned char ch = static_cast<unsigned char>(s[i]);
      if (ch >= 0x40 && ch <= 0x7E) {
        ++i;
        break;
      }
      ++i;
    }
  }
  return i;
}

int DisplayWidthUtf8(const std::string& s) {
  int width = 0;
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '\x1b') {
      const std::size_t next = ConsumeAnsiSequence(s, i);
      if (next <= i) break;
      i = next;
      continue;
    }
    const std::size_t len =
        Utf8CodepointLength(static_cast<unsigned char>(s[i]));
    const std::wstring wide = Utf8ToWide(s.substr(i, len));
    width += wide.empty() ? 1 : CodepointDisplayWidth(wide[0]);
    i += len;
  }
  return width;
}

std::string TruncateVisually(const std::string& s, int maxWidth) {
  if (maxWidth <= 0) return "";
  int visWidth = 0;
  std::size_t i = 0;
  for (; i < s.size();) {
    if (s[i] == '\x1b') {
      const std::size_t next = ConsumeAnsiSequence(s, i);
      if (next <= i) break;
      i = next;
      continue;
    }
    if (s[i] == '\n') break;
    const std::size_t len =
        Utf8CodepointLength(static_cast<unsigned char>(s[i]));
    const std::wstring wide = Utf8ToWide(s.substr(i, len));
    const int cellWidth = wide.empty() ? 1 : CodepointDisplayWidth(wide[0]);
    if (visWidth + cellWidth > maxWidth) break;
    visWidth += cellWidth;
    i += len;
  }
  return s.substr(0, i);
}

std::string TailTruncateVisually(const std::string& s, int maxWidth) {
  if (maxWidth <= 0) return "";
  if (DisplayWidthUtf8(s) <= maxWidth) return s;

  int width = 0;
  std::size_t start = s.size();
  for (std::size_t i = s.size(); i > 0;) {
    std::size_t begin = i - 1;
    while (begin > 0 &&
           (static_cast<unsigned char>(s[begin]) & 0xC0) == 0x80) {
      --begin;
    }
    if (s[begin] == '\x1b') {
      start = begin;
      i = begin;
      continue;
    }
    const std::wstring wide = Utf8ToWide(s.substr(begin, i - begin));
    const int cellWidth = wide.empty() ? 1 : CodepointDisplayWidth(wide[0]);
    if (width + cellWidth > maxWidth) break;
    width += cellWidth;
    start = begin;
    i = begin;
  }
  return s.substr(start);
}

std::string PadRightVisual(const std::string& s, int width) {
  const int currentWidth = DisplayWidthUtf8(s);
  if (currentWidth >= width) return s;
  return s + std::string(static_cast<std::size_t>(width - currentWidth), ' ');
}

std::vector<std::string> WrapLineVisually(const std::string& s, int maxWidth) {
  std::vector<std::string> wrapped;
  if (maxWidth <= 0) {
    wrapped.push_back("");
    return wrapped;
  }
  if (s.empty()) {
    wrapped.push_back("");
    return wrapped;
  }

  std::string current;
  int currentWidth = 0;
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '\n') {
      wrapped.push_back(current);
      current.clear();
      currentWidth = 0;
      ++i;
      continue;
    }
    if (s[i] == '\x1b') {
      const std::size_t next = ConsumeAnsiSequence(s, i);
      if (next <= i) break;
      current.append(s.substr(i, next - i));
      i = next;
      continue;
    }
    const std::size_t len =
        Utf8CodepointLength(static_cast<unsigned char>(s[i]));
    const std::string chunk = s.substr(i, len);
    const std::wstring wide = Utf8ToWide(chunk);
    const int cellWidth = wide.empty() ? 1 : CodepointDisplayWidth(wide[0]);
    if (currentWidth > 0 && currentWidth + cellWidth > maxWidth) {
      wrapped.push_back(current);
      current.clear();
      currentWidth = 0;
    }
    current.append(chunk);
    currentWidth += cellWidth;
    i += len;
  }
  wrapped.push_back(current);
  return wrapped;
}

std::vector<std::string> BuildDisplayLines(
    const std::deque<std::string>& logLines, int contentWidth) {
  std::vector<std::string> displayLines;
  for (const auto& rawLine : logLines) {
    const auto wrapped = WrapLineVisually(rawLine, std::max(1, contentWidth));
    displayLines.insert(displayLines.end(), wrapped.begin(), wrapped.end());
  }
  return displayLines;
}

bool CopyUtf8ToClipboard(const std::string& text) {
  const std::wstring wide = Utf8ToWide(text);
  if (!OpenClipboard(nullptr)) return false;
  EmptyClipboard();

  const std::size_t bytes =
      (wide.size() + 1) * sizeof(std::wstring::value_type);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (memory == nullptr) {
    CloseClipboard();
    return false;
  }

  void* locked = GlobalLock(memory);
  if (locked == nullptr) {
    GlobalFree(memory);
    CloseClipboard();
    return false;
  }

  std::memcpy(locked, wide.c_str(), bytes);
  GlobalUnlock(memory);

  if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
    GlobalFree(memory);
    CloseClipboard();
    return false;
  }

  CloseClipboard();
  return true;
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
    inputHandle_ = GetStdHandle(STD_INPUT_HANDLE);
    DWORD inputMode = 0;
    if (inputHandle_ != INVALID_HANDLE_VALUE &&
        GetConsoleMode(inputHandle_, &inputMode)) {
      hasOriginalInputMode_ = true;
      originalInputMode_ = inputMode;
      DWORD tuiInputMode =
          (inputMode | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT |
           ENABLE_EXTENDED_FLAGS | ENABLE_PROCESSED_INPUT) &
          ~(ENABLE_QUICK_EDIT_MODE | ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
      SetConsoleMode(inputHandle_, tuiInputMode);
    }
    WriteAnsi("\x1b[?1000h");
    WriteAnsi("\x1b[?1006h");
    RedrawAll();
  }

  void Shutdown() {
    if (hasOriginalInputMode_ && inputHandle_ != INVALID_HANDLE_VALUE)
      SetConsoleMode(inputHandle_, originalInputMode_);
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

  bool CopyMessagesToClipboard() {
    std::ostringstream joined;
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      bool first = true;
      for (const auto& line : state_.logLines) {
        if (!first) joined << "\r\n";
        first = false;
        joined << line;
      }
    }
    return CopyUtf8ToClipboard(joined.str());
  }

  void RefreshMessages() {
    const auto sz = GetConsoleSize();
    int totalLines = 0;
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      totalLines = static_cast<int>(
          BuildDisplayLines(state_.logLines, std::max(1, sz.width - 4)).size());
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
      const std::string visibleInput = TailTruncateVisually(
          WideToUtf8(wideLine), std::max(0, sz.width - 4));
      std::cout << "\x1b[37m> \x1b[0m" << visibleInput << std::flush;
    };

    int historyIndex = -1;
    static std::vector<std::wstring> inputHistory;

    while (true) {
      INPUT_RECORD record;
      DWORD eventsRead = 0;
      if (!ReadConsoleInputW(inHandle, &record, 1, &eventsRead) || eventsRead == 0)
        break;

      if (record.EventType == MOUSE_EVENT) {
        MOUSE_EVENT_RECORD& mouse = record.Event.MouseEvent;
        if (mouse.dwEventFlags & MOUSE_WHEELED) {
          const SHORT wheelDelta =
              static_cast<SHORT>((mouse.dwButtonState >> 16) & 0xFFFF);
          int delta = static_cast<int>(wheelDelta);
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
      if (key.wVirtualKeyCode == VK_UP && !inputHistory.empty()) {
        if (historyIndex == -1) {
          historyIndex = static_cast<int>(inputHistory.size()) - 1;
        } else if (historyIndex > 0) {
          --historyIndex;
        }
        wideLine = inputHistory[historyIndex];
        redrawInput();
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
      if (key.wVirtualKeyCode == VK_DOWN && !inputHistory.empty()) {
        if (historyIndex >= 0 &&
            historyIndex < static_cast<int>(inputHistory.size()) - 1) {
          ++historyIndex;
          wideLine = inputHistory[historyIndex];
        } else {
          historyIndex = -1;
          wideLine.clear();
        }
        redrawInput();
        continue;
      }
      if (key.wVirtualKeyCode == VK_ESCAPE) {
        state_.quitRequested.store(true);
        line = "/exit";
        break;
      }
      if (key.wVirtualKeyCode == VK_RETURN) {
        bool ctrlPressed = (key.dwControlKeyState &
            (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        if (ctrlPressed) {
          wideLine.push_back(L'\n');
          redrawInput();
          continue;
        }
        line = WideToUtf8(wideLine);
        if (!Trim(line).empty()) break;
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

    if (!line.empty()) {
      if (inputHistory.empty() || inputHistory.back() != wideLine) {
        inputHistory.push_back(wideLine);
        if (inputHistory.size() > 100) inputHistory.erase(inputHistory.begin());
      }
    }
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
    std::vector<std::string> displayLines =
        BuildDisplayLines(logLines, std::max(1, boxW - 2));

    int maxVisible = height - 2;
    if (maxVisible < 1) maxVisible = 1;
    int totalLines = static_cast<int>(displayLines.size());
    int maxOffset = totalLines - maxVisible;
    if (maxOffset < 0) maxOffset = 0;
    if (scrollOffset_ > maxOffset) scrollOffset_ = maxOffset;
    std::size_t startIdx = static_cast<std::size_t>(scrollOffset_);

    for (int row = 1; row < height - 2; ++row) {
      SetCursorPosition(0, startY + row);
      std::cout << "\x1b[32m\xe2\x94\x82\x1b[0m ";
      std::size_t idx = startIdx + row - 1;
      if (idx < displayLines.size()) {
        std::string line = TruncateVisually(displayLines[idx], boxW - 2);
        std::cout << PadRightVisual(line, boxW - 2);
      } else {
        std::cout << PadRightVisual("", boxW - 2);
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
      hintStr = TruncateVisually(hintStr, boxW - 2);
      std::cout << PadRightVisual(hintStr, boxW - 2);
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
  HANDLE inputHandle_ = INVALID_HANDLE_VALUE;
  DWORD originalInputMode_ = 0;
  bool hasOriginalInputMode_ = false;
};

}  // namespace

int main() {
  const WorkspaceSelection workspace = ResolveWorkspaceSelection();
  const std::string stateRoot = JoinPath(
      workspace.trusted ? workspace.trustedRoot : workspace.launchDir,
      workspace.trusted ? ".cpp-agent" : ".cpp-agent-untrusted");
  const std::string sessionDir = JoinPath(stateRoot, "session");
  const std::string memoryDir = JoinPath(stateRoot, "memory");
  EnsureDirectoryRecursive(sessionDir);
  EnsureDirectoryRecursive(memoryDir);
  TryMarkDirectoryHidden(stateRoot);
  TryMarkDirectoryHidden(sessionDir);
  TryMarkDirectoryHidden(memoryDir);

  if (workspace.trusted && !workspace.trustedRoot.empty()) {
    SetCurrentDirectoryA(workspace.trustedRoot.c_str());
  }

  agent::core::LlmConfig llmCfg;
  llmCfg.apiEndpoint = GetEnvOrDefault(
      "CPP_AGENT_API_ENDPOINT", "http://127.0.0.1:8080/v1/chat/completions");
  llmCfg.mainModel = GetEnvOrDefault(
      "CPP_AGENT_MAIN_MODEL", "Qwen3.6-35B-A3B-UD-Q6_K");
  llmCfg.validatorModel = GetEnvOrDefault(
      "CPP_AGENT_VALIDATOR_MODEL", "");
  llmCfg.fallbackModel = GetEnvOrDefault(
      "CPP_AGENT_FALLBACK_MODEL", "gemma-4-31B-it-Q8_0");
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
  permissionEngine.AddAlwaysAllowRule("Read");
  permissionEngine.AddAlwaysAllowRule("FileWrite");
  permissionEngine.AddAlwaysAllowRule("Write");
  permissionEngine.AddAlwaysAllowRule("Grep");
  permissionEngine.AddAlwaysAllowRule("Glob");
  permissionEngine.AddAutoModeAllowlistedTool("FileRead");
  permissionEngine.AddAutoModeAllowlistedTool("Read");
  permissionEngine.AddAutoModeAllowlistedTool("FileWrite");
  permissionEngine.AddAutoModeAllowlistedTool("Write");
  permissionEngine.AddAutoModeAllowlistedTool("Grep");
  permissionEngine.AddAutoModeAllowlistedTool("Glob");

  toolOrchestrator.SetToolRegistry(&toolRegistry);
  toolOrchestrator.SetSubAgentManager(&subAgentManager);
  toolOrchestrator.SetWorkspaceRoot(workspace.trusted ? workspace.trustedRoot
                                                      : std::string());
  subAgentManager.SetMemoryIndex(&memoryIndex);

  agent::core::AgentConfig config = agent::core::AgentConfig::FromDefaults();
  std::ostringstream systemPrompt;
  systemPrompt
      << "You are a helpful coding agent. Use the available tools to inspect "
      << "code, explain findings, and make careful changes when requested. ";
  if (workspace.trusted && !workspace.trustedRoot.empty()) {
    systemPrompt
        << "The trusted workspace root is `" << workspace.trustedRoot << "`. "
        << "Treat relative file paths as paths inside this workspace. "
        << "Create and modify project files inside this workspace, not inside "
        << "the session or memory directories unless the user explicitly asks "
        << "you to manage session memory. "
        << "If the user references files outside the workspace, read them via "
        << "an explicit absolute local path or ask the user to copy them into "
        << "the workspace first.";
  } else {
    systemPrompt
        << "No workspace is currently trusted. Do not assume relative paths "
        << "refer to a project; use explicit absolute local paths when the "
        << "user references files outside the current session state.";
  }
  config.systemPrompt = systemPrompt.str();
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
  tuiState.projectRoot = workspace.trusted
                             ? ("Workspace: " + workspace.trustedRoot)
                             : ("Workspace: (untrusted) " + workspace.launchDir);
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
  if (workspace.trusted && !workspace.trustedRoot.empty()) {
    tui.AppendMessage("Trusted workspace: " + workspace.trustedRoot);
  } else {
    tui.AppendMessage(
        "Workspace not trusted. Use absolute paths for external files.");
  }

  while (!tuiState.quitRequested.load()) {
    tuiState.statusText = "Ready  |  /help for commands  |  Esc to quit";
    tui.RefreshStatus();

    std::string rawLine = tui.GetInput();
    if (rawLine.empty()) break;

    std::string line = Trim(rawLine);

    if (line == "/exit" || line == "/quit") break;

    if (line == "/help") {
      tui.AppendMessage("/help      Show available commands");
      tui.AppendMessage("/copy      Copy all messages to clipboard");
      tui.AppendMessage("/model     Set the LLM model (usage: /model <name>)");
      tui.AppendMessage("/permission Set permission mode (/permission <mode>)");
      tui.AppendMessage("/cost      Show token usage and cost estimates");
      tui.AppendMessage("/export    Export all messages to build/session/export.md");
      tui.AppendMessage("/workspace Show trusted workspace and state paths");
      tui.AppendMessage("/memory    Show current memory directory status");
      tui.AppendMessage("/status    Show runtime status");
      tui.AppendMessage("/tools     List registered tools");
      tui.AppendMessage("/history   Show recent messages");
      tui.AppendMessage("/clear     Clear message log");
      tui.AppendMessage("/exit      Save session and quit");
      tui.AppendMessage("Tip: Enter to send, Ctrl+Enter for newline.");
      continue;
    }
    if (line.rfind("/permission ", 0) == 0) {
      std::string mode = Trim(line.substr(12));
      if (mode == "default") {
        permissionEngine.SetPermissionMode(agent::core::PermissionMode::Default);
        tui.AppendMessage("Permission mode: default (interactive)");
      } else if (mode == "auto") {
        permissionEngine.SetPermissionMode(agent::core::PermissionMode::Auto);
        tui.AppendMessage("Permission mode: auto (classifier)");
      } else if (mode == "bypass" || mode == "bypassPermissions") {
        permissionEngine.SetPermissionMode(agent::core::PermissionMode::BypassPermissions);
        tui.AppendMessage("Permission mode: bypass (allow all)");
      } else if (mode == "plan") {
        permissionEngine.SetPermissionMode(agent::core::PermissionMode::Plan);
        tui.AppendMessage("Permission mode: plan (no execution)");
      } else if (mode == "acceptedits" || mode == "acceptEdits") {
        permissionEngine.SetPermissionMode(agent::core::PermissionMode::AcceptEdits);
        tui.AppendMessage("Permission mode: acceptEdits (auto-approve edits)");
      } else {
        tui.AppendMessage("Usage: /permission [default|auto|bypass|plan|acceptedits]");
        tui.AppendMessage("Current: " + mode);
      }
      continue;
    }
    if (line == "/permission") {
      std::string current;
      switch (permissionEngine.GetPermissionMode()) {
        case agent::core::PermissionMode::Default: current = "default"; break;
        case agent::core::PermissionMode::Auto: current = "auto"; break;
        case agent::core::PermissionMode::BypassPermissions: current = "bypass"; break;
        case agent::core::PermissionMode::Plan: current = "plan"; break;
        case agent::core::PermissionMode::AcceptEdits: current = "acceptedits"; break;
        case agent::core::PermissionMode::DontAsk: current = "dontask"; break;
      }
      tui.AppendMessage("Permission mode: " + current);
      tui.AppendMessage("Usage: /permission [default|auto|bypass|plan|acceptedits]");
      continue;
    }
    if (line == "/cost") {
      const auto& ms = engine.messages();
      int totalIn = 0, totalOut = 0, totalCacheRead = 0;
      for (const auto& m : ms) {
        totalIn += m.usage.inputTokens;
        totalOut += m.usage.outputTokens;
        totalCacheRead += m.usage.cacheReadInputTokens;
      }
      std::ostringstream cs;
      cs << "Input:  " << totalIn << " tokens\n"
         << "Output: " << totalOut << " tokens\n"
         << "Cache:  " << totalCacheRead << " read tokens\n"
         << "Est. cost: ~$" << std::fixed
         << std::setprecision(4)
         << ((totalIn * 3.0 + totalOut * 15.0 + totalCacheRead * 0.30) / 1000000.0);
      tui.AppendMessage(cs.str());
      continue;
    }
    if (line == "/export") {
      std::string exportPath = sessionDir + "\\export.md";
      std::ofstream out(exportPath, std::ios::binary | std::ios::trunc);
      if (!out) {
        tui.AppendMessage("[error] Cannot write: " + exportPath);
        continue;
      }
      out << "# Agent Session Export\n\n";
      const auto& ms = engine.messages();
      for (const auto& m : ms) {
        std::string role =
            m.role == agent::core::MessageRole::Assistant ? "assistant" :
            m.role == agent::core::MessageRole::System ? "system" : "user";
        out << "## " << role << "\n\n";
        for (const auto& block : m.content) {
          if (block.type == agent::core::BlockType::Text) {
            out << block.asText.text << "\n\n";
          } else if (block.type == agent::core::BlockType::ToolUse) {
            out << "**tool_use**: " << block.asToolUse.name << "\n\n";
          } else if (block.type == agent::core::BlockType::ToolResult) {
            out << "**tool_result**: ";
            if (static_cast<int>(block.asToolResult.content.size()) > 500)
              out << block.asToolResult.content.substr(0, 500) << "...";
            else
              out << block.asToolResult.content;
            out << "\n\n";
          }
        }
      }
      tui.AppendMessage("[exported] " + exportPath);
      continue;
    }
    if (line.rfind("/model ", 0) == 0) {
      std::string newModel = Trim(line.substr(7));
      if (!newModel.empty()) {
        engine.SetModel(newModel);
        tuiState.modelName = newModel;
        tui.AppendMessage("Model set to: " + newModel);
      } else {
        tui.AppendMessage("Current model: " + tuiState.modelName);
      }
      continue;
    }
    if (line == "/model") {
      tui.AppendMessage("Current model: " + tuiState.modelName);
      tui.AppendMessage("Usage: /model <name>  (e.g. /model claude-sonnet)");
      continue;
    }
    if (line == "/memory") {
      tui.AppendMessage("Memory dir: " + memoryDir);
      std::string entrypoint = memoryIndex.ReadEntrypoint();
      if (entrypoint.empty()) {
        tui.AppendMessage("MEMORY.md: (empty)");
      } else {
        tui.AppendMessage("MEMORY.md: " + std::to_string(entrypoint.size()) +
                          " bytes");
      }
      continue;
    }
    if (line == "/workspace") {
      tui.AppendMessage(workspace.trusted
                            ? ("Trusted workspace: " + workspace.trustedRoot)
                            : ("Trusted workspace: (none)"));
      tui.AppendMessage("Session dir: " + sessionDir);
      tui.AppendMessage("Memory dir:  " + memoryDir);
      continue;
    }
    if (line == "/copy") {
      if (tui.CopyMessagesToClipboard())
        tui.AppendMessage("[copied] Messages copied to clipboard.");
      else
        tui.AppendMessage("[error] Failed to copy messages to clipboard.");
      continue;
    }
    if (line == "/status") {
      std::ostringstream ss;
      ss << "Messages: " << engine.messages().size()
         << "  Turns: " << watchdog.metrics().totalTurns
         << "  Healthy: " << (watchdog.IsHealthy() ? "yes" : "no");
      tui.AppendMessage(ss.str());
      ss.str(""); ss.clear();
      ss << "Workspace: "
         << (workspace.trusted ? workspace.trustedRoot : "(untrusted)");
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

    tui.AppendMessage("> " + line);
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
              ? "asst"
          : msgs[i].role == agent::core::MessageRole::System
              ? "sys"
              : "user";
      for (const auto& lineText : SplitLines(text))
        tui.AppendMessage(role + ": " + lineText);
    }

    if (!ok)
      tui.AppendMessage("[error] Turn failed.");

    tuiState.running.store(false);
  }

  tui.Shutdown();
  sessionManager.PersistSnapshot();
  watchdog.Stop();

  return 0;
}
