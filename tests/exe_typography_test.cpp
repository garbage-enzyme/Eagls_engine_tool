// 中文排版回归：共用原版特征构造输入，独立解释补丁机器码并枚举所有 WORD。
// 不执行游戏代码；只允许 cmp eax,imm32 / je rel32 / jmp rel32，因此不会修改寄存器或栈。
#include <eagls/error.hpp>
#include <eagls/exe_patch.hpp>

#include "../src/codec/exe_typography_profile.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
namespace profile = eagls::detail::typography;
constexpr auto target = eagls::ExePatchTarget::gbk_zh_cn;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void put16(Bytes& bytes, std::size_t offset, std::uint16_t value) {
    bytes.at(offset) = static_cast<std::uint8_t>(value);
    bytes.at(offset + 1) = static_cast<std::uint8_t>(value >> 8);
}

void put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i));
}

std::uint32_t u32(const Bytes& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(bytes.at(offset + i)) << (8 * i);
    return value;
}

// 把原基础测试映像扩充为代码节和数据节，RVA 故意不同于磁盘偏移。
// 此映像是测试用构造物，只有少量已知原版指令窗口，不是可运行游戏样本。
Bytes fixture(const Bytes& base, std::size_t anchor) {
    Bytes bytes = base;
    bytes.resize(0x3400, 0xCC);
    put16(bytes, 0x86, 2);
    put32(bytes, 0x178 + 8, 0x2000);
    put32(bytes, 0x178 + 16, 0x2000);
    constexpr std::size_t section = 0x178 + 40;
    std::fill_n(bytes.data() + section, 40, std::uint8_t{0});
    const std::string name = ".data";
    std::copy(name.begin(), name.end(), bytes.data() + section);
    put32(bytes, section + 8, 0x1000);
    put32(bytes, section + 12, 0x5000);
    put32(bytes, section + 16, 0x1000);
    put32(bytes, section + 20, 0x2400);
    put32(bytes, section + 36, 0xC0000040);
    std::copy(profile::layout_original.begin(), profile::layout_original.end(), bytes.data() + anchor);
    std::copy(profile::spacing_original.begin(), profile::spacing_original.end(),
              bytes.data() + anchor - 0x9B1);
    for (std::size_t i = 0; i < 16; ++i) {
        const auto offset = 0x2450 + i * 32;
        if (i < 12) {
            std::copy(profile::gothic_original.begin(), profile::gothic_original.end(), bytes.data() + offset);
        } else {
            std::copy(profile::mincho_original.begin(), profile::mincho_original.end(), bytes.data() + offset);
        }
    }
    return bytes;
}

bool rejected(const Bytes& image) {
    try {
        (void)eagls::apply_exe_patch(image, target);
    } catch (const eagls::Error& error) {
        return error.code() == eagls::ErrorCode::invalid_content;
    }
    return false;
}

bool code_byte(std::size_t offset, std::size_t anchor) {
    return (offset >= anchor + 7 && offset < anchor + 0x53) ||
           (offset >= anchor + 0x7A && offset < anchor + 0x9B) ||
           (offset >= anchor + 0x161 && offset < anchor + 0x21D) ||
           (offset >= anchor - 0x9B1 && offset < anchor - 0x9AC);
}

std::size_t execute(const Bytes& image, std::size_t anchor, std::size_t pc,
                    std::uint16_t value, const std::vector<std::size_t>& exits) {
    bool equal = false;
    for (unsigned step = 0; step < 80; ++step) {
        if (std::find(exits.begin(), exits.end(), pc) != exits.end()) return pc;
        require(code_byte(pc, anchor), "分派跳入业务代码或越出比较块。");
        const auto opcode = image.at(pc);
        const std::size_t length = opcode == 0x0F ? 6 : 5;
        require(code_byte(pc + length - 1, anchor), "分派指令跨过比较块边界。");
        if (opcode == 0x3D) {
            equal = value == u32(image, pc + 1);
            pc += 5;
            continue;
        }
        require(opcode == 0xE9 || (opcode == 0x0F && image.at(pc + 1) == 0x84),
                "分派含不支持的指令或破坏寄存器/栈的指令。");
        const auto raw = u32(image, pc + length - 4);
        const auto displacement = raw <= 0x7FFFFFFFu ? static_cast<std::int64_t>(raw)
                                                    : static_cast<std::int64_t>(raw) - 0x100000000LL;
        const auto next = static_cast<std::int64_t>(pc + length) +
                          ((opcode == 0xE9 || equal) ? displacement : 0);
        require(next >= 0 && next < static_cast<std::int64_t>(image.size()), "分派跳转越界。");
        pc = static_cast<std::size_t>(next);
    }
    throw std::runtime_error("分派没有在有限步数内结束。");
}

void verify_dispatch(const Bytes& patched, std::size_t anchor) {
    // 期望值直接写成 GBK 字节对，不使用生产实现中的分类数组或生成器。
    const std::array<std::array<std::uint8_t, 2>, 16> pairs{{
        {{0xA3,0xAC}}, {{0xA1,0xA3}}, {{0xA1,0xA2}}, {{0xA3,0xBB}},
        {{0xA3,0xBA}}, {{0xA3,0xBF}}, {{0xA3,0xA1}}, {{0xA3,0xA9}},
        {{0xA1,0xBF}}, {{0xA1,0xB7}}, {{0xA1,0xB5}}, {{0xA1,0xB9}},
        {{0xA1,0xBB}}, {{0xA1,0xB1}}, {{0xA1,0xAF}}, {{0xA1,0xAD}}
    }};
    const auto yes = anchor + 0x21D;
    const auto no = anchor + 0x9B;
    const auto spacing = anchor - 0x9B1;
    const auto normal = anchor - 0x21A;
    const std::vector<std::size_t> kinsoku_exits{yes, no};
    const std::vector<std::size_t> spacing_exits{
        normal, spacing + 0x4C, spacing + 0x37, spacing + 0x22,
        spacing + 0x5E, spacing + 0x9A, spacing + 0x85};
    for (std::uint32_t value = 0; value <= 0xFFFF; ++value) {
        const auto low = static_cast<std::uint8_t>(value);
        const auto high = static_cast<std::uint8_t>(value >> 8);
        const bool forbidden = std::any_of(pairs.begin(), pairs.end(), [&](const auto& pair) {
            return pair[0] == low && pair[1] == high;
        });
        const auto actual = execute(patched, anchor, anchor + 7, static_cast<std::uint16_t>(value), kinsoku_exits);
        require(actual == (forbidden ? yes : no), "中文禁则分类与独立期望不符。");
        std::size_t expected = normal;
        if ((low == 0xA3 && high == 0xAC) || (low == 0xA1 && high == 0xA2)) expected = spacing + 0x4C;
        else if (low == 0xA1 && high == 0xA3) expected = spacing + 0x37;
        else if (low == 0xA1 && high == 0xA4) expected = spacing + 0x22;
        else if (low == 0xA3 && high == 0xBF) expected = spacing + 0x5E;
        else if (low == 0xA3 && high == 0xA1) expected = spacing + 0x9A;
        else if (low == 0xA1 && high == 0xAD) expected = spacing + 0x85;
        require(execute(patched, anchor, spacing, static_cast<std::uint16_t>(value), spacing_exits) == expected,
                "中文标点字距分派与独立期望不符。");
    }
}
}  // namespace

void verify_typography(const std::vector<std::uint8_t>& base) {
    // 两种磁盘位置都必须定位成功，不能把样本固定地址当作定位手段。
    for (const std::size_t anchor : {std::size_t{0x1800}, std::size_t{0x1900}}) {
        const auto original = fixture(base, anchor);
        const auto report = eagls::inspect_exe_patch(original, target);
        require(report.state == eagls::ExePatchState::original && report.sites.size() == 26,
                "中文排版应为 6 个基础字节、4 个代码块、16 个字体槽位。");
        const auto patched = eagls::apply_exe_patch(original, target);
        require(patched.size() == original.size(), "中文排版改变了文件大小。");
        require(eagls::inspect_exe_patch(patched, target).state == eagls::ExePatchState::patched,
                "中文排版成品未被识别。");
        const auto old = eagls::apply_exe_patch(original);
        require(eagls::inspect_exe_patch(old, target).state == eagls::ExePatchState::base_patched,
                "旧 GBK 补丁未被识别为可升级。");
        require(eagls::apply_exe_patch(old, target) == patched, "旧版升级与原版直出不一致。");
        require(rejected(patched), "重复应用中文排版未拒绝。");
        verify_dispatch(patched, anchor);

        std::vector<bool> permitted(original.size(), false);
        for (const auto& site : report.sites) {
            require(site.original_bytes.size() == site.patched_bytes.size(), "补丁块长度变化。");
            require(std::equal(site.patched_bytes.begin(), site.patched_bytes.end(),
                               patched.data() + site.file_offset), "补丁块未完整写入。");
            for (std::size_t i = 0; i < site.patched_bytes.size(); ++i) {
                if (site.kind == eagls::ExePatchSiteKind::typography_code) {
                    require(code_byte(site.file_offset + i, anchor), "报告包含不允许修改的业务代码。");
                }
                permitted.at(site.file_offset + i) = true;
            }
            const auto expected_va = site.kind == eagls::ExePatchSiteKind::font_face
                ? 0x400000 + 0x5000 + site.file_offset - 0x2400
                : 0x400000 + 0x1000 + site.file_offset - 0x400;
            if (site.kind == eagls::ExePatchSiteKind::typography_code ||
                site.kind == eagls::ExePatchSiteKind::font_face) {
                require(site.instruction_va == expected_va, "代码块或字体槽位的 VA 换算错误。");
            }
        }
        for (std::size_t i = 0; i < original.size(); ++i) {
            require(permitted[i] || original[i] == patched[i], "修改了预定范围外的字节。");
        }
        for (std::size_t i = 0; i < 16; ++i) {
            const auto offset = 0x2450 + i * 32;
            const std::string name = i < 12 ? "SimHei" : "SimSun";
            const std::size_t capacity = i < 12 ? 14 : 10;
            require(std::equal(name.begin(), name.end(), patched.data() + offset), "字体名替换错误。");
            require(std::all_of(patched.data() + offset + name.size(), patched.data() + offset + capacity,
                                [](std::uint8_t c) { return c == 0; }), "字体剩余槽位未补零。");
            require(patched[offset + capacity] == 0xCC, "字体替换越过原槽位。");
        }

        // 三类拒绝边界：缺失定位/槽位、块内损坏或周围业务代码变化、合法块的混合状态。
        for (const auto offset : {anchor, anchor + 0x54, anchor - 0x9B1 + 12, std::size_t{0x2450}}) {
            auto broken = original;
            broken[offset] ^= 1;
            require(rejected(broken), "损坏的锚点、窗口或字体槽位未被拒绝。");
        }
        auto broken = patched;
        broken[anchor + 0x163] ^= 1;
        require(rejected(broken), "块内部分修改未被拒绝。");
        auto mixed = old;
        std::copy_n(patched.data() + 0x2450, 14, mixed.data() + 0x2450);
        require(eagls::inspect_exe_patch(mixed, target).state == eagls::ExePatchState::mixed && rejected(mixed),
                "未知混合版本被当作旧补丁升级。");
        auto duplicate = original;
        std::copy_n(original.data() + anchor, 7, duplicate.data() + 0x2100);
        require(rejected(duplicate), "重复排版锚点未被拒绝。");
    }
}
