#pragma once

#include <string>
#include <string_view>

namespace eagls::detail {

// Windows 代码页（如 932 = Shift-JIS/CP932、936 = GBK）与 UTF-8 的双向转换。
// 转换失败会抛出 eagls::Error(invalid_content)；调用方负责补充上下文
// （哪个文件、哪条文本），底层只报告"序列无法按该代码页转换"。
[[nodiscard]] std::string codepage_to_utf8(std::string_view bytes, unsigned codepage);
[[nodiscard]] std::string utf8_to_codepage(std::string_view utf8, unsigned codepage);

}  // namespace eagls::detail
