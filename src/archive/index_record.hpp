#pragma once

#include <cstdint>

#include <eagls/archive.hpp>

#include "support/bytes.hpp"

namespace eagls::detail {

// 读取记录里的索引偏移；字段宽度由布局决定（28 字节布局 4 字节，40 字节布局 8 字节）。
inline std::uint64_t read_index_offset(const std::uint8_t* record,
                                       const archive_format::IndexFormat& format) noexcept {
    if (format.offset_width == 8) {
        return read_u64_le(record + format.offset_position);
    }
    return read_u32_le(record + format.offset_position);
}

// 写入记录里的索引偏移；调用方必须先保证 value 能放进 offset_width 字节。
inline void write_index_offset(std::uint8_t* record, const archive_format::IndexFormat& format,
                               std::uint64_t value) noexcept {
    if (format.offset_width == 8) {
        write_u64_le(record + format.offset_position, value);
        return;
    }
    write_u32_le(record + format.offset_position, static_cast<std::uint32_t>(value));
}

}  // namespace eagls::detail
