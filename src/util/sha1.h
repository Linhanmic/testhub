/*
 * TestHub - SHA-1（用于 WebSocket 握手）
 */

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace testhub {

class Sha1 {
public:
    static std::array<unsigned char, 20> digest(const std::string& input) {
        uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;

        std::string msg = input;
        uint64_t bitLen = static_cast<uint64_t>(input.size()) * 8;
        msg.push_back(static_cast<char>(0x80));
        while (msg.size() % 64 != 56) msg.push_back(0);
        for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bitLen >> (i * 8)) & 0xFF));

        for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
            uint32_t w[80];
            for (int i = 0; i < 16; ++i) {
                w[i] = (static_cast<uint32_t>(static_cast<unsigned char>(msg[chunk + i * 4])) << 24) |
                       (static_cast<uint32_t>(static_cast<unsigned char>(msg[chunk + i * 4 + 1])) << 16) |
                       (static_cast<uint32_t>(static_cast<unsigned char>(msg[chunk + i * 4 + 2])) << 8) |
                       static_cast<uint32_t>(static_cast<unsigned char>(msg[chunk + i * 4 + 3]));
            }
            for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

            uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
            for (int i = 0; i < 80; ++i) {
                uint32_t f, k;
                if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
                else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
                else { f = b ^ c ^ d; k = 0xCA62C1D6; }
                uint32_t temp = rotl(a, 5) + f + e + k + w[i];
                e = d; d = c; c = rotl(b, 30); b = a; a = temp;
            }
            h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
        }

        std::array<unsigned char, 20> out{};
        uint32_t hs[5] = {h0, h1, h2, h3, h4};
        for (int i = 0; i < 5; ++i) {
            out[i * 4] = static_cast<unsigned char>((hs[i] >> 24) & 0xFF);
            out[i * 4 + 1] = static_cast<unsigned char>((hs[i] >> 16) & 0xFF);
            out[i * 4 + 2] = static_cast<unsigned char>((hs[i] >> 8) & 0xFF);
            out[i * 4 + 3] = static_cast<unsigned char>(hs[i] & 0xFF);
        }
        return out;
    }

    static std::string hex(const std::string& input) {
        auto d = digest(input);
        static const char* hexChars = "0123456789abcdef";
        std::string out;
        for (unsigned char c : d) {
            out.push_back(hexChars[c >> 4]);
            out.push_back(hexChars[c & 15]);
        }
        return out;
    }

private:
    static uint32_t rotl(uint32_t v, int bits) { return (v << bits) | (v >> (32 - bits)); }
};

} // namespace testhub
