#include "mcp/McpClientManager.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace agent {
namespace mcp {

namespace {

static const int kDefaultTransportTimeoutMs = 15000;
static const char kMcpProtocolVersion[] = "2025-11-25";

std::string Trim(const std::string& value) {
  const std::string whitespace = " \t\r\n";
  const std::size_t start = value.find_first_not_of(whitespace);
  if (start == std::string::npos) {
    return std::string();
  }
  const std::size_t end = value.find_last_not_of(whitespace);
  return value.substr(start, end - start + 1);
}

std::wstring ToWide(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int size = MultiByteToWideChar(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &wide[0], size);
  return wide;
}

std::string ToUtf8(const std::wstring& text) {
  if (text.empty()) {
    return std::string();
  }
  const int size = WideCharToMultiByte(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
      nullptr, nullptr);
  std::string utf8(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &utf8[0], size,
      nullptr, nullptr);
  return utf8;
}

std::wstring QuoteWindowsArg(const std::string& value) {
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

std::vector<std::string> SplitCommandLine(const std::string& commandLine) {
  std::vector<std::string> parts;
  std::string current;
  bool inQuotes = false;

  for (std::size_t i = 0; i < commandLine.size(); ++i) {
    const char ch = commandLine[i];
    if (ch == '"') {
      inQuotes = !inQuotes;
      continue;
    }
    if (!inQuotes && (ch == ' ' || ch == '\t')) {
      if (!current.empty()) {
        parts.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }

  if (!current.empty()) {
    parts.push_back(current);
  }
  return parts;
}

std::wstring BuildCommandLine(
    const std::string& executable,
    const std::vector<std::string>& arguments) {
  std::wstring commandLine = QuoteWindowsArg(executable);
  for (const auto& argument : arguments) {
    commandLine.append(L" ");
    commandLine.append(QuoteWindowsArg(argument));
  }
  return commandLine;
}

std::size_t SkipWhitespace(const std::string& json, std::size_t pos) {
  while (pos < json.size() &&
         (json[pos] == ' ' || json[pos] == '\n' || json[pos] == '\r' ||
          json[pos] == '\t')) {
    ++pos;
  }
  return pos;
}

bool ExtractJsonFieldValue(
    const std::string& json,
    const std::string& field,
    std::size_t* valueStart) {
  const std::string token = "\"" + field + "\"";
  const std::size_t fieldPos = json.find(token);
  if (fieldPos == std::string::npos) {
    return false;
  }

  const std::size_t colonPos = json.find(':', fieldPos + token.size());
  if (colonPos == std::string::npos) {
    return false;
  }

  *valueStart = SkipWhitespace(json, colonPos + 1);
  return *valueStart < json.size();
}

bool ExtractJsonStringField(
    const std::string& json,
    const std::string& field,
    std::string* value) {
  std::size_t valueStart = 0;
  if (!ExtractJsonFieldValue(json, field, &valueStart) ||
      json[valueStart] != '"') {
    return false;
  }

  std::ostringstream output;
  bool escape = false;
  for (std::size_t i = valueStart + 1; i < json.size(); ++i) {
    const char ch = json[i];
    if (escape) {
      switch (ch) {
        case 'n':
          output << '\n';
          break;
        case 'r':
          output << '\r';
          break;
        case 't':
          output << '\t';
          break;
        default:
          output << ch;
          break;
      }
      escape = false;
      continue;
    }
    if (ch == '\\') {
      escape = true;
      continue;
    }
    if (ch == '"') {
      *value = output.str();
      return true;
    }
    output << ch;
  }

  return false;
}

bool ExtractJsonBoolField(
    const std::string& json,
    const std::string& field,
    bool defaultValue) {
  std::size_t valueStart = 0;
  if (!ExtractJsonFieldValue(json, field, &valueStart)) {
    return defaultValue;
  }
  if (json.compare(valueStart, 4, "true") == 0) {
    return true;
  }
  if (json.compare(valueStart, 5, "false") == 0) {
    return false;
  }
  return defaultValue;
}

bool ExtractJsonIntField(
    const std::string& json,
    const std::string& field,
    int* value) {
  if (value == nullptr) {
    return false;
  }
  std::size_t valueStart = 0;
  if (!ExtractJsonFieldValue(json, field, &valueStart)) {
    return false;
  }
  std::size_t end = valueStart;
  if (end < json.size() && json[end] == '-') {
    ++end;
  }
  while (end < json.size() &&
         std::isdigit(static_cast<unsigned char>(json[end]))) {
    ++end;
  }
  if (end == valueStart || (end == valueStart + 1 && json[valueStart] == '-')) {
    return false;
  }
  *value = std::atoi(json.substr(valueStart, end - valueStart).c_str());
  return true;
}

bool ExtractJsonObjectField(
    const std::string& json,
    const std::string& field,
    std::string* value) {
  std::size_t valueStart = 0;
  if (!ExtractJsonFieldValue(json, field, &valueStart) ||
      json[valueStart] != '{') {
    return false;
  }

  int depth = 0;
  bool inString = false;
  bool escape = false;
  for (std::size_t i = valueStart; i < json.size(); ++i) {
    const char ch = json[i];
    if (inString) {
      if (escape) {
        escape = false;
      } else if (ch == '\\') {
        escape = true;
      } else if (ch == '"') {
        inString = false;
      }
      continue;
    }
    if (ch == '"') {
      inString = true;
      continue;
    }
    if (ch == '{') {
      ++depth;
    } else if (ch == '}') {
      --depth;
      if (depth == 0) {
        *value = json.substr(valueStart, i - valueStart + 1);
        return true;
      }
    }
  }

  return false;
}

bool ExtractJsonArrayField(
    const std::string& json,
    const std::string& field,
    std::string* value) {
  std::size_t valueStart = 0;
  if (!ExtractJsonFieldValue(json, field, &valueStart) ||
      json[valueStart] != '[') {
    return false;
  }

  int depth = 0;
  bool inString = false;
  bool escape = false;
  for (std::size_t i = valueStart; i < json.size(); ++i) {
    const char ch = json[i];
    if (inString) {
      if (escape) {
        escape = false;
      } else if (ch == '\\') {
        escape = true;
      } else if (ch == '"') {
        inString = false;
      }
      continue;
    }
    if (ch == '"') {
      inString = true;
      continue;
    }
    if (ch == '[') {
      ++depth;
    } else if (ch == ']') {
      --depth;
      if (depth == 0) {
        *value = json.substr(valueStart, i - valueStart + 1);
        return true;
      }
    }
  }

  return false;
}

std::vector<std::string> SplitTopLevelObjects(const std::string& arrayJson) {
  std::vector<std::string> objects;
  int depth = 0;
  bool inString = false;
  bool escape = false;
  std::size_t objectStart = std::string::npos;

  for (std::size_t i = 0; i < arrayJson.size(); ++i) {
    const char ch = arrayJson[i];
    if (inString) {
      if (escape) {
        escape = false;
      } else if (ch == '\\') {
        escape = true;
      } else if (ch == '"') {
        inString = false;
      }
      continue;
    }

    if (ch == '"') {
      inString = true;
      continue;
    }
    if (ch == '{') {
      if (depth == 0) {
        objectStart = i;
      }
      ++depth;
    } else if (ch == '}') {
      --depth;
      if (depth == 0 && objectStart != std::string::npos) {
        objects.push_back(arrayJson.substr(objectStart, i - objectStart + 1));
        objectStart = std::string::npos;
      }
    }
  }

  return objects;
}

std::string EscapeJsonString(const std::string& value) {
  std::ostringstream output;
  for (char ch : value) {
    switch (ch) {
      case '\\':
        output << "\\\\";
        break;
      case '"':
        output << "\\\"";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        output << ch;
        break;
    }
  }
  return output.str();
}

std::string BuildJsonRpcRequest(
    int id,
    const std::string& method,
    const std::string& paramsJson) {
  return std::string("{\"jsonrpc\":\"2.0\",\"id\":") + std::to_string(id) +
         ",\"method\":\"" + EscapeJsonString(method) + "\",\"params\":" +
         (paramsJson.empty() ? "{}" : paramsJson) + "}";
}

std::string BuildJsonRpcNotification(
    const std::string& method,
    const std::string& paramsJson) {
  return std::string("{\"jsonrpc\":\"2.0\",\"method\":\"") +
         EscapeJsonString(method) + "\",\"params\":" +
         (paramsJson.empty() ? "{}" : paramsJson) + "}";
}

std::string BuildInitializeParams() {
  return std::string(
      "{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{\"roots\":{},"
      "\"sampling\":{}},\"clientInfo\":{\"name\":\"cpp-agent\",\"version\":\"0.1.0\"}}");
}

long long NowUnixMs() {
  return static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

bool IsLikelySessionId(const std::string& value) {
  if (value.empty() || value.size() > 255) {
    return false;
  }
  for (char ch : value) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c < 0x21 || c > 0x7E) {
      return false;
    }
  }
  return true;
}

std::string GenerateSessionToken() {
  std::ostringstream stream;
  stream << std::hex << std::nouppercase << NowUnixMs() << "-"
         << GetCurrentProcessId() << "-" << std::rand() << std::rand();
  return stream.str();
}

bool QueryHeaderString(
    HINTERNET request,
    DWORD queryFlag,
    LPCWSTR headerName,
    std::wstring* value) {
  if (value == nullptr) {
    return false;
  }

  DWORD sizeBytes = 0;
  WinHttpQueryHeaders(
      request, queryFlag, headerName, WINHTTP_NO_OUTPUT_BUFFER, &sizeBytes,
      WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || sizeBytes == 0) {
    return false;
  }

  std::vector<wchar_t> buffer(sizeBytes / sizeof(wchar_t));
  if (!WinHttpQueryHeaders(request, queryFlag, headerName, &buffer[0], &sizeBytes,
                           WINHTTP_NO_HEADER_INDEX)) {
    return false;
  }

  std::size_t charCount = sizeBytes / sizeof(wchar_t);
  while (charCount > 0 && buffer[charCount - 1] == L'\0') {
    --charCount;
  }
  value->assign(buffer.data(), charCount);
  return true;
}

std::string ToLowerAscii(const std::string& text) {
  std::string lowered = text;
  for (char& ch : lowered) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return lowered;
}

bool TryGetRawHeaderValue(
    HINTERNET request,
    const std::string& headerName,
    std::string* value) {
  if (value == nullptr) {
    return false;
  }

  std::wstring rawHeadersWide;
  if (!QueryHeaderString(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                         WINHTTP_HEADER_NAME_BY_INDEX, &rawHeadersWide)) {
    return false;
  }

  const std::string rawHeaders = ToUtf8(rawHeadersWide);
  std::istringstream stream(rawHeaders);
  const std::string expectedPrefix = ToLowerAscii(headerName) + ":";
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::string trimmed = Trim(line);
    if (trimmed.size() < expectedPrefix.size()) {
      continue;
    }
    const std::string lowered = ToLowerAscii(trimmed);
    if (lowered.compare(0, expectedPrefix.size(), expectedPrefix) == 0) {
      *value = Trim(trimmed.substr(expectedPrefix.size()));
      return !value->empty();
    }
  }
  return false;
}

bool QueryStatusCode(HINTERNET request, DWORD* statusCode) {
  if (statusCode == nullptr) {
    return false;
  }
  DWORD size = sizeof(DWORD);
  return WinHttpQueryHeaders(
      request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX, statusCode, &size,
      WINHTTP_NO_HEADER_INDEX) == TRUE;
}

bool IsJsonRpcSessionExpiredBody(const std::string& bodyJson) {
  return bodyJson.find("\"code\":-32001") != std::string::npos &&
         (bodyJson.find("session") != std::string::npos ||
          bodyJson.find("Session") != std::string::npos);
}

std::string DecodeEventStreamPayload(const std::string& rawBody) {
  std::istringstream stream(rawBody);
  std::string line;
  std::string currentData;
  std::string lastJson;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      const std::string trimmed = Trim(currentData);
      if (!trimmed.empty() && trimmed != "[DONE]") {
        lastJson = trimmed;
      }
      currentData.clear();
      continue;
    }
    if (line.find("data:") == 0) {
      if (!currentData.empty()) {
        currentData.push_back('\n');
      }
      currentData += Trim(line.substr(5));
    }
  }
  if (lastJson.empty()) {
    const std::string trimmed = Trim(currentData);
    if (!trimmed.empty() && trimmed != "[DONE]") {
      lastJson = trimmed;
    }
  }
  return lastJson;
}

std::string DecodeNdjsonPayload(const std::string& rawBody) {
  std::istringstream stream(rawBody);
  std::string line;
  std::string lastJson;
  while (std::getline(stream, line)) {
    const std::string trimmed = Trim(line);
    if (!trimmed.empty()) {
      lastJson = trimmed;
    }
  }
  return lastJson;
}

std::string NormalizeContentType(const std::string& contentType) {
  std::string trimmed = Trim(contentType);
  std::string normalized;
  normalized.reserve(trimmed.size());
  for (char ch : trimmed) {
    normalized.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch))));
  }
  const std::size_t semicolon = normalized.find(';');
  if (semicolon != std::string::npos) {
    normalized = normalized.substr(0, semicolon);
  }
  normalized = Trim(normalized);
  return normalized;
}

std::string DecodeStreamableHttpPayload(
    const std::string& contentType,
    const std::string& rawBody) {
  const std::string mediaType = NormalizeContentType(contentType);
  if (mediaType == "text/event-stream") {
    return DecodeEventStreamPayload(rawBody);
  }
  if (mediaType == "application/x-ndjson" ||
      mediaType == "application/jsonl") {
    return DecodeNdjsonPayload(rawBody);
  }
  return rawBody;
}

struct SseEventFrame {
  std::string eventId;
  std::string data;
  int retryMs = 0;
};

void AppendSseDataLine(std::string* currentData, const std::string& value) {
  if (currentData == nullptr) {
    return;
  }
  if (!currentData->empty()) {
    currentData->push_back('\n');
  }
  currentData->append(value);
}

void FinalizeSseEvent(
    std::string* currentId,
    std::string* currentData,
    int* currentRetryMs,
    std::vector<SseEventFrame>* events) {
  if (currentId == nullptr || currentData == nullptr ||
      currentRetryMs == nullptr || events == nullptr) {
    return;
  }
  if (currentId->empty() && currentData->empty() && *currentRetryMs == 0) {
    return;
  }
  SseEventFrame event;
  event.eventId = *currentId;
  event.data = Trim(*currentData);
  event.retryMs = *currentRetryMs;
  events->push_back(event);
  currentId->clear();
  currentData->clear();
  *currentRetryMs = 0;
}

void ParseSseChunk(
    const std::string& chunk,
    std::string* carry,
    std::string* currentId,
    std::string* currentData,
    int* currentRetryMs,
    std::vector<SseEventFrame>* events) {
  if (carry == nullptr || currentId == nullptr || currentData == nullptr ||
      currentRetryMs == nullptr || events == nullptr) {
    return;
  }
  carry->append(chunk);
  std::size_t lineStart = 0;
  while (true) {
    const std::size_t lineEnd = carry->find('\n', lineStart);
    if (lineEnd == std::string::npos) {
      break;
    }
    std::string line = carry->substr(lineStart, lineEnd - lineStart);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lineStart = lineEnd + 1;
    if (line.empty()) {
      FinalizeSseEvent(currentId, currentData, currentRetryMs, events);
      continue;
    }
    if (line[0] == ':') {
      continue;
    }
    const std::size_t colon = line.find(':');
    const std::string field = colon == std::string::npos ? line : line.substr(0, colon);
    std::string value =
        colon == std::string::npos ? std::string() : line.substr(colon + 1);
    if (!value.empty() && value[0] == ' ') {
      value.erase(value.begin());
    }
    if (field == "id") {
      *currentId = value;
    } else if (field == "data") {
      AppendSseDataLine(currentData, value);
    } else if (field == "retry") {
      *currentRetryMs = std::max(0, std::atoi(value.c_str()));
    }
  }
  carry->erase(0, lineStart);
}

bool IsJsonRpcResponseForId(const std::string& bodyJson, int requestId) {
  int responseId = 0;
  return ExtractJsonIntField(bodyJson, "id", &responseId) &&
         responseId == requestId;
}

bool ParseInitializeResponse(
    const std::string& bodyJson,
    McpServerCapabilities* capabilities,
    std::string* instructions,
    std::string* serverVersion,
    std::string* error) {
  if (capabilities == nullptr || instructions == nullptr ||
      serverVersion == nullptr || error == nullptr) {
    return false;
  }

  std::string result;
  if (!ExtractJsonObjectField(bodyJson, "result", &result)) {
    *error = "initialize response missing result object";
    return false;
  }

  std::string capabilitiesJson;
  if (ExtractJsonObjectField(result, "capabilities", &capabilitiesJson)) {
    std::string ignored;
    capabilities->tools =
        ExtractJsonObjectField(capabilitiesJson, "tools", &ignored);
    capabilities->prompts =
        ExtractJsonObjectField(capabilitiesJson, "prompts", &ignored);
    std::string resourcesJson;
    capabilities->resources =
        ExtractJsonObjectField(capabilitiesJson, "resources", &resourcesJson);
    if (capabilities->resources) {
      capabilities->resourcesSubscribe =
          ExtractJsonBoolField(resourcesJson, "subscribe", false);
    }
    capabilities->elicitation =
        ExtractJsonObjectField(capabilitiesJson, "elicitation", &ignored);
  }

  ExtractJsonStringField(result, "instructions", instructions);
  std::string serverInfo;
  if (ExtractJsonObjectField(result, "serverInfo", &serverInfo)) {
    ExtractJsonStringField(serverInfo, "version", serverVersion);
  }
  return true;
}

std::string ExtractToolsPayload(const std::string& bodyJson) {
  std::string result;
  if (ExtractJsonObjectField(bodyJson, "result", &result)) {
    return result;
  }
  return bodyJson;
}

bool WriteAll(HANDLE handle, const std::string& data) {
  const char* cursor = data.c_str();
  DWORD remaining = static_cast<DWORD>(data.size());
  while (remaining > 0) {
    DWORD written = 0;
    if (!WriteFile(handle, cursor, remaining, &written, nullptr)) {
      return false;
    }
    cursor += written;
    remaining -= written;
  }
  return true;
}

bool ReadFramedMessage(
    HANDLE pipeHandle,
    HANDLE processHandle,
    DWORD timeoutMs,
    std::string* body,
    std::string* error) {
  std::string buffer;
  const DWORD started = GetTickCount();
  int contentLength = -1;

  while (true) {
    const std::size_t headerEnd = buffer.find("\r\n\r\n");
    if (headerEnd != std::string::npos) {
      const std::string header = buffer.substr(0, headerEnd);
      const std::size_t marker = header.find("Content-Length:");
      if (marker == std::string::npos) {
        *error = "stdio MCP response missing Content-Length header";
        return false;
      }
      contentLength = std::atoi(header.substr(marker + 15).c_str());
      const std::size_t bodyStart = headerEnd + 4;
      if (buffer.size() >= bodyStart + static_cast<std::size_t>(contentLength)) {
        *body = buffer.substr(bodyStart, static_cast<std::size_t>(contentLength));
        return true;
      }
    }

    if (GetTickCount() - started > timeoutMs) {
      *error = "stdio MCP response timed out";
      return false;
    }

    DWORD available = 0;
    if (!PeekNamedPipe(pipeHandle, nullptr, 0, nullptr, &available, nullptr)) {
      *error = "PeekNamedPipe failed while waiting for MCP response";
      return false;
    }
    if (available == 0) {
      if (WaitForSingleObject(processHandle, 25) == WAIT_OBJECT_0) {
        *error = "MCP stdio process exited before sending a response";
        return false;
      }
      Sleep(10);
      continue;
    }

    char chunk[4096];
    DWORD bytesRead = 0;
    if (!ReadFile(pipeHandle, chunk, sizeof(chunk), &bytesRead, nullptr)) {
      *error = "ReadFile failed while reading MCP stdio response";
      return false;
    }
    buffer.append(chunk, chunk + bytesRead);
  }
}

class StdIoMcpTransport : public McpTransport {
 public:
  ~StdIoMcpTransport() override { Close(); }

  bool Connect(const McpServerConfig& config, std::string* error) override {
    config_ = config;
    const std::vector<std::string> parts = SplitCommandLine(config.endpoint);
    if (parts.empty()) {
      if (error) {
        *error = "stdio transport endpoint is empty";
      }
      return false;
    }

    SECURITY_ATTRIBUTES securityAttributes;
    securityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
    securityAttributes.lpSecurityDescriptor = nullptr;
    securityAttributes.bInheritHandle = TRUE;

    HANDLE stdoutWrite = nullptr;
    HANDLE stdinRead = nullptr;
    if (!CreatePipe(&stdoutRead_, &stdoutWrite, &securityAttributes, 0) ||
        !CreatePipe(&stdinRead, &stdinWrite_, &securityAttributes, 0)) {
      if (error) {
        *error = "CreatePipe failed for stdio transport";
      }
      Close();
      return false;
    }

    SetHandleInformation(stdoutRead_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdinWrite_, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startupInfo;
    ZeroMemory(&startupInfo, sizeof(startupInfo));
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = stdinRead;
    startupInfo.hStdOutput = stdoutWrite;
    startupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION processInfo;
    ZeroMemory(&processInfo, sizeof(processInfo));

    std::vector<std::string> arguments(parts.begin() + 1, parts.end());
    std::wstring commandLine = BuildCommandLine(parts[0], arguments);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    const BOOL created = CreateProcessW(
        nullptr,
        mutableCommand.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo);

    CloseHandle(stdoutWrite);
    CloseHandle(stdinRead);

    if (!created) {
      if (error) {
        *error = "CreateProcessW failed for stdio transport";
      }
      Close();
      return false;
    }

    processHandle_ = processInfo.hProcess;
    processThread_ = processInfo.hThread;
    nextRequestId_ = 1;

    std::string initializeResponse;
    if (!SendFramedRequest(
            BuildJsonRpcRequest(nextRequestId_++, "initialize",
                                BuildInitializeParams()),
            &initializeResponse, error)) {
      Close();
      return false;
    }
    if (!ParseInitializeResponse(initializeResponse, &capabilities_,
                                 &instructions_, &serverVersion_, error)) {
      Close();
      return false;
    }

    std::string ignored;
    if (!SendFramedRequest(BuildJsonRpcNotification("notifications/initialized",
                                                    "{}"),
                           &ignored, nullptr, false)) {
      Close();
      if (error) {
        *error = "failed to send initialized notification";
      }
      return false;
    }
    return true;
  }

  void PopulateConnectionState(McpServerConnection* connection) const override {
    if (connection == nullptr) {
      return;
    }
    connection->capabilities = capabilities_;
    connection->instructions = instructions_;
    connection->serverVersion = serverVersion_;
  }

  McpTransportResponse Send(const McpTransportRequest& request) override {
    McpTransportResponse response;
    if (stdinWrite_ == nullptr || stdoutRead_ == nullptr) {
      response.error = "stdio transport is not connected";
      return response;
    }

    std::string body;
    if (!SendFramedRequest(BuildJsonRpcRequest(nextRequestId_++, request.method,
                                               request.paramsJson),
                           &body, &response.error)) {
      return response;
    }
    response.ok = true;
    response.bodyJson = body;
    return response;
  }

  void Close() override {
    if (stdinWrite_ != nullptr) {
      CloseHandle(stdinWrite_);
      stdinWrite_ = nullptr;
    }
    if (stdoutRead_ != nullptr) {
      CloseHandle(stdoutRead_);
      stdoutRead_ = nullptr;
    }
    if (processHandle_ != nullptr) {
      DWORD exitCode = 0;
      if (GetExitCodeProcess(processHandle_, &exitCode) && exitCode == STILL_ACTIVE) {
        TerminateProcess(processHandle_, 0);
      }
      CloseHandle(processHandle_);
      processHandle_ = nullptr;
    }
    if (processThread_ != nullptr) {
      CloseHandle(processThread_);
      processThread_ = nullptr;
    }
  }

 private:
  bool SendFramedRequest(
      const std::string& payload,
      std::string* responseBody,
      std::string* error,
      bool expectResponse = true) {
    const std::string framed = "Content-Length: " +
                               std::to_string(payload.size()) +
                               "\r\n\r\n" + payload;
    if (!WriteAll(stdinWrite_, framed)) {
      if (error != nullptr) {
        *error = "failed to write MCP stdio request";
      }
      return false;
    }
    if (!expectResponse) {
      if (responseBody != nullptr) {
        responseBody->clear();
      }
      return true;
    }
    return ReadFramedMessage(stdoutRead_, processHandle_, kDefaultTransportTimeoutMs,
                             responseBody, error);
  }

  McpServerConfig config_;
  McpServerCapabilities capabilities_;
  std::string instructions_;
  std::string serverVersion_;
  HANDLE stdinWrite_ = nullptr;
  HANDLE stdoutRead_ = nullptr;
  HANDLE processHandle_ = nullptr;
  HANDLE processThread_ = nullptr;
  int nextRequestId_ = 1;
};

class HttpMcpTransport : public McpTransport {
 public:
  ~HttpMcpTransport() override { Close(); }

  bool Connect(const McpServerConfig& config, std::string* error) override {
    config_ = config;
    nextRequestId_ = 1;
    clientSessionId_ = GenerateSessionToken();
    sessionId_.clear();
    sessionExpiresAtUnixMs_ = 0;
    lastActivityUnixMs_ = 0;
    reconnectCount_ = 0;
    return InitializeConnection(error);
  }

  void PopulateConnectionState(McpServerConnection* connection) const override {
    if (connection == nullptr) {
      return;
    }
    connection->capabilities = capabilities_;
    connection->instructions = instructions_;
    connection->serverVersion = serverVersion_;
    connection->transportSessionId = sessionId_;
    connection->clientSessionId = clientSessionId_;
    connection->sessionExpiresAtUnixMs = sessionExpiresAtUnixMs_;
    connection->lastActivityUnixMs = lastActivityUnixMs_;
    connection->reconnectCount = reconnectCount_;
    connection->streamableHttp = true;
  }

  McpTransportResponse Send(const McpTransportRequest& request) override {
    McpTransportResponse response;
    if (session_ == nullptr || connect_ == nullptr) {
      if (!InitializeConnection(&response.error)) {
        return response;
      }
    }

    std::string body;
    int requestId = nextRequestId_++;
    if (!PostJson(BuildJsonRpcRequest(requestId, request.method,
                                      request.paramsJson),
                  requestId, &body, &response.error)) {
      if (ShouldRetryTransportError(response.error) && Reconnect(&response.error) &&
          PostJson(BuildJsonRpcRequest(requestId, request.method,
                                       request.paramsJson),
                   requestId,
                   &body, &response.error)) {
        response.ok = true;
        response.bodyJson = body;
      }
      return response;
    }
    response.ok = true;
    response.bodyJson = body;
    return response;
  }

  void Close() override {
    if (connect_ != nullptr) {
      WinHttpCloseHandle(connect_);
      connect_ = nullptr;
    }
    if (session_ != nullptr) {
      WinHttpCloseHandle(session_);
      session_ = nullptr;
    }
    sessionId_.clear();
    sessionExpiresAtUnixMs_ = 0;
    lastActivityUnixMs_ = 0;
    lastEventId_.clear();
    retryAfterMs_ = 250;
  }

 private:
  static constexpr long long kDefaultSessionTtlMs = 10LL * 60LL * 1000LL;
  static constexpr int kMaxSseResumeAttempts = 5;

  enum class SseReadOutcome {
    ResponseReceived,
    NeedResume,
    Failed
  };

  bool InitializeConnection(std::string* error) {
    Close();
    if (!OpenHandles(error)) {
      return false;
    }

    std::string initializeResponse;
    if (!PostJson(BuildJsonRpcRequest(nextRequestId_++, "initialize",
                                      BuildInitializeParams()),
                  1, &initializeResponse, error, false)) {
      Close();
      return false;
    }
    if (!ParseInitializeResponse(initializeResponse, &capabilities_,
                                 &instructions_, &serverVersion_, error)) {
      Close();
      return false;
    }

    std::string ignored;
    if (!PostJson(BuildJsonRpcNotification("notifications/initialized", "{}"),
                  0, &ignored, error, true)) {
      Close();
      return false;
    }
    return true;
  }

  bool OpenHandles(std::string* error) {
    URL_COMPONENTS components;
    ZeroMemory(&components, sizeof(components));
    components.dwStructSize = sizeof(components);
    wchar_t hostName[256] = {0};
    wchar_t urlPath[2048] = {0};
    wchar_t extraInfo[1024] = {0};
    components.lpszHostName = hostName;
    components.dwHostNameLength = sizeof(hostName) / sizeof(wchar_t);
    components.lpszUrlPath = urlPath;
    components.dwUrlPathLength = sizeof(urlPath) / sizeof(wchar_t);
    components.lpszExtraInfo = extraInfo;
    components.dwExtraInfoLength = sizeof(extraInfo) / sizeof(wchar_t);

    endpoint_ = ToWide(config_.endpoint);
    if (!WinHttpCrackUrl(endpoint_.c_str(), 0, 0, &components)) {
      if (error) {
        *error = "WinHttpCrackUrl failed";
      }
      return false;
    }

    secure_ = components.nScheme == INTERNET_SCHEME_HTTPS;
    port_ = components.nPort;
    host_ = std::wstring(components.lpszHostName, components.dwHostNameLength);
    path_ = std::wstring(components.lpszUrlPath, components.dwUrlPathLength) +
            std::wstring(components.lpszExtraInfo, components.dwExtraInfoLength);

    session_ = WinHttpOpen(L"cpp-agent/0.1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_ == nullptr) {
      if (error) {
        *error = "WinHttpOpen failed";
      }
      return false;
    }

    WinHttpSetTimeouts(session_, 5000, 5000, 15000, 30000);
    connect_ = WinHttpConnect(session_, host_.c_str(), port_, 0);
    if (connect_ == nullptr) {
      if (error) {
        *error = "WinHttpConnect failed";
      }
      Close();
      return false;
    }
    return true;
  }

  bool Reconnect(std::string* error) {
    ++reconnectCount_;
    sessionId_.clear();
    sessionExpiresAtUnixMs_ = 0;
    lastActivityUnixMs_ = 0;
    lastEventId_.clear();
    retryAfterMs_ = 250;
    return InitializeConnection(error);
  }

  bool ResetConnectHandle(std::string* error) {
    if (session_ == nullptr) {
      if (error) {
        *error = "WinHTTP session is not initialized";
      }
      return false;
    }
    if (connect_ != nullptr) {
      WinHttpCloseHandle(connect_);
      connect_ = nullptr;
    }
    connect_ = WinHttpConnect(session_, host_.c_str(), port_, 0);
    if (connect_ == nullptr) {
      if (error) {
        *error = "WinHttpConnect failed";
      }
      return false;
    }
    return true;
  }

  void MaybeExpireSession() {
    if (sessionId_.empty() || sessionExpiresAtUnixMs_ <= 0) {
      return;
    }
    if (NowUnixMs() >= sessionExpiresAtUnixMs_) {
      sessionId_.clear();
      sessionExpiresAtUnixMs_ = 0;
    }
  }

  bool ShouldRetryTransportError(const std::string& error) const {
    return error.find("session expired") != std::string::npos ||
           error.find("WinHttpSendRequest") != std::string::npos ||
           error.find("WinHttpReceiveResponse") != std::string::npos ||
           error.find("WinHttpReadData") != std::string::npos ||
           error.find("SSE stream closed") != std::string::npos;
  }

  bool PostJson(
      const std::string& payload,
      int requestId,
      std::string* responseBody,
      std::string* error,
      bool notificationOnly = false) {
    MaybeExpireSession();
    lastEventId_.clear();
    retryAfterMs_ = 250;

    if (session_ == nullptr || connect_ == nullptr) {
      if (error) {
        *error = "HTTP transport is not connected";
      }
      return false;
    }

    const std::wstring headers = BuildHeaders(true, true, std::string());
    HINTERNET request = OpenRequest(L"POST", headers, error);
    if (request == nullptr) {
      return false;
    }
    BOOL sent = WinHttpSendRequest(
        request,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        WINHTTP_NO_REQUEST_DATA,
        0,
        static_cast<DWORD>(payload.size()),
        0);
    if (!sent) {
      if (error) {
        *error = "WinHttpSendRequest failed";
      }
      WinHttpCloseHandle(request);
      return false;
    }

    if (!WritePayloadInChunks(request, payload, error)) {
      WinHttpCloseHandle(request);
      return false;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
      if (error) {
        *error = "WinHttpReceiveResponse failed";
      }
      WinHttpCloseHandle(request);
      return false;
    }

    DWORD statusCode = 0;
    QueryStatusCode(request, &statusCode);
    UpdateSessionHeaders(request);

    std::wstring contentTypeWide;
    QueryHeaderString(request, WINHTTP_QUERY_CONTENT_TYPE,
                      WINHTTP_HEADER_NAME_BY_INDEX, &contentTypeWide);
    const std::string contentType = ToUtf8(contentTypeWide);

    if (statusCode == 404) {
      std::string body404;
      ReadResponseBody(request, &body404, error);
      WinHttpCloseHandle(request);
      sessionId_.clear();
      sessionExpiresAtUnixMs_ = 0;
      if (error) {
        *error = "session expired";
      }
      return false;
    }
    if (statusCode == 202 && notificationOnly) {
      WinHttpCloseHandle(request);
      if (responseBody != nullptr) {
        responseBody->clear();
      }
      return true;
    }
    if (statusCode >= 400 && statusCode != 404) {
      std::string ignoredBody;
      ReadResponseBody(request, &ignoredBody, error);
      WinHttpCloseHandle(request);
      if (error) {
        *error = "HTTP MCP request failed with status " +
                 std::to_string(static_cast<int>(statusCode));
      }
      return false;
    }

    const std::string mediaType = NormalizeContentType(contentType);
    std::string body;
    bool ok = false;
    if (mediaType == "text/event-stream") {
      ok = ReadEventStreamResponse(request, requestId, &body, error);
    } else {
      ok = ReadResponseBody(request, &body, error);
      WinHttpCloseHandle(request);
      request = nullptr;
      if (ok) {
        body = DecodeStreamableHttpPayload(contentType, body);
      }
    }
    if (!ok) {
      if (request != nullptr) {
        WinHttpCloseHandle(request);
      }
      return false;
    }

    lastActivityUnixMs_ = NowUnixMs();
    if (sessionExpiresAtUnixMs_ == 0) {
      sessionExpiresAtUnixMs_ = lastActivityUnixMs_ + kDefaultSessionTtlMs;
    }

    if (responseBody != nullptr) {
      *responseBody = body;
    }
    if (notificationOnly && responseBody != nullptr) {
      responseBody->clear();
    }
    return true;
  }

  HINTERNET OpenRequest(
      const wchar_t* verb,
      const std::wstring& headers,
      std::string* error) const {
    HINTERNET request = WinHttpOpenRequest(
        connect_, verb, path_.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, secure_ ? WINHTTP_FLAG_SECURE : 0);
    if (request == nullptr) {
      if (error) {
        *error = "WinHttpOpenRequest failed";
      }
      return nullptr;
    }
    if (!WinHttpAddRequestHeaders(request, headers.c_str(),
                                  static_cast<DWORD>(headers.size()),
                                  WINHTTP_ADDREQ_FLAG_ADD)) {
      if (error) {
        *error = "WinHttpAddRequestHeaders failed";
      }
      WinHttpCloseHandle(request);
      return nullptr;
    }
    return request;
  }

  HINTERNET OpenStandaloneRequest(
      const wchar_t* verb,
      const std::wstring& headers,
      HINTERNET* tempSession,
      HINTERNET* tempConnect,
      std::string* error) const {
    if (tempSession == nullptr || tempConnect == nullptr) {
      if (error) {
        *error = "temporary WinHTTP handles are required";
      }
      return nullptr;
    }
    *tempSession = WinHttpOpen(L"cpp-agent/0.1.0",
                               WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (*tempSession == nullptr) {
      if (error) {
        *error = "WinHttpOpen failed";
      }
      return nullptr;
    }
    WinHttpSetTimeouts(*tempSession, 5000, 5000, 15000, 30000);
    *tempConnect = WinHttpConnect(*tempSession, host_.c_str(), port_, 0);
    if (*tempConnect == nullptr) {
      if (error) {
        *error = "WinHttpConnect failed";
      }
      WinHttpCloseHandle(*tempSession);
      *tempSession = nullptr;
      return nullptr;
    }

    HINTERNET request = WinHttpOpenRequest(
        *tempConnect, verb, path_.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, secure_ ? WINHTTP_FLAG_SECURE : 0);
    if (request == nullptr) {
      if (error) {
        *error = "WinHttpOpenRequest failed";
      }
      WinHttpCloseHandle(*tempConnect);
      WinHttpCloseHandle(*tempSession);
      *tempConnect = nullptr;
      *tempSession = nullptr;
      return nullptr;
    }
    if (!WinHttpAddRequestHeaders(request, headers.c_str(),
                                  static_cast<DWORD>(headers.size()),
                                  WINHTTP_ADDREQ_FLAG_ADD)) {
      if (error) {
        *error = "WinHttpAddRequestHeaders failed";
      }
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(*tempConnect);
      WinHttpCloseHandle(*tempSession);
      *tempConnect = nullptr;
      *tempSession = nullptr;
      return nullptr;
    }
    return request;
  }

  std::wstring BuildHeaders(
      bool includeContentType,
      bool acceptJson,
      const std::string& lastEventId) const {
    std::wstring headers = L"Cache-Control: no-cache\r\n";
    if (includeContentType) {
      headers += L"Content-Type: application/json\r\n";
    }
    if (acceptJson) {
      headers +=
          L"Accept: application/json, text/event-stream, application/x-ndjson\r\n";
    } else {
      headers += L"Accept: text/event-stream\r\n";
    }
    headers += L"MCP-Protocol-Version: " + ToWide(kMcpProtocolVersion) + L"\r\n";
    if (!clientSessionId_.empty()) {
      headers += L"X-Mcp-Client-Session-Id: " + ToWide(clientSessionId_) +
                 L"\r\n";
    }
    if (!sessionId_.empty()) {
      headers += L"MCP-Session-Id: " + ToWide(sessionId_) + L"\r\n";
    }
    if (!lastEventId.empty()) {
      headers += L"Last-Event-ID: " + ToWide(lastEventId) + L"\r\n";
    }
    return headers;
  }

  bool WritePayloadInChunks(
      HINTERNET request,
      const std::string& payload,
      std::string* error) {
    std::string mutablePayload = payload;
    const std::size_t kChunkSize = 4096;
    std::size_t offset = 0;
    while (offset < mutablePayload.size()) {
      const DWORD chunkSize = static_cast<DWORD>(
          std::min<std::size_t>(kChunkSize, mutablePayload.size() - offset));
      DWORD written = 0;
      if (!WinHttpWriteData(request,
                            &mutablePayload[offset],
                            chunkSize, &written) ||
          written != chunkSize) {
        if (error) {
          *error = "WinHttpWriteData failed";
        }
        return false;
      }
      offset += written;
    }
    return true;
  }

  bool ReadResponseBody(
      HINTERNET request,
      std::string* responseBody,
      std::string* error) {
    if (responseBody == nullptr) {
      return false;
    }

    std::string body;
    DWORD available = 0;
    do {
      available = 0;
      if (!WinHttpQueryDataAvailable(request, &available)) {
        if (error) {
          *error = "WinHttpQueryDataAvailable failed";
        }
        return false;
      }
      if (available == 0) {
        break;
      }
      std::vector<char> buffer(available);
      DWORD downloaded = 0;
      if (!WinHttpReadData(request, &buffer[0], available, &downloaded)) {
        if (error) {
          *error = "WinHttpReadData failed";
        }
        return false;
      }
      body.append(buffer.begin(), buffer.begin() + downloaded);
    } while (available > 0);

    *responseBody = body;
    return true;
  }

  SseReadOutcome ConsumeSseRequest(
      HINTERNET request,
      int requestId,
      std::string* responseBody,
      std::string* error) {
    std::string carry;
    std::string currentId;
    std::string currentData;
    int currentRetryMs = 0;
    while (true) {
      DWORD available = 0;
      if (!WinHttpQueryDataAvailable(request, &available)) {
        if (error) {
          *error = "WinHttpQueryDataAvailable failed";
        }
        return SseReadOutcome::Failed;
      }
      if (available == 0) {
        break;
      }

      std::vector<char> buffer(available);
      DWORD downloaded = 0;
      if (!WinHttpReadData(request, &buffer[0], available, &downloaded)) {
        if (error) {
          *error = "WinHttpReadData failed";
        }
        return SseReadOutcome::Failed;
      }

      std::vector<SseEventFrame> events;
      ParseSseChunk(std::string(buffer.begin(), buffer.begin() + downloaded),
                    &carry, &currentId, &currentData, &currentRetryMs, &events);
      for (const auto& event : events) {
        if (!event.eventId.empty()) {
          lastEventId_ = event.eventId;
        }
        if (event.retryMs > 0) {
          retryAfterMs_ = event.retryMs;
        }
        const std::string trimmed = Trim(event.data);
        if (trimmed.empty() || trimmed == "[DONE]") {
          continue;
        }
        if (IsJsonRpcResponseForId(trimmed, requestId)) {
          if (responseBody != nullptr) {
            *responseBody = trimmed;
          }
          return SseReadOutcome::ResponseReceived;
        }
      }
    }

    std::vector<SseEventFrame> tailEvents;
    ParseSseChunk("\n\n", &carry, &currentId, &currentData, &currentRetryMs,
                  &tailEvents);
    for (const auto& event : tailEvents) {
      if (!event.eventId.empty()) {
        lastEventId_ = event.eventId;
      }
      if (event.retryMs > 0) {
        retryAfterMs_ = event.retryMs;
      }
      const std::string trimmed = Trim(event.data);
      if (!trimmed.empty() && IsJsonRpcResponseForId(trimmed, requestId)) {
        if (responseBody != nullptr) {
          *responseBody = trimmed;
        }
        return SseReadOutcome::ResponseReceived;
      }
    }

    if (!lastEventId_.empty()) {
      return SseReadOutcome::NeedResume;
    }
    if (error) {
      *error = "SSE stream closed before JSON-RPC response";
    }
    return SseReadOutcome::Failed;
  }

  bool ReadEventStreamResponse(
      HINTERNET request,
      int requestId,
      std::string* responseBody,
      std::string* error) {
    HINTERNET activeRequest = request;
    HINTERNET tempResumeSession = nullptr;
    HINTERNET tempResumeConnect = nullptr;
    int resumeAttempts = 0;
    while (activeRequest != nullptr) {
      const SseReadOutcome outcome =
          ConsumeSseRequest(activeRequest, requestId, responseBody, error);
      WinHttpCloseHandle(activeRequest);
      activeRequest = nullptr;
      if (tempResumeConnect != nullptr) {
        WinHttpCloseHandle(tempResumeConnect);
        tempResumeConnect = nullptr;
      }
      if (tempResumeSession != nullptr) {
        WinHttpCloseHandle(tempResumeSession);
        tempResumeSession = nullptr;
      }
      if (outcome == SseReadOutcome::ResponseReceived) {
        return true;
      }
      if (outcome != SseReadOutcome::NeedResume ||
          resumeAttempts >= kMaxSseResumeAttempts) {
        if (outcome == SseReadOutcome::NeedResume && error) {
          *error = "SSE stream closed before JSON-RPC response";
        }
        return false;
      }

      ++resumeAttempts;
      ++reconnectCount_;
      Sleep(static_cast<DWORD>(std::max(50, retryAfterMs_)));
      const std::wstring headers = BuildHeaders(false, false, lastEventId_);
      activeRequest = OpenStandaloneRequest(
          L"GET", headers, &tempResumeSession, &tempResumeConnect, error);
      if (activeRequest == nullptr) {
        return false;
      }
      if (!WinHttpSendRequest(activeRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        if (error) {
          *error = "WinHttpSendRequest failed during SSE resume";
        }
        WinHttpCloseHandle(activeRequest);
        return false;
      }
      if (!WinHttpReceiveResponse(activeRequest, nullptr)) {
        if (error) {
          *error = "WinHttpReceiveResponse failed during SSE resume";
        }
        WinHttpCloseHandle(activeRequest);
        return false;
      }
      UpdateSessionHeaders(activeRequest);
    }
    return false;
  }

  void UpdateSessionHeaders(HINTERNET request) {
    std::string candidate;
    std::wstring sessionIdWide;
    if (QueryHeaderString(request, WINHTTP_QUERY_CUSTOM, L"MCP-Session-Id",
                          &sessionIdWide)) {
      candidate = Trim(ToUtf8(sessionIdWide));
    } else if (!TryGetRawHeaderValue(request, "MCP-Session-Id", &candidate)) {
      TryGetRawHeaderValue(request, "Mcp-Session-Id", &candidate);
    }
    if (IsLikelySessionId(candidate)) {
      sessionId_ = candidate;
      sessionExpiresAtUnixMs_ = NowUnixMs() + kDefaultSessionTtlMs;
    }
  }

  McpServerConfig config_;
  McpServerCapabilities capabilities_;
  std::string instructions_;
  std::string serverVersion_;
  std::string clientSessionId_;
  std::string sessionId_;
  HINTERNET session_ = nullptr;
  HINTERNET connect_ = nullptr;
  std::wstring endpoint_;
  std::wstring host_;
  std::wstring path_;
  INTERNET_PORT port_ = 0;
  bool secure_ = false;
  long long sessionExpiresAtUnixMs_ = 0;
  long long lastActivityUnixMs_ = 0;
  std::string lastEventId_;
  int retryAfterMs_ = 250;
  int reconnectCount_ = 0;
  int nextRequestId_ = 1;
};

std::unique_ptr<McpTransport> CreateDefaultTransport(
    const McpServerConfig& config) {
  const std::string transportType = Trim(config.transportType);
  const std::string endpoint = Trim(config.endpoint);
  if (transportType == "stdio") {
    return std::unique_ptr<McpTransport>(new StdIoMcpTransport());
  }
  if (transportType == "http" || transportType == "streamable_http" ||
      endpoint.find("http://") == 0 || endpoint.find("https://") == 0) {
    return std::unique_ptr<McpTransport>(new HttpMcpTransport());
  }
  return std::unique_ptr<McpTransport>();
}

}  // namespace

struct McpClientManager::ManagedConnection {
  McpServerConnection state;
  std::unique_ptr<McpTransport> transport;
};

McpClientManager::McpClientManager()
    : transportFactory_(CreateDefaultTransport) {}

McpClientManager::~McpClientManager() {
  for (const auto& connection : connections_) {
    if (connection && connection->transport) {
      connection->transport->Close();
    }
  }
}

void McpClientManager::SetTransportFactory(McpTransportFactory factory) {
  transportFactory_ = factory;
}

bool McpClientManager::RegisterServer(const McpServerConfig& config) {
  for (const auto& existing : connections_) {
    if (existing->state.name == config.name) {
      return false;
    }
  }

  std::shared_ptr<ManagedConnection> connection(new ManagedConnection());
  connection->state.type = McpServerConnection::Type::Pending;
  connection->state.name = config.name;
  connection->state.config = config;
  connections_.push_back(connection);
  return true;
}

std::vector<McpServerConnection> McpClientManager::connections() const {
  std::vector<McpServerConnection> states;
  for (const auto& connection : connections_) {
    states.push_back(connection->state);
  }
  return states;
}

bool McpClientManager::ConnectServer(const std::string& serverName) {
  ManagedConnection* connection = FindConnection(serverName);
  if (connection == nullptr) {
    return false;
  }
  if (!transportFactory_) {
    MarkFailed(serverName, "no MCP transport factory configured");
    return false;
  }

  std::unique_ptr<McpTransport> transport =
      transportFactory_(connection->state.config);
  if (!transport) {
    MarkFailed(serverName, "unsupported MCP transport type");
    return false;
  }

  std::string error;
  if (!transport->Connect(connection->state.config, &error)) {
    MarkFailed(serverName, error.empty() ? "transport connect failed" : error);
    return false;
  }

  transport->PopulateConnectionState(&connection->state);
  connection->transport = std::move(transport);
  return MarkConnected(serverName);
}

bool McpClientManager::MarkConnected(const std::string& serverName) {
  ManagedConnection* connection = FindConnection(serverName);
  if (connection) {
    connection->state.type = McpServerConnection::Type::Connected;
    connection->state.error.clear();
    return true;
  }
  return false;
}

bool McpClientManager::MarkFailed(
    const std::string& serverName,
    const std::string& error) {
  ManagedConnection* connection = FindConnection(serverName);
  if (connection) {
    if (connection->transport) {
      connection->transport->Close();
      connection->transport.reset();
    }
    connection->state.type = McpServerConnection::Type::Failed;
    connection->state.error = error;
    return true;
  }
  return false;
}

bool McpClientManager::MarkNeedsAuth(const std::string& serverName) {
  ManagedConnection* connection = FindConnection(serverName);
  if (connection) {
    connection->state.type = McpServerConnection::Type::NeedsAuth;
    return true;
  }
  return false;
}

bool McpClientManager::HandleOAuth401(const std::string& serverName) {
  ManagedConnection* connection = FindConnection(serverName);
  if (!connection) return false;

  if (connection->state.type != McpServerConnection::Type::NeedsAuth)
    return false;

  if (!oAuthTokenProvider_) return false;

  std::string freshToken;
  try {
    freshToken = oAuthTokenProvider_();
  } catch (...) {
    connection->state.error = "OAuth token refresh threw exception";
    return false;
  }

  if (freshToken.empty()) {
    connection->state.error = "OAuth token provider returned empty token";
    return false;
  }

  if (connection->transport) {
    connection->transport->Close();
    connection->transport.reset();
  }

  if (!transportFactory_) {
    connection->state.error = "No transport factory set for OAuth reconnect";
    return false;
  }

  connection->transport = transportFactory_(connection->state.config);
  if (!connection->transport) {
    connection->state.error = "Transport factory returned null for OAuth reconnect";
    return false;
  }

  std::string connectError;
  if (!connection->transport->Connect(connection->state.config, &connectError)) {
    connection->state.error = "OAuth reconnect failed: " + connectError;
    return false;
  }

  connection->state.type = McpServerConnection::Type::Connected;
  connection->state.error.clear();
  connection->state.reconnectCount++;

  return true;
}

void McpClientManager::SetOAuthTokenProvider(
    std::function<std::string()> provider) {
  oAuthTokenProvider_ = std::move(provider);
}

int McpClientManager::GetConnectionBatchLimit() {
  char buffer[64] = {0};
  DWORD len = GetEnvironmentVariableA(
      "MCP_SERVER_CONNECTION_BATCH_SIZE", buffer, sizeof(buffer));
  if (len == 0 || len >= sizeof(buffer)) return 3;
  int val = std::atoi(buffer);
  if (val <= 0) return 3;
  if (val > 20) return 20;
  return val;
}

bool McpClientManager::RefreshToolsFromTransport(const std::string& serverName) {
  ManagedConnection* connection = FindConnection(serverName);
  if (connection == nullptr || !connection->transport) {
    return false;
  }

  if (!connection->state.capabilities.tools) {
    connection->state.error = "MCP server does not advertise tools capability";
    return false;
  }

  const McpTransportResponse response =
      connection->transport->Send({"tools/list", "{}"});
  connection->transport->PopulateConnectionState(&connection->state);
  if (!response.ok) {
    MarkFailed(serverName,
               response.error.empty() ? "tools/list failed" : response.error);
    return false;
  }

  std::vector<McpToolSchema> tools;
  std::string error;
  if (!ParseToolsListResponse(serverName, response.bodyJson, &tools, &error)) {
    connection->state.error = error;
    return false;
  }
  return SetFetchedTools(serverName, tools);
}

bool McpClientManager::SetFetchedTools(
    const std::string& serverName,
    const std::vector<McpToolSchema>& tools) {
  ManagedConnection* connection = FindConnection(serverName);
  if (connection) {
    connection->state.tools.clear();
    for (auto tool : tools) {
      tool.serverName = serverName;
      if (tool.fullyQualifiedName.empty()) {
        tool.fullyQualifiedName = BuildMcpToolName(serverName, tool.toolName);
      }
      if (tool.inputSchemaJson.empty()) {
        tool.inputSchemaJson = "{}";
      }
      if (static_cast<int>(tool.description.size()) > kMaxMcpDescriptionLength) {
        tool.description =
            tool.description.substr(0, kMaxMcpDescriptionLength) +
            "\xe2\x80\xa6 [truncated]";
      }
      connection->state.tools.push_back(tool);
    }
    connection->state.error.clear();
    return true;
  }
  return false;
}

std::vector<McpToolSchema> McpClientManager::FetchToolsForClient(
    const std::string& serverName) const {
  const ManagedConnection* connection = FindConnection(serverName);
  if (connection &&
      connection->state.type == McpServerConnection::Type::Connected) {
    return connection->state.tools;
  }
  return std::vector<McpToolSchema>();
}

bool McpClientManager::ParseToolsListResponse(
    const std::string& serverName,
    const std::string& bodyJson,
    std::vector<McpToolSchema>* tools,
    std::string* error) {
  if (tools == nullptr || error == nullptr) {
    return false;
  }
  tools->clear();
  error->clear();

  const std::string payload = ExtractToolsPayload(bodyJson);
  std::string toolsArray;
  if (!ExtractJsonArrayField(payload, "tools", &toolsArray)) {
    *error = "tools/list response missing tools array";
    return false;
  }

  const std::vector<std::string> toolObjects = SplitTopLevelObjects(toolsArray);
  for (const auto& toolObject : toolObjects) {
    McpToolSchema tool;
    tool.serverName = serverName;
    if (!ExtractJsonStringField(toolObject, "name", &tool.toolName)) {
      *error = "tool object missing name";
      return false;
    }
    ExtractJsonStringField(toolObject, "description", &tool.description);
    if (!ExtractJsonObjectField(toolObject, "inputSchema", &tool.inputSchemaJson)) {
      ExtractJsonObjectField(toolObject, "input_schema", &tool.inputSchemaJson);
    }
    if (tool.inputSchemaJson.empty()) {
      tool.inputSchemaJson = "{}";
    }

    std::string annotations;
    if (ExtractJsonObjectField(toolObject, "annotations", &annotations)) {
      tool.readOnlyHint =
          ExtractJsonBoolField(annotations, "readOnlyHint", false);
      tool.destructiveHint =
          ExtractJsonBoolField(annotations, "destructiveHint", false);
      tool.openWorldHint =
          ExtractJsonBoolField(annotations, "openWorldHint", false);
    }

    tool.fullyQualifiedName = BuildMcpToolName(serverName, tool.toolName);
    tools->push_back(tool);
  }

  return true;
}

std::string McpClientManager::BuildMcpToolName(
    const std::string& serverName,
    const std::string& toolName) {
  return "mcp__" + NormalizeNameForMcp(serverName) + "__" +
         NormalizeNameForMcp(toolName);
}

McpClientManager::ManagedConnection* McpClientManager::FindConnection(
    const std::string& serverName) {
  for (auto& connection : connections_) {
    if (connection->state.name == serverName) {
      return connection.get();
    }
  }
  return nullptr;
}

const McpClientManager::ManagedConnection* McpClientManager::FindConnection(
    const std::string& serverName) const {
  for (const auto& connection : connections_) {
    if (connection->state.name == serverName) {
      return connection.get();
    }
  }
  return nullptr;
}

std::string McpClientManager::NormalizeNameForMcp(
    const std::string& name) {
  std::string normalized;
  normalized.reserve(name.size());

  for (char ch : name) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (std::isalnum(c)) {
      normalized.push_back(
          static_cast<char>(std::tolower(c)));
    } else if (ch == '-' || ch == '_' || ch == ' ') {
      normalized.push_back('_');
    }
  }

  normalized.erase(
      std::unique(normalized.begin(), normalized.end(),
                  [](char lhs, char rhs) {
                    return lhs == '_' && rhs == '_';
                  }),
      normalized.end());

  if (!normalized.empty() && normalized.front() == '_') {
    normalized.erase(normalized.begin());
  }
  if (!normalized.empty() && normalized.back() == '_') {
    normalized.pop_back();
  }

  return normalized;
}

bool McpClientManager::RefreshPromptsFromTransport(
    const std::string& serverName) {
  ManagedConnection* connection = FindConnection(serverName);
  if (!connection || !connection->transport) return false;
  if (!connection->state.capabilities.prompts) return false;

  McpTransportResponse response =
      connection->transport->Send({"prompts/list", "{}"});
  if (!response.ok) {
    connection->state.error = response.error;
    return false;
  }

  std::string result;
  ExtractJsonObjectField(response.bodyJson, "result", &result);
  if (result.empty()) result = response.bodyJson;

  std::string promptsArray;
  if (!ExtractJsonArrayField(result, "prompts", &promptsArray)) return false;

  auto objects = SplitTopLevelObjects(promptsArray);
  for (const auto& obj : objects) {
    McpPromptSchema prompt;
    prompt.serverName = serverName;
    ExtractJsonStringField(obj, "name", &prompt.name);
    ExtractJsonStringField(obj, "description", &prompt.description);
    if (!prompt.name.empty())
      connection->state.prompts.push_back(prompt);
  }
  return true;
}

std::vector<McpPromptSchema> McpClientManager::FetchPromptsForClient(
    const std::string& serverName) const {
  const auto* conn = FindConnection(serverName);
  if (conn && conn->state.type == McpServerConnection::Type::Connected)
    return conn->state.prompts;
  return {};
}

bool McpClientManager::RefreshResourcesFromTransport(
    const std::string& serverName) {
  ManagedConnection* connection = FindConnection(serverName);
  if (!connection || !connection->transport) return false;
  if (!connection->state.capabilities.resources) return false;

  McpTransportResponse response =
      connection->transport->Send({"resources/list", "{}"});
  if (!response.ok) {
    connection->state.error = response.error;
    return false;
  }

  std::string result;
  ExtractJsonObjectField(response.bodyJson, "result", &result);
  if (result.empty()) result = response.bodyJson;

  std::string resourcesArray;
  if (!ExtractJsonArrayField(result, "resources", &resourcesArray)) return false;

  auto objects = SplitTopLevelObjects(resourcesArray);
  for (const auto& obj : objects) {
    McpResourceSchema resource;
    resource.serverName = serverName;
    ExtractJsonStringField(obj, "uri", &resource.uri);
    ExtractJsonStringField(obj, "name", &resource.name);
    ExtractJsonStringField(obj, "description", &resource.description);
    ExtractJsonStringField(obj, "mimeType", &resource.mimeType);
    if (!resource.uri.empty())
      connection->state.resources.push_back(resource);
  }
  return true;
}

std::vector<McpResourceSchema> McpClientManager::FetchResourcesForClient(
    const std::string& serverName) const {
  const auto* conn = FindConnection(serverName);
  if (conn && conn->state.type == McpServerConnection::Type::Connected)
    return conn->state.resources;
  return {};
}

}  // namespace mcp
}  // namespace agent
