/*
 * TestHub - Windows 服务（SCM）安装 / 卸载 / 调度
 * 仅在 _WIN32 下编译并链入 testhub.exe。
 */

#pragma once

#include <functional>
#include <string>

#ifdef _WIN32

/**
 * 把 testhub.exe 注册为自动启动的 Windows 服务。binaryPath 含 --service run 及配置参数。
 */
bool winServiceInstall(const std::string& name, const std::string& displayName,
                       const std::string& binaryPath, std::string* error);

bool winServiceUninstall(const std::string& name, std::string* error);

/**
 * 交给服务控制管理器。由 SCM 启动时阻塞直到服务停止并返回 true。
 * 在控制台直接运行（ERROR_FAILED_SERVICE_CONTROLLER_CONNECT）时返回 false，由调用方以前台方式继续。
 */
bool winServiceDispatch(const std::string& name,
                        const std::function<bool()>& start,
                        const std::function<void()>& wait,
                        const std::function<void()>& stop,
                        const std::function<void()>& requestStop);

/** 当前可执行文件的绝对路径 */
std::string winExecutablePath();

/** 为 CreateService 的 binPath 转义单个参数 */
std::string winQuoteArg(const std::string& s);

#endif
