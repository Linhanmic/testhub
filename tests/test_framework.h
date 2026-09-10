/*
 * TestHub - 微型测试框架（零依赖）
 *
 * 用法：
 *   TEST_CASE(name) { CHECK(a == b); REQUIRE(ptr != nullptr); CHECK_EQ(x, 3); }
 */

#pragma once

#include <chrono>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace tf {

struct Failure : std::exception {
    std::string message;
    explicit Failure(std::string m) : message(std::move(m)) {}
    const char* what() const noexcept override { return message.c_str(); }
};

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

struct Context {
    int checksFailed = 0;
    std::vector<std::string> messages;
};

inline Context*& current() {
    static Context* ctx = nullptr;
    return ctx;
}

inline void recordFailure(const std::string& file, int line, const std::string& expr, const std::string& detail) {
    std::ostringstream os;
    os << file << ":" << line << ": CHECK failed: " << expr;
    if (!detail.empty()) os << "\n      " << detail;
    if (current()) {
        current()->checksFailed++;
        current()->messages.push_back(os.str());
    } else {
        std::cerr << os.str() << std::endl;
    }
}

template <typename T>
std::string show(const T& v) {
    std::ostringstream os;
    os << v;
    return os.str();
}
inline std::string show(bool v) { return v ? "true" : "false"; }
inline std::string show(const std::string& v) { return "\"" + v + "\""; }
inline std::string show(const char* v) { return "\"" + std::string(v) + "\""; }

// 用法: <binary> [--filter <substr> | <substr>] [--list]
inline int runAll(int argc, char** argv) {
    std::string filter;
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--list") listOnly = true;
        else if (arg == "--filter" && i + 1 < argc) filter = argv[++i];
        else if (!arg.empty() && arg[0] != '-') filter = arg;
    }
    if (listOnly) {
        for (auto& t : registry()) {
            if (filter.empty() || t.name.find(filter) != std::string::npos) std::cout << t.name << "\n";
        }
        return 0;
    }
    int passed = 0, failed = 0, skipped = 0;
    auto suiteStart = std::chrono::steady_clock::now();
    for (auto& t : registry()) {
        if (!filter.empty() && t.name.find(filter) == std::string::npos) {
            ++skipped;
            continue;
        }
        Context ctx;
        current() = &ctx;
        auto start = std::chrono::steady_clock::now();
        try {
            t.fn();
        } catch (const Failure& f) {
            ctx.checksFailed++;
            ctx.messages.push_back(f.message);
        } catch (const std::exception& e) {
            ctx.checksFailed++;
            ctx.messages.push_back(std::string("unexpected exception: ") + e.what());
        } catch (...) {
            ctx.checksFailed++;
            ctx.messages.push_back("unexpected non-standard exception");
        }
        current() = nullptr;
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (ctx.checksFailed == 0) {
            ++passed;
            std::cout << "[ PASS ] " << t.name << " (" << static_cast<int>(ms) << " ms)\n";
        } else {
            ++failed;
            std::cout << "[ FAIL ] " << t.name << " (" << static_cast<int>(ms) << " ms)\n";
            for (auto& m : ctx.messages) std::cout << "    " << m << "\n";
        }
    }
    double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - suiteStart).count();
    std::cout << "\n" << passed << " passed, " << failed << " failed";
    if (skipped) std::cout << ", " << skipped << " skipped";
    std::cout << " (" << static_cast<int>(total) << " ms)\n";
    return failed == 0 ? 0 : 1;
}

} // namespace tf

#define TF_CONCAT_INNER(a, b) a##b
#define TF_CONCAT(a, b) TF_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                                       \
    static void TF_CONCAT(tf_test_, __LINE__)();                                              \
    static ::tf::Registrar TF_CONCAT(tf_reg_, __LINE__)(name, &TF_CONCAT(tf_test_, __LINE__)); \
    static void TF_CONCAT(tf_test_, __LINE__)()

#define CHECK(expr)                                                                \
    do {                                                                           \
        if (!(expr)) ::tf::recordFailure(__FILE__, __LINE__, #expr, "");           \
    } while (0)

#define CHECK_MSG(expr, msg)                                                       \
    do {                                                                           \
        if (!(expr)) ::tf::recordFailure(__FILE__, __LINE__, #expr, (msg));        \
    } while (0)

#define CHECK_EQ(a, b)                                                                                  \
    do {                                                                                                \
        auto tf_a = (a);                                                                                \
        auto tf_b = (b);                                                                                \
        if (!(tf_a == tf_b))                                                                            \
            ::tf::recordFailure(__FILE__, __LINE__, #a " == " #b,                                       \
                                "left: " + ::tf::show(tf_a) + "  right: " + ::tf::show(tf_b));          \
    } while (0)

#define REQUIRE(expr)                                                                        \
    do {                                                                                     \
        if (!(expr)) throw ::tf::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                                         ": REQUIRE failed: " #expr);                        \
    } while (0)

#define REQUIRE_EQ(a, b)                                                                                    \
    do {                                                                                                    \
        auto tf_a = (a);                                                                                    \
        auto tf_b = (b);                                                                                    \
        if (!(tf_a == tf_b))                                                                                \
            throw ::tf::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) +                    \
                                ": REQUIRE failed: " #a " == " #b "\n      left: " + ::tf::show(tf_a) +     \
                                "  right: " + ::tf::show(tf_b));                                            \
    } while (0)

#define CHECK_THROWS(expr, ExceptionType)                                                    \
    do {                                                                                     \
        bool tf_thrown = false;                                                              \
        try { (void)(expr); } catch (const ExceptionType&) { tf_thrown = true; } catch (...) {} \
        if (!tf_thrown) ::tf::recordFailure(__FILE__, __LINE__, #expr, "expected " #ExceptionType); \
    } while (0)
