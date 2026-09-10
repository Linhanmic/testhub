/*
 * TestHub - Base64 编解码
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace testhub {

class Base64 {
public:
    static std::string encode(const unsigned char* data, size_t len) {
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((len + 2) / 3) * 4);
        size_t i = 0;
        while (i + 2 < len) {
            uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
            out.push_back(table[(v >> 18) & 63]);
            out.push_back(table[(v >> 12) & 63]);
            out.push_back(table[(v >> 6) & 63]);
            out.push_back(table[v & 63]);
            i += 3;
        }
        if (i < len) {
            uint32_t v = static_cast<uint32_t>(data[i]) << 16;
            if (i + 1 < len) v |= static_cast<uint32_t>(data[i + 1]) << 8;
            out.push_back(table[(v >> 18) & 63]);
            out.push_back(table[(v >> 12) & 63]);
            out.push_back(i + 1 < len ? table[(v >> 6) & 63] : '=');
            out.push_back('=');
        }
        return out;
    }

    static std::string encode(const std::string& s) {
        return encode(reinterpret_cast<const unsigned char*>(s.data()), s.size());
    }

    static std::string decode(const std::string& in) {
        auto value = [](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+' || c == '-') return 62;
            if (c == '/' || c == '_') return 63;
            return -1;
        };
        std::string out;
        uint32_t buf = 0;
        int bits = 0;
        for (char c : in) {
            if (c == '=') break;
            int v = value(c);
            if (v < 0) continue;
            buf = (buf << 6) | static_cast<uint32_t>(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<char>((buf >> bits) & 0xFF));
            }
        }
        return out;
    }
};

} // namespace testhub
