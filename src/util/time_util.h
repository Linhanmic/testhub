/*
 * TestHub - 时间工具
 */

#pragma once

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace testhub {

class TimeUtil {
public:
    using TimePoint = std::chrono::system_clock::time_point;

    /**
     * ISO 8601 UTC 格式，如 2024-01-01T10:00:00.123Z；零值返回空串
     */
    static std::string toIso8601(const TimePoint& tp) {
        if (tp.time_since_epoch().count() == 0) return "";
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
        std::time_t seconds = static_cast<std::time_t>(ms / 1000);
        int millis = static_cast<int>(ms % 1000);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &seconds);
#else
        gmtime_r(&seconds, &tm);
#endif
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec, millis);
        return buf;
    }

    /**
     * 解析 toIso8601 产生的格式（YYYY-MM-DDTHH:MM:SS[.mmm]Z）；空串或格式错误返回零值
     */
    static TimePoint fromIso8601(const std::string& text) {
        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, millis = 0;
        if (text.size() < 20) return TimePoint{};
        int n = std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second);
        if (n != 6) return TimePoint{};
        size_t dot = text.find('.', 19);
        if (dot != std::string::npos) {
            // 只取前三位小数
            int scale = 100;
            for (size_t i = dot + 1; i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) && scale > 0; ++i) {
                millis += (text[i] - '0') * scale;
                scale /= 10;
            }
        }
        std::tm tm{};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = second;
#ifdef _WIN32
        std::time_t t = _mkgmtime(&tm);
#else
        std::time_t t = timegm(&tm);
#endif
        if (t == static_cast<std::time_t>(-1)) return TimePoint{};
        return std::chrono::system_clock::from_time_t(t) + std::chrono::milliseconds(millis);
    }

    /**
     * 毫秒时间戳
     */
    static long long toEpochMillis(const TimePoint& tp) {
        if (tp.time_since_epoch().count() == 0) return 0;
        return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
    }

    static TimePoint now() { return std::chrono::system_clock::now(); }

    /**
     * 本地时间格式化（用于 ID 生成与日志）
     */
    static std::string formatLocal(const TimePoint& tp, const char* fmt = "%Y%m%d-%H%M%S") {
        std::time_t t = std::chrono::system_clock::to_time_t(tp);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), fmt, &tm);
        return buf;
    }

    /**
     * HTTP 日期格式（RFC 7231）
     */
    static std::string httpDate(const TimePoint& tp = now()) {
        std::time_t t = std::chrono::system_clock::to_time_t(tp);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
        return buf;
    }
};

} // namespace testhub
