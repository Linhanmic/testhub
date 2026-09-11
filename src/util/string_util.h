/*
 * TestHub - 字符串工具
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace testhub {

/**
 * 字符串工具类
 */
class StringUtil {
public:
    /**
     * 转换为小写
     */
    static std::string toLower(const std::string& str) {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return result;
    }

    /**
     * 转换为大写
     */
    static std::string toUpper(const std::string& str) {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return result;
    }

    /**
     * 去除首尾空格
     */
    static std::string trim(const std::string& str) {
        std::string result = str;
        result.erase(0, result.find_first_not_of(" \t\n\r\f\v"));
        result.erase(result.find_last_not_of(" \t\n\r\f\v") + 1);
        return result;
    }

    /**
     * 分割字符串
     */
    static std::vector<std::string> split(const std::string& str, const std::string& delimiter) {
        std::vector<std::string> tokens;
        
        if (str.empty()) {
            return tokens;
        }
        
        size_t start = 0;
        size_t end = 0;
        
        while ((end = str.find(delimiter, start)) != std::string::npos) {
            std::string token = str.substr(start, end - start);
            tokens.push_back(token);
            start = end + delimiter.length();
        }
        
        // 添加最后一个 token
        std::string token = str.substr(start);
        tokens.push_back(token);
        
        return tokens;
    }

    /**
     * 连接字符串
     */
    static std::string join(const std::vector<std::string>& strings, const std::string& delimiter) {
        if (strings.empty()) {
            return "";
        }
        
        std::ostringstream oss;
        for (size_t i = 0; i < strings.size(); ++i) {
            if (i > 0) {
                oss << delimiter;
            }
            oss << strings[i];
        }
        
        return oss.str();
    }

    /**
     * 替换字符串
     */
    static std::string replace(const std::string& str, const std::string& from, const std::string& to) {
        std::string result = str;
        size_t pos = 0;
        
        while ((pos = result.find(from, pos)) != std::string::npos) {
            result.replace(pos, from.length(), to);
            pos += to.length();
        }
        
        return result;
    }

    /**
     * 检查是否以指定字符串开头
     */
    static bool startsWith(const std::string& str, const std::string& prefix) {
        if (prefix.length() > str.length()) {
            return false;
        }
        return str.compare(0, prefix.length(), prefix) == 0;
    }

    /**
     * 检查是否以指定字符串结尾
     */
    static bool endsWith(const std::string& str, const std::string& suffix) {
        if (suffix.length() > str.length()) {
            return false;
        }
        return str.compare(str.length() - suffix.length(), suffix.length(), suffix) == 0;
    }

    /**
     * 检查是否包含指定字符串
     */
    static bool contains(const std::string& str, const std::string& substr) {
        return str.find(substr) != std::string::npos;
    }

    /**
     * 转换为整数
     */
    static int toInt(const std::string& str) {
        try {
            return std::stoi(str);
        } catch (...) {
            return 0;
        }
    }

    /**
     * 转换为浮点数
     */
    static double toDouble(const std::string& str) {
        try {
            return std::stod(str);
        } catch (...) {
            return 0.0;
        }
    }

    /**
     * 转换为布尔值
     */
    static bool toBool(const std::string& str) {
        std::string lower = toLower(str);
        return lower == "true" || lower == "1" || lower == "yes";
    }

    /**
     * 检查是否为空
     */
    static bool isEmpty(const std::string& str) {
        return str.empty();
    }

    /**
     * 检查是否为空白
     */
    static bool isBlank(const std::string& str) {
        return trim(str).empty();
    }
};

} // namespace testhub
