/*
 * TestHub - 持久化自动化测试系统
 * 程序入口
 */

#include "testhub.h"

#include <iostream>
#include <string>
#include <fstream>
#include <csignal>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#endif

// 全局变量
static testhub::TestHub* g_testhub = nullptr;
static std::atomic<bool> g_running{true};

/**
 * 信号处理
 */
void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    g_running = false;
    
    if (g_testhub) {
        g_testhub->stop();
    }
}

/**
 * 打印使用说明
 */
void printUsage(const char* programName) {
    std::cout << "TestHub - 持久化自动化测试系统" << std::endl;
    std::cout << std::endl;
    std::cout << "用法: " << programName << " [选项]" << std::endl;
    std::cout << std::endl;
    std::cout << "选项:" << std::endl;
    std::cout << "  -p, --port <port>           服务器端口 (默认: 8080)" << std::endl;
    std::cout << "  -l, --language <language>   Runner 语言 (默认: java)" << std::endl;
    std::cout << "  -d, --dir <path>            项目目录 (默认: 当前目录)" << std::endl;
    std::cout << "  --specs <path>              规范目录 (默认: specs)" << std::endl;
    std::cout << "  --daemon                    后台运行模式" << std::endl;
    std::cout << "  --pid-file <path>           PID 文件路径" << std::endl;
    std::cout << "  -h, --help                  显示帮助" << std::endl;
    std::cout << "  -v, --version               显示版本" << std::endl;
    std::cout << std::endl;
    std::cout << "示例:" << std::endl;
    std::cout << "  " << programName << " --port 8080 --language java" << std::endl;
    std::cout << "  " << programName << " --daemon --pid-file /var/run/testhub.pid" << std::endl;
    std::cout << std::endl;
    std::cout << "API 端点:" << std::endl;
    std::cout << "  POST /api/v1/tests/run      提交测试" << std::endl;
    std::cout << "  GET  /api/v1/tests/{id}      查询测试状态" << std::endl;
    std::cout << "  GET  /api/v1/tests           列出所有测试" << std::endl;
    std::cout << "  GET  /api/v1/runner/status   Runner 状态" << std::endl;
    std::cout << "  GET  /api/v1/health          健康检查" << std::endl;
}

/**
 * 打印版本
 */
void printVersion() {
    std::cout << "TestHub version 1.0.0" << std::endl;
}

/**
 * 写入 PID 文件
 */
bool writePidFile(const std::string& pidFile) {
#ifdef _WIN32
    DWORD pid = GetCurrentProcessId();
#else
    pid_t pid = getpid();
#endif
    
    std::ofstream file(pidFile);
    if (file.is_open()) {
        file << pid;
        file.close();
        return true;
    }
    
    return false;
}

/**
 * 主函数
 */
int main(int argc, char* argv[]) {
    // 解析命令行参数
    testhub::TestHubConfig config;
    bool daemonMode = false;
    std::string pidFile;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "-p" || arg == "--port") {
            if (i + 1 < argc) {
                config.port = std::stoi(argv[++i]);
            }
        } else if (arg == "-l" || arg == "--language") {
            if (i + 1 < argc) {
                config.runnerLanguage = argv[++i];
            }
        } else if (arg == "-d" || arg == "--dir") {
            if (i + 1 < argc) {
                config.projectPath = argv[++i];
            }
        } else if (arg == "--specs") {
            if (i + 1 < argc) {
                config.specsDir = argv[++i];
            }
        } else if (arg == "--daemon") {
            daemonMode = true;
        } else if (arg == "--pid-file") {
            if (i + 1 < argc) {
                pidFile = argv[++i];
            }
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            printVersion();
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }
    
    // 设置信号处理
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    // 初始化 TestHub
    testhub::TestHub& testhub = testhub::TestHub::getInstance();
    g_testhub = &testhub;
    
    if (!testhub.initialize(config)) {
        std::cerr << "Failed to initialize TestHub" << std::endl;
        return 1;
    }
    
    // 写入 PID 文件
    if (!pidFile.empty()) {
        if (!writePidFile(pidFile)) {
            std::cerr << "Failed to write PID file: " << pidFile << std::endl;
            return 1;
        }
    }
    
    // 启动服务器
    if (!testhub.start()) {
        std::cerr << "Failed to start TestHub" << std::endl;
        return 1;
    }
    
    std::cout << "TestHub is running. Press Ctrl+C to stop." << std::endl;
    
    // 主循环
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    // 停止服务器
    testhub.stop();
    
    // 清理 PID 文件
    if (!pidFile.empty()) {
        std::remove(pidFile.c_str());
    }
    
    std::cout << "TestHub stopped." << std::endl;
    
    return 0;
}
