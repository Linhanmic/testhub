/*
 * TestHub - Runner Bridge Implementation
 */

#include "runner_bridge.h"
#include "../util/string_util.h"

#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>

namespace testhub {

RunnerProcess::RunnerProcess() = default;
RunnerProcess::~RunnerProcess() { stop(); }

bool RunnerProcess::start(const std::string& command, const std::string& workingDir) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!createPipes()) return false;

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = stdoutWrite_;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::string cmdLine = command;
    if (CreateProcessA(NULL, (LPSTR)cmdLine.c_str(), NULL, NULL, TRUE, 0, NULL,
                       workingDir.empty() ? NULL : workingDir.c_str(), &si, &pi)) {
        processHandle_ = pi.hProcess;
        pid_ = pi.dwProcessId;
        running_ = true;
        CloseHandle(pi.hThread);
        return true;
    }
    closePipes();
    return false;
#else
    return false;
#endif
}

void RunnerProcess::stop() {
#ifdef _WIN32
    if (processHandle_) {
        TerminateProcess(processHandle_, 0);
        WaitForSingleObject(processHandle_, 5000);
        CloseHandle(processHandle_);
        processHandle_ = NULL;
    }
    closePipes();
#endif
    running_ = false;
    pid_ = 0;
}

bool RunnerProcess::isRunning() const {
    if (!running_ || pid_ == 0) return false;
#ifdef _WIN32
    if (processHandle_) {
        DWORD exitCode;
        if (GetExitCodeProcess(processHandle_, &exitCode)) {
            return exitCode == STILL_ACTIVE;
        }
    }
#endif
    return false;
}

bool RunnerProcess::sendMessage(const std::string& message) {
#ifdef _WIN32
    if (!stdinWrite_) return false;
    DWORD written;
    std::string data = message + "\n";
    return WriteFile(stdinWrite_, data.c_str(), data.length(), &written, NULL);
#else
    return false;
#endif
}

std::string RunnerProcess::receiveMessage(int timeout) {
#ifdef _WIN32
    if (!stdoutRead_) return "";
    DWORD available;
    if (!PeekNamedPipe(stdoutRead_, NULL, 0, NULL, &available, NULL) || available == 0) {
        auto start = std::chrono::steady_clock::now();
        while (true) {
            if (!PeekNamedPipe(stdoutRead_, NULL, 0, NULL, &available, NULL) || available > 0) break;
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() >= timeout) return "";
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    char buffer[4096];
    DWORD bytesRead;
    if (ReadFile(stdoutRead_, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        return std::string(buffer, bytesRead);
    }
#endif
    return "";
}

bool RunnerProcess::createPipes() {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    if (!CreatePipe(&stdinRead_, &stdinWrite_, &sa, 0)) return false;
    SetHandleInformation(stdinWrite_, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&stdoutRead_, &stdoutWrite_, &sa, 0)) { closePipes(); return false; }
    SetHandleInformation(stdoutRead_, HANDLE_FLAG_INHERIT, 0);
    return true;
#else
    return false;
#endif
}

void RunnerProcess::closePipes() {
#ifdef _WIN32
    if (stdinRead_) { CloseHandle(stdinRead_); stdinRead_ = NULL; }
    if (stdinWrite_) { CloseHandle(stdinWrite_); stdinWrite_ = NULL; }
    if (stdoutRead_) { CloseHandle(stdoutRead_); stdoutRead_ = NULL; }
    if (stdoutWrite_) { CloseHandle(stdoutWrite_); stdoutWrite_ = NULL; }
#endif
}

RunnerBridge::RunnerBridge() = default;
RunnerBridge::~RunnerBridge() { stopRunner(); }

bool RunnerBridge::startRunner(const std::string& language, const std::string& projectPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == RunnerState::CONNECTED || state_ == RunnerState::CONNECTING) return true;
    language_ = language;
    projectPath_ = projectPath;
    state_ = RunnerState::CONNECTING;
    publishRunnerEvent(EventType::RUNNER_CONNECTED, "Connecting to " + language + " runner...");
    std::string command = getRunnerCommand(language);
    process_ = std::make_unique<RunnerProcess>();
    if (!process_->start(command, projectPath)) {
        state_ = RunnerState::RUNNER_ERROR;
        publishRunnerEvent(EventType::RUNNER_ERROR, "Failed to start runner process");
        return false;
    }
    stopReceiving_ = false;
    receiveThread_ = std::thread(&RunnerBridge::receiveLoop, this);
    stopHeartbeat_ = false;
    heartbeatThread_ = std::thread(&RunnerBridge::heartbeatLoop, this);
    state_ = RunnerState::CONNECTED;
    publishRunnerEvent(EventType::RUNNER_CONNECTED, language + " runner connected");
    return true;
}

void RunnerBridge::stopRunner() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == RunnerState::DISCONNECTED) return;
        stopReceiving_ = true;
        if (receiveThread_.joinable()) receiveThread_.join();
        stopHeartbeat_ = true;
        if (heartbeatThread_.joinable()) heartbeatThread_.join();
        if (process_) { process_->stop(); process_.reset(); }
        state_ = RunnerState::DISCONNECTED;
    }
    publishRunnerEvent(EventType::RUNNER_DISCONNECTED, "Runner disconnected");
}

bool RunnerBridge::restartRunner() {
    stopRunner();
    return startRunner(language_, projectPath_);
}

bool RunnerBridge::isConnected() const {
    return state_ == RunnerState::CONNECTED || state_ == RunnerState::BUSY;
}

RunnerStatus RunnerBridge::getStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RunnerStatus status;
    status.state = state_;
    status.language = language_;
    status.pid = process_ ? process_->getPid() : 0;
    status.lastHeartbeat = std::chrono::system_clock::now();
    return status;
}

StepResult RunnerBridge::executeStep(const std::string& stepText, const std::vector<std::string>& args) {
    StepResult result;
    result.stepText = stepText;
    if (!isConnected()) {
        result.state = TestState::TEST_ERROR;
        result.errorMessage = "Runner not connected";
        return result;
    }
    RunnerMessage message;
    message.type = RunnerMessageType::ExecuteStep;
    message.id = ++messageIdCounter_;
    std::ostringstream payload;
    payload << "{\"stepText\":\"" << stepText << "\",\"args\":[";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) payload << ",";
        payload << "\"" << args[i] << "\"";
    }
    payload << "]}";
    message.payload = payload.str();
    auto start = std::chrono::steady_clock::now();
    RunnerResponse response = sendAndWait(message, 60000);
    auto end = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration<double>(end - start).count();
    if (response.success) {
        result.state = TestState::PASSED;
    } else {
        result.state = TestState::FAILED;
        result.errorMessage = response.error;
    }
    return result;
}

std::vector<StepValue> RunnerBridge::getAllSteps() {
    std::vector<StepValue> steps;
    if (!isConnected()) return steps;
    RunnerMessage message;
    message.type = RunnerMessageType::AllSteps;
    message.id = ++messageIdCounter_;
    message.payload = "{}";
    RunnerResponse response = sendAndWait(message);
    return steps;
}

bool RunnerBridge::cacheFile(const CacheFileRequest& request) {
    if (!isConnected()) return false;
    RunnerMessage message;
    message.type = RunnerMessageType::CacheFile;
    message.id = ++messageIdCounter_;
    std::ostringstream payload;
    payload << "{\"filePath\":\"" << request.filePath << "\",\"content\":\"" << request.content << "\",\"status\":";
    switch (request.status) {
        case CacheFileRequest::Status::OPENED: payload << "\"OPENED\""; break;
        case CacheFileRequest::Status::CHANGED: payload << "\"CHANGED\""; break;
        case CacheFileRequest::Status::CLOSED: payload << "\"CLOSED\""; break;
        case CacheFileRequest::Status::CREATED: payload << "\"CREATED\""; break;
        case CacheFileRequest::Status::DELETED: payload << "\"DELETED\""; break;
    }
    payload << "}";
    message.payload = payload.str();
    RunnerResponse response = sendAndWait(message);
    return response.success;
}

std::vector<std::pair<int, std::string>> RunnerBridge::getStepPositions(const std::string& filePath) {
    std::vector<std::pair<int, std::string>> positions;
    if (!isConnected()) return positions;
    RunnerMessage message;
    message.type = RunnerMessageType::StepPositions;
    message.id = ++messageIdCounter_;
    message.payload = "{\"filePath\":\"" + filePath + "\"}";
    RunnerResponse response = sendAndWait(message);
    return positions;
}

std::vector<std::string> RunnerBridge::getImplementationFiles() {
    std::vector<std::string> files;
    if (!isConnected()) return files;
    RunnerMessage message;
    message.type = RunnerMessageType::ImplementationFileList;
    message.id = ++messageIdCounter_;
    message.payload = "{}";
    RunnerResponse response = sendAndWait(message);
    return files;
}

RunnerResponse RunnerBridge::sendAndWait(const RunnerMessage& message, int timeout) {
    int msgId = sendMessage(message);
    auto start = std::chrono::steady_clock::now();
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pendingResponses_.find(msgId);
            if (it != pendingResponses_.end()) {
                RunnerResponse response = it->second;
                pendingResponses_.erase(it);
                return response;
            }
        }
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() >= timeout) {
            RunnerResponse response;
            response.messageId = msgId;
            response.success = false;
            response.error = "Timeout waiting for runner response";
            return response;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int RunnerBridge::sendMessage(const RunnerMessage& message) {
    if (!process_) return -1;
    std::string serialized = serializeMessage(message);
    process_->sendMessage(serialized);
    return message.id;
}

void RunnerBridge::receiveLoop() {
    while (!stopReceiving_) {
        if (process_ && process_->isRunning()) {
            std::string message = process_->receiveMessage(100);
            if (!message.empty()) handleMessage(message);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void RunnerBridge::heartbeatLoop() {
    while (!stopHeartbeat_) {
        if (isConnected()) {
            if (process_ && !process_->isRunning()) {
                state_ = RunnerState::RUNNER_ERROR;
                publishRunnerEvent(EventType::RUNNER_DISCONNECTED, "Runner process died");
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

void RunnerBridge::handleMessage(const std::string& message) {
    RunnerResponse response = parseResponse(message);
    std::lock_guard<std::mutex> lock(mutex_);
    pendingResponses_[response.messageId] = response;
}

RunnerResponse RunnerBridge::parseResponse(const std::string& responseStr) {
    RunnerResponse response;
    response.success = true;
    response.payload = responseStr;
    return response;
}

std::string RunnerBridge::serializeMessage(const RunnerMessage& message) {
    std::ostringstream oss;
    oss << "{\"id\":" << message.id << ",\"type\":" << static_cast<int>(message.type) << ",\"payload\":" << message.payload << "}";
    return oss.str();
}

std::string RunnerBridge::getRunnerCommand(const std::string& language) const {
    if (language == "java") return "gauge-java-runner --stdio";
    if (language == "python") return "gauge-python-runner --stdio";
    if (language == "csharp") return "gauge-dotnet-runner --stdio";
    if (language == "js" || language == "javascript") return "gauge-js-runner --stdio";
    if (language == "ruby") return "gauge-ruby-runner --stdio";
    return "gauge-" + language + "-runner --stdio";
}

void RunnerBridge::publishRunnerEvent(const std::string& eventType, const std::string& detail) {
    publishEvent(eventType, "", {{"language", language_}, {"detail", detail}});
}

} // namespace testhub
