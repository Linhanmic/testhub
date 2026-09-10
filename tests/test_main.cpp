#include "test_framework.h"
#include "util/logger.h"

int main(int argc, char** argv) {
    // 测试输出保持整洁：仅打印警告及以上级别日志
    testhub::Logger::getInstance().setLevel(testhub::LogLevel::Warn);
    return tf::runAll(argc, argv);
}
