#pragma once

#include <string_view>

namespace eagls {

// 返回核心库版本；结果指向静态存储区，可在整个进程生命周期内使用。
[[nodiscard]] std::string_view version() noexcept;

}
