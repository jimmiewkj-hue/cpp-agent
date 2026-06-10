#include "infra/ProcessRunner.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace agent {
namespace infra {

ProcessRunResult ProcessRunner::Run(const ProcessRunOptions& options) const {
  ProcessRunResult result;

  SECURITY_ATTRIBUTES securityAttributes;
  securityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
  securityAttributes.lpSecurityDescriptor = nullptr;
  securityAttributes.bInheritHandle = TRUE;

  HANDLE stdoutRead = nullptr;
  HANDLE stdoutWrite = nullptr;
  if (!CreatePipe(&stdoutRead, &stdoutWrite, &securityAttributes, 0)) {
    result.spawnFailed = true;
    result.errorMessage = "CreatePipe stdout failed";
    return result;
  }
  SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

  HANDLE stdinRead = nullptr;
  HANDLE stdinWrite = nullptr;
  if (!options.stdinData.empty()) {
    if (!CreatePipe(&stdinRead, &stdinWrite, &securityAttributes, 0)) {
      result.spawnFailed = true;
      result.errorMessage = "CreatePipe stdin failed";
      CloseHandle(stdoutRead);
      CloseHandle(stdoutWrite);
      return result;
    }
    SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
  }

  STARTUPINFOW startupInfo;
  ZeroMemory(&startupInfo, sizeof(startupInfo));
  startupInfo.cb = sizeof(startupInfo);
  startupInfo.dwFlags = STARTF_USESTDHANDLES;
  startupInfo.hStdInput = options.stdinData.empty()
                              ? GetStdHandle(STD_INPUT_HANDLE)
                              : stdinRead;
  startupInfo.hStdOutput = stdoutWrite;
  startupInfo.hStdError = stdoutWrite;

  PROCESS_INFORMATION processInfo;
  ZeroMemory(&processInfo, sizeof(processInfo));

  std::wstring commandLine = BuildCommandLine(options);
  std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
  mutableCommand.push_back(L'\0');

  const std::wstring workingDirectory = options.workingDirectory.empty()
                                            ? L""
                                            : ToWide(options.workingDirectory);
  LPCWSTR workingDirPtr =
      workingDirectory.empty() ? nullptr : workingDirectory.c_str();

  std::vector<wchar_t> envBlock;
  LPVOID envPtr = nullptr;
  if (!options.envVars.empty()) {
    std::wstring envStr;
    for (const auto& kv : options.envVars) {
      envStr += ToWide(kv.first);
      envStr.push_back(L'=');
      envStr += ToWide(kv.second);
      envStr.push_back(L'\0');
    }
    envStr.push_back(L'\0');
    envBlock.assign(envStr.begin(), envStr.end());
    envPtr = envBlock.data();
  }

  const BOOL created = CreateProcessW(
      nullptr,
      mutableCommand.data(),
      nullptr,
      nullptr,
      TRUE,
      CREATE_NO_WINDOW | (options.envVars.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT),
      envPtr,
      workingDirPtr,
      &startupInfo,
      &processInfo);

  CloseHandle(stdoutWrite);
  stdoutWrite = nullptr;
  if (stdinRead != nullptr) { CloseHandle(stdinRead); stdinRead = nullptr; }

  if (!created) {
    result.spawnFailed = true;
    result.errorMessage = "CreateProcessW failed";
    CloseHandle(stdoutRead);
    if (stdinWrite != nullptr) { CloseHandle(stdinWrite); stdinWrite = nullptr; }
    return result;
  }

  if (!options.stdinData.empty() && stdinWrite != nullptr) {
    DWORD written = 0;
    WriteFile(stdinWrite, options.stdinData.data(),
              static_cast<DWORD>(options.stdinData.size()), &written, nullptr);
    CloseHandle(stdinWrite);
    stdinWrite = nullptr;
  }

  // CRITICAL FIX: Read stdout concurrently to prevent pipe-buffer deadlock.
  // When the child process writes more output than the pipe buffer (~4KB on
  // Windows), it blocks on WriteFile waiting for the buffer to drain. If the
  // parent is in WaitForSingleObject waiting for the child to exit, neither
  // can proceed — classic deadlock. Solution: spawn a reader thread that
  // drains the pipe while we wait for the process.
  std::atomic<bool> processDone{false};
  std::string collectedStdout;

  std::thread stdoutReader([&stdoutRead, &processDone, &collectedStdout]() {
    char buffer[4096];
    for (;;) {
      DWORD bytesAvailable = 0;
      if (!PeekNamedPipe(stdoutRead, nullptr, 0, nullptr,
                         &bytesAvailable, nullptr)) {
        break;  // Pipe broken or error
      }
      if (bytesAvailable > 0) {
        DWORD bytesRead = 0;
        DWORD toRead =
            (bytesAvailable < sizeof(buffer)) ? bytesAvailable : sizeof(buffer);
        if (!ReadFile(stdoutRead, buffer, toRead, &bytesRead, nullptr) ||
            bytesRead == 0) {
          break;
        }
        collectedStdout.append(buffer, bytesRead);
      } else if (processDone.load()) {
        break;  // Process exited and no more data in pipe
      } else {
        Sleep(5);  // Brief sleep to avoid busy-wait
      }
    }
  });

  const DWORD waitCode =
      WaitForSingleObject(processInfo.hProcess, options.timeoutMs);
  if (waitCode == WAIT_TIMEOUT) {
    result.timedOut = true;
    TerminateProcess(processInfo.hProcess, 124);
    WaitForSingleObject(processInfo.hProcess, 5000);
  }

  // Signal the reader thread that the process has exited, then wait for it
  // to finish draining any remaining data from the pipe.
  processDone.store(true);
  if (stdoutReader.joinable()) stdoutReader.join();

  DWORD exitCode = 0;
  GetExitCodeProcess(processInfo.hProcess, &exitCode);
  result.exitCode = static_cast<int>(exitCode);

  result.stdoutText = std::move(collectedStdout);

  CloseHandle(stdoutRead);
  if (stdinWrite != nullptr) { CloseHandle(stdinWrite); }
  CloseHandle(processInfo.hThread);
  CloseHandle(processInfo.hProcess);
  return result;
}

std::wstring ProcessRunner::BuildCommandLine(
    const ProcessRunOptions& options) const {
  std::wstring commandLine = QuoteWindowsArg(options.executable);
  for (const auto& argument : options.arguments) {
    commandLine.append(L" ");
    commandLine.append(QuoteWindowsArg(argument));
  }
  return commandLine;
}

std::wstring ProcessRunner::ToWide(const std::string& text) const {
  if (text.empty()) return std::wstring();

  const int size = MultiByteToWideChar(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &wide[0], size);
  return wide;
}

std::string ProcessRunner::ToUtf8(const std::wstring& text) const {
  if (text.empty()) return std::string();

  const int size = WideCharToMultiByte(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
      nullptr, nullptr);
  std::string utf8(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &utf8[0], size,
      nullptr, nullptr);
  return utf8;
}

std::wstring ProcessRunner::QuoteWindowsArg(const std::string& value) const {
  const std::wstring wide = ToWide(value);
  if (wide.find_first_of(L" \t\"") == std::wstring::npos) {
    return wide;
  }

  std::wstring quoted;
  quoted.push_back(L'"');
  for (wchar_t ch : wide) {
    if (ch == L'"') {
      quoted.append(L"\\\"");
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back(L'"');
  return quoted;
}

}  // namespace infra
}  // namespace agent
