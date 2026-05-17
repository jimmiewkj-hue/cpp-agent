#include "infra/ProcessRunner.h"

#include <windows.h>

#include <string>
#include <vector>

namespace agent {
namespace infra {

namespace {

std::string ReadFromPipe(HANDLE pipeHandle) {
  std::string output;
  char buffer[4096];
  DWORD bytesRead = 0;

  while (ReadFile(pipeHandle, buffer, sizeof(buffer), &bytesRead, nullptr) &&
         bytesRead > 0) {
    output.append(buffer, buffer + bytesRead);
  }

  return output;
}

}  // namespace

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
    result.errorMessage = "CreatePipe failed";
    return result;
  }

  SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startupInfo;
  ZeroMemory(&startupInfo, sizeof(startupInfo));
  startupInfo.cb = sizeof(startupInfo);
  startupInfo.dwFlags = STARTF_USESTDHANDLES;
  startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
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

  const BOOL created = CreateProcessW(
      nullptr,
      mutableCommand.data(),
      nullptr,
      nullptr,
      TRUE,
      CREATE_NO_WINDOW,
      nullptr,
      workingDirPtr,
      &startupInfo,
      &processInfo);

  CloseHandle(stdoutWrite);
  stdoutWrite = nullptr;

  if (!created) {
    result.spawnFailed = true;
    result.errorMessage = "CreateProcessW failed";
    CloseHandle(stdoutRead);
    return result;
  }

  const DWORD waitCode =
      WaitForSingleObject(processInfo.hProcess, options.timeoutMs);
  if (waitCode == WAIT_TIMEOUT) {
    result.timedOut = true;
    TerminateProcess(processInfo.hProcess, 124);
    WaitForSingleObject(processInfo.hProcess, 5000);
  }

  DWORD exitCode = 0;
  GetExitCodeProcess(processInfo.hProcess, &exitCode);
  result.exitCode = static_cast<int>(exitCode);

  result.stdoutText = ReadFromPipe(stdoutRead);

  CloseHandle(stdoutRead);
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
