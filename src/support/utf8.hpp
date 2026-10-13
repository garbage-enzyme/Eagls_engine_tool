#pragma once

#include <string_view>

namespace eagls::detail {

// 严格校验 UTF-8：拒绝过长编码、代理项（U+D800..U+DFFF）、超过 U+10FFFF 的码点
// 以及被截断的序列。空串视为合法。
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

}  // namespace eagls::detail
