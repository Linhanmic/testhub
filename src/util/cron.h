/*
 * TestHub - 五字段 cron 表达式（UTC）
 * 格式：minute hour day-of-month month day-of-week
 * 支持 * , - / 与 JAN–DEC / SUN–SAT 名称，以及 @hourly/@daily/@weekly/@monthly/@yearly。
 * 日与星期都不是 * 时按 Vixie cron：二者满足其一即可。
 */

#pragma once

#include "time_util.h"

#include <cctype>
#include <cstdint>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace testhub {

class CronExpr {
public:
    CronExpr() = default;

    static bool parse(const std::string& text, CronExpr& out, std::string* error = nullptr) {
        CronExpr c;
        std::string raw = trimCopy(text);
        if (raw.empty()) return fail(error, "cron expression is empty");
        std::string expanded = expandAlias(raw);
        if (expanded.empty()) return fail(error, "unknown cron alias: " + raw);
        std::vector<std::string> fields = splitWs(expanded);
        if (fields.size() != 5) {
            return fail(error, "cron must have 5 fields (minute hour day month weekday), got " +
                                   std::to_string(fields.size()));
        }
        if (!parseField(fields[0], 0, 59, nullptr, 0, c.minute_, error, "minute")) return false;
        if (!parseField(fields[1], 0, 23, nullptr, 0, c.hour_, error, "hour")) return false;
        if (!parseField(fields[2], 1, 31, nullptr, 0, c.dom_, error, "day-of-month")) return false;
        if (!parseField(fields[3], 1, 12, kMonths, 12, c.month_, error, "month")) return false;
        if (!parseField(fields[4], 0, 7, kWeekdays, 7, c.dow_, error, "day-of-week")) return false;
        // 7 与 0 都表示周日
        if (c.dow_ & (1ull << 7)) c.dow_ |= 1ull;
        if (c.dow_ & 1ull) c.dow_ |= (1ull << 7);
        c.domAll_ = fields[2] == "*";
        c.dowAll_ = fields[4] == "*";
        c.source_ = raw;
        out = std::move(c);
        return true;
    }

    const std::string& source() const { return source_; }

    bool matches(const std::tm& utc) const {
        int min = utc.tm_min;
        int hour = utc.tm_hour;
        int mday = utc.tm_mday;
        int mon = utc.tm_mon + 1;
        int wday = utc.tm_wday;  // 0 = Sunday
        if (!bit(minute_, min) || !bit(hour_, hour) || !bit(month_, mon)) return false;
        if (domAll_ && dowAll_) return true;
        if (domAll_) return bit(dow_, wday);
        if (dowAll_) return bit(dom_, mday);
        return bit(dom_, mday) || bit(dow_, wday);
    }

    bool matches(TimeUtil::TimePoint tp) const {
        std::tm utc{};
        if (!toUtc(tp, utc)) return false;
        return matches(utc);
    }

    /** 返回 tp 之后（不含本分钟）下一次匹配；一年内找不到则返回零值 */
    TimeUtil::TimePoint nextAfter(TimeUtil::TimePoint tp) const {
        auto minute = std::chrono::duration_cast<std::chrono::minutes>(tp.time_since_epoch());
        TimeUtil::TimePoint cursor = TimeUtil::TimePoint(minute) + std::chrono::minutes(1);
        constexpr int kMax = 366 * 24 * 60;
        for (int i = 0; i < kMax; ++i) {
            if (matches(cursor)) return cursor;
            cursor += std::chrono::minutes(1);
        }
        return TimeUtil::TimePoint{};
    }

    static long long epochMinute(TimeUtil::TimePoint tp) {
        return std::chrono::duration_cast<std::chrono::minutes>(tp.time_since_epoch()).count();
    }

private:
    static constexpr const char* kMonths[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };
    static constexpr const char* kWeekdays[] = {
        "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
    };

    std::string source_;
    uint64_t minute_ = 0;
    uint64_t hour_ = 0;
    uint64_t dom_ = 0;
    uint64_t month_ = 0;
    uint64_t dow_ = 0;
    bool domAll_ = true;
    bool dowAll_ = true;

    static bool bit(uint64_t bits, int v) {
        if (v < 0 || v >= 64) return false;
        return (bits & (1ull << v)) != 0;
    }

    static bool fail(std::string* error, const std::string& msg) {
        if (error) *error = msg;
        return false;
    }

    static std::string trimCopy(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    }

    static std::string expandAlias(const std::string& raw) {
        if (raw[0] != '@') return raw;
        if (raw == "@yearly" || raw == "@annually") return "0 0 1 1 *";
        if (raw == "@monthly") return "0 0 1 * *";
        if (raw == "@weekly") return "0 0 * * 0";
        if (raw == "@daily" || raw == "@midnight") return "0 0 * * *";
        if (raw == "@hourly") return "0 * * * *";
        return "";
    }

    static std::vector<std::string> splitWs(const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char ch : s) {
            if (std::isspace(static_cast<unsigned char>(ch))) {
                if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            } else {
                cur.push_back(ch);
            }
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    static std::string toUpper(std::string s) {
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    static bool parseName(const std::string& tok, const char* const* names, int n, int& value) {
        std::string u = toUpper(tok);
        for (int i = 0; i < n; ++i) {
            if (u == names[i]) { value = i; return true; }
        }
        return false;
    }

    static bool parseInt(const std::string& tok, int minv, int maxv, const char* const* names, int nNames,
                         int nameBase, int& value, std::string* error, const char* field) {
        if (names && parseName(tok, names, nNames, value)) {
            value += nameBase;  // months: name index 0 → 1; weekdays: 0 → 0
            if (value < minv || value > maxv) return fail(error, std::string(field) + " name out of range");
            return true;
        }
        if (tok.empty()) return fail(error, std::string(field) + " is empty");
        int v = 0;
        for (char ch : tok) {
            if (!std::isdigit(static_cast<unsigned char>(ch))) {
                return fail(error, std::string(field) + " has invalid token '" + tok + "'");
            }
            v = v * 10 + (ch - '0');
            if (v > 99) break;
        }
        if (v < minv || v > maxv) {
            return fail(error, std::string(field) + " value " + tok + " out of range " +
                                   std::to_string(minv) + "-" + std::to_string(maxv));
        }
        value = v;
        return true;
    }

    static bool parseField(const std::string& field, int minv, int maxv, const char* const* names, int nNames,
                           uint64_t& bits, std::string* error, const char* label) {
        bits = 0;
        if (field.empty()) return fail(error, std::string(label) + " is empty");
        size_t start = 0;
        while (start <= field.size()) {
            size_t comma = field.find(',', start);
            std::string part = field.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (part.empty()) return fail(error, std::string(label) + " has an empty list item");
            int step = 1;
            size_t slash = part.find('/');
            std::string range = part;
            if (slash != std::string::npos) {
                range = part.substr(0, slash);
                int sv = 0;
                if (!parseInt(part.substr(slash + 1), 1, maxv - minv + 1, nullptr, 0, 0, sv, error, label)) return false;
                step = sv;
            }
            int begin = minv, end = maxv;
            if (range != "*") {
                size_t dash = range.find('-');
                if (dash != std::string::npos) {
                    if (!parseInt(range.substr(0, dash), minv, maxv, names, nNames, names && nNames == 12 ? 1 : 0,
                                  begin, error, label))
                        return false;
                    if (!parseInt(range.substr(dash + 1), minv, maxv, names, nNames, names && nNames == 12 ? 1 : 0,
                                  end, error, label))
                        return false;
                    if (end < begin) return fail(error, std::string(label) + " range is reversed");
                } else {
                    if (!parseInt(range, minv, maxv, names, nNames, names && nNames == 12 ? 1 : 0, begin, error, label))
                        return false;
                    end = begin;
                }
            }
            for (int v = begin; v <= end; v += step) bits |= (1ull << v);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (bits == 0) return fail(error, std::string(label) + " matches nothing");
        return true;
    }

    static bool toUtc(TimeUtil::TimePoint tp, std::tm& utc) {
        std::time_t t = std::chrono::system_clock::to_time_t(tp);
#ifdef _WIN32
        if (gmtime_s(&utc, &t) != 0) return false;
#else
        if (!gmtime_r(&t, &utc)) return false;
#endif
        return true;
    }
};

} // namespace testhub
