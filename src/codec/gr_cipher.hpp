#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace eagls::detail {

// DAT 正文与 GR 前缀共用的异或密钥：12 字节 ASCII，不含结尾 NUL。
inline constexpr std::string_view content_key = "EAGLS_SYSTEM";

// GR 前缀异或使用的 Lehmer 随机数：按引擎 sub_422530 的 Schrage 分解实现，
// 避免有符号乘法溢出；种子先与常量 123459876 异或，再开始递推。
class GrRandom {
public:
    explicit GrRandom(std::uint8_t seed) : state_(seed ^ 123459876) {}

    unsigned next() {
        state_ = 48271 * (state_ % 44488) - 3399 * (state_ / 44488);
        if (state_ <= 0) {
            state_ += 2147483647;
        }
        return static_cast<unsigned>(static_cast<double>(state_) * 4.656612875245797e-10 * 256.0);
    }

private:
    std::int32_t state_;
};

// 对 GR 的前 size 字节做异或；加密与解密是同一个操作。
// size 通常取 content_format::gr_encrypted_size（5963），data 至少有这么多字节。
inline void xor_gr_prefix(std::uint8_t* data, std::size_t size, std::uint8_t seed) {
    GrRandom random(seed);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] ^= static_cast<std::uint8_t>(content_key[random.next() % content_key.size()]);
    }
}

}  // namespace eagls::detail
