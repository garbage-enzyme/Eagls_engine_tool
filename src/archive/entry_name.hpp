#pragma once

#include <string>
#include <string_view>

namespace eagls::detail {

// 把 ASCII 字母 A-Z 折叠为小写，其余字节（包括多字节 UTF-8）保持不变。
// 只折叠 ASCII 是有意的：结果不依赖系统区域设置，也不会误合并不同的非 ASCII 名称。
[[nodiscard]] std::string fold_ascii_case(std::string_view name);

// 忽略 ASCII 大小写的相等比较，规则同 fold_ascii_case。
[[nodiscard]] bool equals_ignoring_ascii_case(std::string_view left, std::string_view right) noexcept;

// 条目名能否直接作为输出目录下的单个文件名。
// 拒绝：空名、"."、".."、路径分隔符、Windows 非法字符（<>:"/\|?*）、控制字符、
// 以空格或点结尾的名称，以及 Windows 保留设备名（CON、PRN、AUX、NUL、COM1-9、LPT1-9，
// 带扩展名时同样保留，例如 NUL.txt）。
[[nodiscard]] bool is_safe_file_name(std::string_view name) noexcept;

}  // namespace eagls::detail
