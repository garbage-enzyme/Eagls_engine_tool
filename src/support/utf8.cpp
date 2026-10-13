#include "support/utf8.hpp"

#include <cstddef>
#include <cstdint>

namespace eagls::detail {

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t position = 0;
    while (position < text.size()) {
        const auto lead = static_cast<unsigned char>(text[position]);
        if (lead < 0x80) {
            ++position;
            continue;
        }

        // 后续字节数、该长度允许的最小码点（用来拒绝过长编码）以及首字节携带的位。
        std::size_t continuation_count = 0;
        std::uint32_t minimum = 0;
        std::uint32_t code_point = 0;
        if (lead >= 0xC2 && lead <= 0xDF) {
            continuation_count = 1;
            minimum = 0x80;
            code_point = lead & 0x1Fu;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            continuation_count = 2;
            minimum = 0x800;
            code_point = lead & 0x0Fu;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            continuation_count = 3;
            minimum = 0x10000;
            code_point = lead & 0x07u;
        } else {
            // 0x80..0xC1 是孤立的后续字节或必然过长的首字节，0xF5 以上超出 Unicode 范围。
            return false;
        }

        if (position + continuation_count >= text.size()) {
            return false;  // 序列被截断
        }
        for (std::size_t i = 1; i <= continuation_count; ++i) {
            const auto next = static_cast<unsigned char>(text[position + i]);
            if ((next & 0xC0u) != 0x80u) {
                return false;
            }
            code_point = (code_point << 6) | (next & 0x3Fu);
        }

        const bool is_surrogate = code_point >= 0xD800 && code_point <= 0xDFFF;
        if (code_point < minimum || code_point > 0x10FFFF || is_surrogate) {
            return false;
        }
        position += continuation_count + 1;
    }
    return true;
}

}  // namespace eagls::detail
