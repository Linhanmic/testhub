/*
 * TestHub - 持久化自动化测试系统
 * 程序入口：解析命令行、加载配置、启动服务器并等待退出信号
 */

#include "testhub.h"
#include "util/file_util.h"
#include "util/logger.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

std::atomic<bool> g_shutdownRequested{false};
std::mutex g_shutdownMutex;
std::condition_variable g_shutdownCv;

void handleSignal(int) {
    g_shutdownRequested = true;
    g_shutdownCv.notify_all();
}

void printUsage(const char* program) {
    std::cout
        << "TestHub " << testhub::TestHub::version() << " - 持久化自动化测试系统\n\n"
        << "用法: " << program << " [选项]\n\n"
        << "选项:\n"
        << "  -p, --port <port>          HTTP 监听端口（默认 8080，0 表示自动分配）\n"
        << "  -H, --host <host>          监听地址（默认 0.0.0.0）\n"
        << "  -l, --language <lang>      Runner 语言：mock | python | node | custom（默认 mock）\n"
        << "  -r, --runner-cmd <cmd>     自定义 Runner 启动命令（覆盖语言默认值）\n"
        << "  -d, --dir <path>           测试项目目录（Runner 工作目录）\n"
        << "  -s, --specs <path>         规范目录（默认 specs）\n"
        << "      --concepts <path>      概念(.cpt)目录（默认与规范目录相同）\n"
        << "  -c, --config <file>        JSON 配置文件\n"
        << "  -j, --concurrency <n>      并发执行的测试数（默认 1）\n"
        << "      --timeout <ms>         测试默认超时（毫秒）\n"
        << "      --results-dir <path>   结果持久化目录（默认 data/results）\n"
        << "      --no-persist           不持久化结果，仅保存在内存\n"
        << "      --log-level <level>    debug | info | warn | error | off\n"
        << "      --log-file <file>      日志文件\n"
        << "      --no-ui                不提供 Web UI\n"
        << "      --web-dir <path>       从磁盘目录提供 Web UI（前端开发模式）\n"
        << "      --pid-file <file>      写入 PID 文件\n"
#ifndef _WIN32
        << "      --daemon               以守护进程方式运行（POSIX）\n"
#endif
        << "      --print-config         打印最终生效配置并退出\n"
        << "  -v, --version              显示版本\n"
        << "  -h, --help                 显示帮助\n\n"
        << "示例:\n"
        << "  " << program << " --port 8080 --specs ./specs\n"
        << "  " << program << " --language python --dir ./my-tests\n"
        << "  " << program << " --config testhub.json\n";
}

bool needsValue(const std::string& opt) {
    static const char* withValue[] = {
        "-p", "--port", "-H", "--host", "-l", "--language", "-r", "--runner-cmd", "-d", "--dir",
        "-s", "--specs", "--concepts", "-c", "--config", "-j", "--concurrency", "--timeout",
        "--results-dir", "--log-level", "--log-file", "--web-dir", "--pid-file"};
    for (const char* w : withValue) {
        if (opt == w) return true;
    }
    return false;
}

int parseIntOrExit(const std::string& opt, const std::string& value) {
    try {
        size_t idx = 0;
        int v = std::stoi(value, &idx);
        if (idx != value.size()) throw std::invalid_argument("trailing characters");
        return v;
    } catch (const std::exception&) {
        std::cerr << "错误: 选项 " << opt << " 需要整数参数，得到 '" << value << "'\n";
        std::exit(2);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    testhub::TestHubConfig config;
    std::string configFile;
    std::string pidFile;
    bool daemonize = false;
    bool printConfig = false;

    // 第一遍：先找配置文件，使命令行选项能覆盖配置文件
    for (int i = 1; i < argc; ++i) {
        std::string opt = argv[i];
        if ((opt == "-c" || opt == "--config") && i + 1 < argc) configFile = argv[i + 1];
    }
    if (!configFile.empty()) {
        if (!testhub::FileUtil::fileExists(configFile)) {
            std::cerr << "错误: 配置文件不存在: " << configFile << "\n";
            return 2;
        }
        std::string error;
        testhub::Json json = testhub::Json::tryParse(testhub::FileUtil::readFile(configFile), &error);
        if (!error.empty()) {
            std::cerr << "错误: 配置文件解析失败: " << error << "\n";
            return 2;
        }
        config.applyJson(json);
    }

    for (int i = 1; i < argc; ++i) {
        std::string opt = argv[i];
        std::string value;
        if (needsValue(opt)) {
            if (i + 1 >= argc) {
                std::cerr << "错误: 选项 " << opt << " 缺少参数\n";
                return 2;
            }
            value = argv[++i];
        }

        if (opt == "-h" || opt == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (opt == "-v" || opt == "--version") {
            std::cout << "TestHub " << testhub::TestHub::version() << "\n";
            return 0;
        } else if (opt == "-p" || opt == "--port") {
            config.port = parseIntOrExit(opt, value);
        } else if (opt == "-H" || opt == "--host") {
            config.host = value;
        } else if (opt == "-l" || opt == "--language") {
            config.runnerLanguage = value;
        } else if (opt == "-r" || opt == "--runner-cmd") {
            config.runnerCommand = value;
            if (config.runnerLanguage == "mock") config.runnerLanguage = "custom";
        } else if (opt == "-d" || opt == "--dir") {
            config.projectPath = value;
        } else if (opt == "-s" || opt == "--specs") {
            config.specsDir = value;
        } else if (opt == "--concepts") {
            config.conceptsDir = value;
        } else if (opt == "-c" || opt == "--config") {
            // 已在第一遍处理
        } else if (opt == "-j" || opt == "--concurrency") {
            config.maxConcurrentTests = parseIntOrExit(opt, value);
        } else if (opt == "--timeout") {
            config.defaultTimeout = parseIntOrExit(opt, value);
        } else if (opt == "--results-dir") {
            config.resultsDir = value;
        } else if (opt == "--no-persist") {
            config.resultsDir.clear();
        } else if (opt == "--log-level") {
            config.logLevel = value;
        } else if (opt == "--log-file") {
            config.logFile = value;
        } else if (opt == "--no-ui") {
            config.enableWebUi = false;
        } else if (opt == "--web-dir") {
            config.webDir = value;
        } else if (opt == "--pid-file") {
            pidFile = value;
        } else if (opt == "--daemon") {
            daemonize = true;
        } else if (opt == "--print-config") {
            printConfig = true;
        } else {
            std::cerr << "错误: 未知选项 " << opt << "\n\n";
            printUsage(argv[0]);
            return 2;
        }
    }

    if (config.port < 0 || config.port > 65535) {
        std::cerr << "错误: 端口必须在 0-65535 之间\n";
        return 2;
    }
    if (config.maxConcurrentTests < 1) config.maxConcurrentTests = 1;

    if (printConfig) {
        std::cout << config.toJson().dump(2) << "\n";
        return 0;
    }

#ifndef _WIN32
    if (daemonize) {
        pid_t pid = fork();
        if (pid < 0) {
            std::cerr << "错误: fork 失败: " << std::strerror(errno) << "\n";
            return 1;
        }
        if (pid > 0) {
            std::cout << "TestHub 已在后台启动 (pid " << pid << ")\n";
            return 0;
        }
        if (setsid() < 0) return 1;
        if (config.logFile.empty()) config.logFile = "testhub.log";
        testhub::Logger::getInstance().setConsole(false);
        (void)!freopen("/dev/null", "r", stdin);
        (void)!freopen("/dev/null", "w", stdout);
        (void)!freopen("/dev/null", "w", stderr);
    }
#else
    if (daemonize) {
        std::cerr << "警告: --daemon 在 Windows 上不受支持，将以前台方式运行\n";
    }
#endif

    if (!pidFile.empty()) {
        std::ofstream pf(pidFile);
#ifndef _WIN32
        pf << getpid() << "\n";
#else
        pf << "0\n";
#endif
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGHUP, handleSignal);
#endif

    testhub::TestHub hub;
    if (!hub.initialize(config)) {
        std::cerr << "错误: TestHub 初始化失败\n";
        return 1;
    }
    if (!hub.start()) {
        std::cerr << "错误: TestHub 启动失败（端口 " << config.port << " 是否已被占用？）\n";
        return 1;
    }

    if (!daemonize) {
        std::cout << "\n  TestHub " << testhub::TestHub::version() << " 已启动\n"
                  << "  Web UI : http://" << (config.host == "0.0.0.0" ? "localhost" : config.host) << ":" << hub.boundPort() << "/\n"
                  << "  API    : http://" << (config.host == "0.0.0.0" ? "localhost" : config.host) << ":" << hub.boundPort() << "/api/v1/\n"
                  << "  Runner : " << config.runnerLanguage << "\n"
                  << "  Specs  : " << hub.getSpecs().specsDir() << "\n"
                  << "  按 Ctrl+C 停止\n\n";
    }

    {
        std::unique_lock<std::mutex> lock(g_shutdownMutex);
        // 信号处理函数不能安全地持锁，因此用带超时的等待避免丢失唤醒
        while (!g_shutdownRequested.load()) {
            g_shutdownCv.wait_for(lock, std::chrono::milliseconds(200));
        }
    }

    std::cout << "\n正在停止 TestHub...\n";
    hub.stop();
    if (!pidFile.empty()) testhub::FileUtil::deleteFile(pidFile);
    return 0;
}
