/*
 * TestHub - Windows 服务实现
 */

#include "win_service.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsvc.h>

#include <cstring>
#include <filesystem>
#include <vector>

namespace {

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
std::string g_serviceName = "TestHub";
std::function<bool()> g_start;
std::function<void()> g_wait;
std::function<void()> g_stop;
std::function<void()> g_requestStop;

void report(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHintMs = 0) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = waitHintMs;
    g_status.dwControlsAccepted = (state == SERVICE_START_PENDING) ? 0
                                : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
    if (state == SERVICE_RUNNING || state == SERVICE_STOPPED) g_status.dwCheckPoint = 0;
    else g_status.dwCheckPoint++;
    if (g_statusHandle) SetServiceStatus(g_statusHandle, &g_status);
}

DWORD WINAPI handlerEx(DWORD control, DWORD, LPVOID, LPVOID) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        report(SERVICE_STOP_PENDING, NO_ERROR, 8000);
        if (g_requestStop) g_requestStop();
        return NO_ERROR;
    }
    if (control == SERVICE_CONTROL_INTERROGATE) return NO_ERROR;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

VOID WINAPI serviceMain(DWORD, LPSTR*) {
    std::string exe = winExecutablePath();
    if (!exe.empty()) {
        auto dir = std::filesystem::path(exe).parent_path();
        SetCurrentDirectoryA(dir.string().c_str());
    }
    g_statusHandle = RegisterServiceCtrlHandlerExA(g_serviceName.c_str(), handlerEx, nullptr);
    if (!g_statusHandle) return;
    report(SERVICE_START_PENDING, NO_ERROR, 10000);
    bool ok = g_start && g_start();
    if (!ok) {
        report(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
        return;
    }
    report(SERVICE_RUNNING);
    if (g_wait) g_wait();
    if (g_stop) g_stop();
    report(SERVICE_STOPPED);
}

std::string lastErrorMessage() {
    DWORD code = GetLastError();
    char buf[256];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, buf, sizeof(buf), nullptr);
    std::string msg = n ? std::string(buf, n) : ("error " + std::to_string(code));
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' ')) msg.pop_back();
    return msg;
}

} // namespace

std::string winQuoteArg(const std::string& s) {
    if (s.find_first_of(" \t\"") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string winExecutablePath() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "";
    return std::string(buf, n);
}

bool winServiceInstall(const std::string& name, const std::string& displayName,
                       const std::string& binaryPath, std::string* error) {
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        if (error) *error = "OpenSCManager: " + lastErrorMessage();
        return false;
    }
    SC_HANDLE svc = CreateServiceA(
        scm, name.c_str(), displayName.c_str(),
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        binaryPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc) {
        if (error) *error = "CreateService: " + lastErrorMessage();
        CloseServiceHandle(scm);
        return false;
    }
    SERVICE_DESCRIPTIONA desc{};
    char text[] = "TestHub automated test daemon (HTTP API, Web UI, Runner bridge)";
    desc.lpDescription = text;
    ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

bool winServiceUninstall(const std::string& name, std::string* error) {
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        if (error) *error = "OpenSCManager: " + lastErrorMessage();
        return false;
    }
    SC_HANDLE svc = OpenServiceA(scm, name.c_str(), SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!svc) {
        if (error) *error = "OpenService: " + lastErrorMessage();
        CloseServiceHandle(scm);
        return false;
    }
    SERVICE_STATUS st{};
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    if (!DeleteService(svc)) {
        if (error) *error = "DeleteService: " + lastErrorMessage();
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return false;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

bool winServiceDispatch(const std::string& name,
                        const std::function<bool()>& start,
                        const std::function<void()>& wait,
                        const std::function<void()>& stop,
                        const std::function<void()>& requestStop) {
    g_serviceName = name.empty() ? "TestHub" : name;
    g_start = start;
    g_wait = wait;
    g_stop = stop;
    g_requestStop = requestStop;
    std::vector<char> nameBuf(g_serviceName.begin(), g_serviceName.end());
    nameBuf.push_back('\0');
    SERVICE_TABLE_ENTRYA table[] = {
        {nameBuf.data(), serviceMain},
        {nullptr, nullptr},
    };
    if (StartServiceCtrlDispatcherA(table)) return true;
    if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) return false;
    return true;
}

#endif
