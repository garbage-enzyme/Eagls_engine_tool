// 中文排版仅重用已识别的比较块，不搬移代码、不扩充 PE、不修改原文件。
#include "codec/exe_typography_profile.hpp"

#include <eagls/error.hpp>

#include "support/bytes.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace eagls::detail::typography {
namespace {
using Bytes = std::vector<std::uint8_t>;

struct Block {
    std::size_t offset;
    Bytes replacement;
};

// 仅生成 cmp eax,imm32 / je rel32 / jmp rel32；不改通用寄存器和栈。
// 所有地址均相对锚点且位于同一代码节，故不依赖 ImageBase、RVA 或磁盘起点。
void append_u32(Bytes& code, std::uint32_t value) {
    const auto offset = code.size();
    code.resize(offset + 4);
    write_u32_le(code.data() + offset, value);
}

void jump(Bytes& code, std::int64_t start, std::int64_t target, bool equal = false) {
    if (equal) code.push_back(0x0F);
    code.push_back(equal ? 0x84 : 0xE9);
    const auto next = start + static_cast<std::int64_t>(code.size()) + 4;
    append_u32(code, static_cast<std::uint32_t>(target - next));
}

void compare(Bytes& code, std::size_t start, std::uint16_t value, std::int64_t target) {
    code.push_back(0x3D);
    append_u32(code, value);
    jump(code, static_cast<std::int64_t>(start), target, true);
}

void pad(Bytes& code, std::size_t capacity) {
    if (code.size() > capacity) {
        throw Error(ErrorCode::invalid_content, "中文排版指令超过原比较块容量。");
    }
    code.resize(capacity, 0x90);
}

std::vector<Block> layout_blocks() {
    // 行首禁则：，。、；：？！）】》〉」』”’…；按输入字节的小端 WORD 比较。
    constexpr std::uint16_t forbidden[] = {
        0xACA3, 0xA3A1, 0xA2A1, 0xBBA3, 0xBAA3, 0xBFA3, 0xA1A3, 0xA9A3,
        0xBFA1, 0xB7A1, 0xB5A1, 0xB9A1, 0xBBA1, 0xB1A1, 0xAFA1, 0xADA1};
    Bytes first;
    jump(first, entry, kinsoku);

    // 前 6 项恰好占据入口跳转后的 71 字节（6*11+5），第 7 项放第二块。
    // 逗号与顿号共用原顿号参数，其余保持原类别的紧缩系数，不改变字号/字重。
    struct SpacingCase {
        std::uint16_t value;
        std::int64_t target;
    };
    constexpr SpacingCase spacing[] = {
        {0xACA3, 0x40BB34 - 0x40C499},  // ，
        {0xA2A1, 0x40BB34 - 0x40C499},  // 、
        {0xA3A1, 0x40BB1F - 0x40C499},  // 。
        {0xA4A1, 0x40BB0A - 0x40C499},  // ·
        {0xBFA3, 0x40BB46 - 0x40C499},  // ？
        {0xA1A3, 0x40BB82 - 0x40C499},  // ！
        {0xADA1, 0x40BB6D - 0x40C499},  // …
    };
    for (std::size_t i = 0; i < 6; ++i) {
        compare(first, entry, spacing[i].value, spacing[i].target);
    }
    jump(first, entry, spacing_second);
    pad(first, 76);

    Bytes second;
    compare(second, spacing_second, spacing[6].value, spacing[6].target);
    jump(second, spacing_second, 0x40C27F - 0x40C499);  // 原普通字符字距路径
    pad(second, 33);

    Bytes last;
    for (const auto value : forbidden) compare(last, kinsoku, value, allow);
    jump(last, kinsoku, wrap);
    pad(last, 188);
    return {{entry, std::move(first)}, {spacing_second, std::move(second)},
            {kinsoku, std::move(last)}};
}

ExePatchSite site_at(const Bytes& bytes, const PeImage& image, const PeSection& section,
                     std::size_t offset, Bytes original, Bytes replacement,
                     ExePatchSiteKind kind) {
    ExePatchSite site;
    site.kind = kind;
    site.file_offset = static_cast<std::uint32_t>(offset);
    site.instruction_va = image.file_offset_to_va(section, offset);
    site.current_bytes.assign(bytes.data() + offset, bytes.data() + offset + original.size());
    site.original_bytes = std::move(original);
    site.patched_bytes = std::move(replacement);
    return site;
}

// 可修改块必须完整匹配原版或本工具产物；间隙及出口逐字节保持原版。
// 不接受“每个字节各自像原版或补丁”的拼接，因为那可能组成错误的指令。
template <std::size_t N>
void inspect_window(const Bytes& bytes, const PeImage& image, const PeSection& section,
                    std::size_t offset, const std::array<std::uint8_t, N>& original,
                    const std::vector<Block>& blocks, ExePatchReport& report) {
    Bytes normalized(bytes.data() + offset, bytes.data() + offset + N);
    for (const auto& block : blocks) {
        const auto end = block.offset + block.replacement.size();
        auto site = site_at(bytes, image, section, offset + block.offset,
                            Bytes(original.data() + block.offset, original.data() + end),
                            block.replacement, ExePatchSiteKind::typography_code);
        if (site.current_bytes != site.original_bytes && site.current_bytes != site.patched_bytes) {
            throw Error(ErrorCode::invalid_content, "中文排版指令块与原版或已知补丁不匹配。");
        }
        std::copy(site.original_bytes.begin(), site.original_bytes.end(),
                  normalized.data() + block.offset);
        report.sites.push_back(std::move(site));
    }
    if (!std::equal(normalized.begin(), normalized.end(), original.begin())) {
        throw Error(ErrorCode::invalid_content, "中文排版周围的换行、错误处理或字距计算代码不匹配。");
    }
}

template <std::size_t N>
void append_font(const Bytes& bytes, const PeImage& image, const std::array<std::uint8_t, N>& original,
                 const char* face, std::size_t expected, ExePatchReport& report) {
    Bytes replacement(N, 0);
    const std::string name(face);
    if (name.size() >= N) throw Error(ErrorCode::invalid_content, "字体名超过原字符串槽位。");
    std::copy(name.begin(), name.end(), replacement.begin());
    std::size_t count = 0;
    for (const auto& section : image.sections) {
        // 不扫描资源和代码；样本字体位于 .data，同时允许正常的只读数据节布局。
        if (section.executable() || (section.name != ".data" && section.name != ".rdata")) continue;
        const auto size = section.data_size();
        for (std::size_t i = 0; i + N <= size; ++i) {
            const auto offset = section.raw_offset + i;
            const auto* current = bytes.data() + offset;
            if (!std::equal(original.begin(), original.end(), current) &&
                !std::equal(replacement.begin(), replacement.end(), current)) continue;
            report.sites.push_back(site_at(bytes, image, section, offset,
                                           Bytes(original.begin(), original.end()), replacement,
                                           ExePatchSiteKind::font_face));
            ++count;
        }
    }
    if (count != expected) {
        throw Error(ErrorCode::invalid_content,
                    "字体 " + name + " 的槽位数量不符（期望 " + std::to_string(expected) +
                        "，实际 " + std::to_string(count) + "）。");
    }
}
}  // namespace

void append_sites(const Bytes& bytes, const PeImage& image, ExePatchReport& report) {
    const PeSection* found_section = nullptr;
    std::size_t found_offset = 0;
    for (const auto& section : image.sections) {
        if (!section.executable()) continue;
        const auto size = section.data_size();
        for (std::size_t i = 0; i + anchor_size <= size; ++i) {
            if (!std::equal(layout_original.begin(), layout_original.begin() + anchor_size,
                            bytes.data() + section.raw_offset + i)) continue;
            if (found_section != nullptr) {
                throw Error(ErrorCode::invalid_content, "中文排版锚点出现多次，无法唯一定位。");
            }
            // 两个完整窗口必须落在同一代码节的有效磁盘数据内。
            if (i < spacing_distance || layout_original.size() > size - i) {
                throw Error(ErrorCode::invalid_content, "中文排版指令窗口超出代码节。");
            }
            found_section = &section;
            found_offset = section.raw_offset + i;
        }
    }
    if (found_section == nullptr) {
        throw Error(ErrorCode::invalid_content, "未找到中文排版锚点，仅支持已识别的游戏版本。");
    }

    inspect_window(bytes, image, *found_section, found_offset, layout_original, layout_blocks(), report);
    Bytes redirect;
    jump(redirect, -static_cast<std::int64_t>(spacing_distance), spacing_first);
    inspect_window(bytes, image, *found_section, found_offset - spacing_distance,
                   spacing_original, {{0, std::move(redirect)}}, report);
    append_font(bytes, image, gothic_original, "SimHei", 12, report);
    append_font(bytes, image, mincho_original, "SimSun", 4, report);
}

}  // namespace eagls::detail::typography
