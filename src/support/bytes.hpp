#pragma once

#include <cstdint>

namespace eagls::detail {

// 显式读取磁盘小端字段，不依赖主机字节序或结构体布局。
inline std::uint16_t read_u16_le(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(bytes[0] | (static_cast<unsigned>(bytes[1]) << 8));
}

inline std::uint32_t read_u32_le(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

inline std::int32_t read_i32_le(const std::uint8_t* bytes) noexcept {
    const auto value = read_u32_le(bytes);
    const auto signed_value = value <= 0x7FFFFFFFu ? static_cast<std::int64_t>(value)
                                                : static_cast<std::int64_t>(value) - 0x100000000LL;
    return static_cast<std::int32_t>(signed_value);
}

inline void write_u32_le(std::uint8_t* bytes, std::uint32_t value) noexcept {
    for (unsigned i = 0; i < 4; ++i) {
        bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

}  // namespace eagls::detail
