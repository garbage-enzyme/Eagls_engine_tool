#pragma once

#include <cstdint>
#include <vector>

namespace eagls::detail {

// DAT 正文区的"每隔一字节异或"掩码，解密与加密是同一个对合操作。
// 从偏移 3600 起，用按 int8 符号扩展后的种子初始化 MSVC rand；
// 跳过的字节不消耗随机数；末尾种子字节本身不参与。
// 调用方必须保证 data.size() >= 3602（标签表 + 至少 2 字节）。
void xor_dat_body(std::vector<std::uint8_t>& data, std::uint8_t seed);

}  // namespace eagls::detail
