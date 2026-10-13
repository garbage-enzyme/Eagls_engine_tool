#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace eagls::detail {

// 索引表的加密密钥：46 字节 ASCII，不含结尾 NUL。
// 同一字符串可以在游戏主程序 EAGLS.exe 中找到，EAGLS.dll 中没有。
inline constexpr std::string_view index_key = "1qaz2wsx3edc4rfv5tgb6yhn7ujm8ik,9ol.0p;/-@:^[]";

static_assert(index_key.size() == 46, "索引密钥长度应为 46 字节");

// 对索引槽位表原位异或：第 i 个字节异或 index_key[rand() % 46]，
// 其中 rand() 是以 seed 为种子的 MSVC 线性同余序列（见 MsvcRand），每个字节取一个新值。
//
// 这是对称的流密码式异或，解密与加密是同一个操作。
// 只处理槽位表本身；索引末尾的 4 字节种子是明文，调用方不要把它放进 data。
void apply_index_cipher(std::uint8_t* data, std::size_t size, std::uint32_t seed) noexcept;

}  // namespace eagls::detail
