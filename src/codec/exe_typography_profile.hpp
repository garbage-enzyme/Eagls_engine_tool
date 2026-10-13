#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <eagls/exe_patch.hpp>
#include "pe_image.hpp"

namespace eagls::detail::typography {

// 样本的指令窗口。VA 仅用于解释布局，不作为文件定位地址。
// 原样核验未修改的错误处理、换行和字距计算代码，拒绝在不匹配的版本上重写分支。
// 合成测试共用原版窗口作输入，补丁后的分类期望与机器码解释器独立实现。
template <std::size_t N>
constexpr auto signature(const char (&hex)[N]) {
    static_assert(N % 2 == 1);
    std::array<std::uint8_t, (N - 1) / 2> result{};
    const auto digit = [](char c) constexpr -> unsigned {
        return c >= '0' && c <= '9' ? static_cast<unsigned>(c - '0')
                                  : static_cast<unsigned>(c - 'a' + 10);
    };
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<std::uint8_t>((digit(hex[i * 2]) << 4) | digit(hex[i * 2 + 1]));
    }
    return result;
}

// 0x40C499..0x40C6D0：前 7 字节为不变锚点 mov eax,[esp+20h] / movzx eax,word ptr [eax]。
inline constexpr auto layout_original = signature(
    "8b4424200fb7003d817600000f8f960100000f8405020000"
    "3d834600000f8f3e0100000f84f40100003d834200007f4a"
    "0f84e70100003d834000000f84dc0100003d814100000f84"
    "d10100003d81420000eb4268e06a4700e80acdffff8b8424"
    "9482050050e8fdccffff83c4088bc55f5e5d5b81c4608205"
    "00c33d814300000f84980100003d834400000f848d010000"
    "3d814500000f84820100008b9424748205008b8494702200"
    "008b54244469c0380100008bb89034e0008b808c34e0008b"
    "8c247882050003fa8b54243003d703c2895424308b942484"
    "82050033f63bc289742434894c243c0f8e460100008b5424"
    "208d42fe668b153c6b47006689108b949c6c4202008bb49c"
    "6c2200005269f6380100008b542434508b849c7462010050"
    "525181ec380100008d8424d40100008bfc81c66834e000b9"
    "4e00000050f3a5e893efffff8bbc2474010000b905000000"
    "8bf081c450010000f3a58bebe9d60000003d815800007f24"
    "0f84af0000002d814800000f84a400000083e8020f849b00"
    "00002dfe000000e909ffffff3d815b00000f84860000003d"
    "83620000747f3d816a0000e9edfeffff3d82a300007f3b74"
    "6c3d838500007f1a74633d81780000745c3d817a00007455"
    "3d83830000e9c3feffff3d8387000074443d829f0000743d"
    "3d82a10000e9abfeffff3d82e100007f1a742a3d82a50000"
    "74233d82a70000741c3d82c10000e98afeffff3d82e30000"
    "740b3d82e500000f857efeffff8b44243485c00f8572feff"
    "ffc744243401000000eb0489742444");

// 0x40BAE8..0x40BB94：仅首条 cmp 改成跳转，所有原字距计算分支保持原样。
inline constexpr auto spacing_original = signature(
    "3d814800007f6c74553d81410000743c3d8142000074203d"
    "814500000f85750700008b0dc86672010faf8c24c0820500"
    "2bf9e9690700008b15c46672010faf9424c08205002bfae9"
    "54070000a1c06672010faf8424c0820500e9400700008b0d"
    "d06672010faf8c24c08205002bf9e92d0700003d81490000"
    "74203d816300000f85120700008b15cc6672010faf9424c0"
    "8205002bfae906070000a1d46672010faf8424c0820500e9"
    "f2060000");
static_assert(layout_original.size() == 567 && spacing_original.size() == 172);

// 下列位置均相对不变锚点 0x40C499；两段间隙含业务代码，不能覆盖。
inline constexpr std::size_t anchor_size = 7;
inline constexpr std::size_t entry = 0x40C4A0 - 0x40C499;
inline constexpr std::size_t spacing_first = entry + 5;
inline constexpr std::size_t spacing_second = 0x40C513 - 0x40C499;
inline constexpr std::size_t kinsoku = 0x40C5FA - 0x40C499;
inline constexpr std::size_t allow = 0x40C6B6 - 0x40C499;
inline constexpr std::size_t wrap = 0x40C534 - 0x40C499;
inline constexpr std::size_t spacing_distance = 0x40C499 - 0x40BAE8;

inline constexpr auto gothic_original = signature("826c827220835383568362834e00");
inline constexpr auto mincho_original = signature("826c82722096be92a900");

void append_sites(const std::vector<std::uint8_t>& bytes, const PeImage& image,
                  ExePatchReport& report);

}  // namespace eagls::detail::typography
