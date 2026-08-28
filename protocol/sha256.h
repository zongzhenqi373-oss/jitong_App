#pragma once
// 纯 C++ 无依赖的 SHA-256 实现（header-only），客户端 client_core 与服务端 IMServer 共用。
// 用于登录/注册的密码哈希：客户端对密码做一次 SHA-256，服务端再加盐二次哈希后存库。

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace im {

class Sha256 {
public:
    Sha256() { reset(); }

    void reset()
    {
        m_len = 0;
        m_bitLen = 0;
        m_state[0] = 0x6a09e667;
        m_state[1] = 0xbb67ae85;
        m_state[2] = 0x3c6ef372;
        m_state[3] = 0xa54ff53a;
        m_state[4] = 0x510e527f;
        m_state[5] = 0x9b05688c;
        m_state[6] = 0x1f83d9ab;
        m_state[7] = 0x5be0cd19;
    }

    void update(const unsigned char* data, std::size_t len)
    {
        for (std::size_t i = 0; i < len; ++i) {
            m_data[m_len++] = data[i];
            if (m_len == 64) { transform(); m_bitLen += 512; m_len = 0; }
        }
    }

    void update(const char* data, std::size_t len)
    {
        update(reinterpret_cast<const unsigned char*>(data), len);
    }

    void update(const std::string& s) { update(s.data(), s.size()); }

    std::vector<unsigned char> final()
    {
        const std::size_t i = m_len;
        // 填充：先补 0x80，再补 0，最后 8 字节存原始比特长度（大端）
        m_data[m_len++] = 0x80;
        if (m_len > 56) {
            while (m_len < 64) m_data[m_len++] = 0x00;
            transform();
            m_len = 0;
        }
        while (m_len < 56) m_data[m_len++] = 0x00;
        const std::uint64_t totalBits = m_bitLen + static_cast<std::uint64_t>(i) * 8;
        for (int k = 7; k >= 0; --k) {
            m_data[m_len++] = static_cast<unsigned char>((totalBits >> (k * 8)) & 0xFF);
        }
        transform();

        std::vector<unsigned char> out(32);
        for (int j = 0; j < 8; ++j) {
            out[j * 4 + 0] = static_cast<unsigned char>((m_state[j] >> 24) & 0xFF);
            out[j * 4 + 1] = static_cast<unsigned char>((m_state[j] >> 16) & 0xFF);
            out[j * 4 + 2] = static_cast<unsigned char>((m_state[j] >> 8) & 0xFF);
            out[j * 4 + 3] = static_cast<unsigned char>(m_state[j] & 0xFF);
        }
        return out;
    }

private:
    static constexpr std::uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };

    void transform()
    {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(m_data[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(m_data[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(m_data[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(m_data[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
        std::uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];

        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + S1 + ch + K[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = S0 + maj;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        m_state[0] += a; m_state[1] += b; m_state[2] += c; m_state[3] += d;
        m_state[4] += e; m_state[5] += f; m_state[6] += g; m_state[7] += h;
    }

    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    unsigned char m_data[64];
    std::size_t m_len = 0;
    std::uint64_t m_bitLen = 0;
    std::uint32_t m_state[8];
};

// 计算 SHA-256 并返回小写十六进制字符串（64 字符）
inline std::string sha256Hex(const std::string& input)
{
    Sha256 h;
    h.update(input);
    const std::vector<unsigned char> digest = h.final();
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned char byte : digest) {
        out.push_back(hex[(byte >> 4) & 0x0F]);
        out.push_back(hex[byte & 0x0F]);
    }
    return out;
}

} // namespace im
