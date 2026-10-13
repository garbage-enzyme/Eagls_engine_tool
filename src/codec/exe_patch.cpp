// EAGLS.exe 中文显示补丁的定位与应用。
//
// 不依赖固定偏移：在可执行节中按特征码定位两类硬编码。
// 定位结果先与 IA-32 指令语义逐字节核对，再用于修改；命中数不符时明确失败，
// 绝不在未识别的文件上落字节。
#include <eagls/exe_patch.hpp>

#include <eagls/error.hpp>

#include "codec/pe_image.hpp"
#include "codec/exe_typography_profile.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace eagls {
namespace {

using detail::PeImage;
using detail::PeSection;

// 字体字符集写入：mov byte ptr [esp+disp], imm = C6 44 24 disp imm。
// disp 字节通配（由 kCharsetCells 过滤）；imm 按原版/补丁两种取值分别生成完整特征后合并，
// 因此同一函数在原版与已打补丁的文件里都能被定位。
constexpr std::uint8_t kCharsetPattern[] = {0xC6, 0x44, 0x24, 0x00, 0x80};
constexpr bool kCharsetWildcard[] = {false, false, false, true, false};
constexpr std::uint8_t kCharsetOriginal = 0x80;
constexpr std::array<std::uint8_t, 5> kCharsetCells{0x3F, 0x2F, 0x3B, 0x33, 0x5B};
constexpr std::size_t kCharsetImmediateOffset = 4;  // 立即数字节在命中点内的偏移

// 前导字节判定函数（sub_401900）的整体特征，29 字节：
//   mov al,[esp+4] / cmp al,81h / jb +4 / cmp al,0A0h / jbe +8 /
//   cmp al,0E0h / jb +10 / cmp al,0FCh / ja +6 / mov eax,1 / ret / xor eax,eax / ret
// 全游戏唯一的双字节判定，被脚本解析、排版与出字共用，因此整体特征必须唯一。
// +9 处的立即数（cmp al,0A0h）按原版/补丁两种取值分别生成完整特征后合并。
constexpr std::uint8_t kLeadPattern[] = {
    0x8A, 0x44, 0x24, 0x04, 0x3C, 0x81, 0x72, 0x04, 0x3C, 0xA0, 0x76, 0x08, 0x3C, 0xE0, 0x72,
    0x0A, 0x3C, 0xFC, 0x77, 0x06, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0x33, 0xC0, 0xC3};
constexpr std::size_t kLeadCmpOffset = 9;   // cmp al, 0A0h 的立即数在命中点内的偏移
constexpr std::size_t kLeadCmpStart = 8;    // cmp al, 0A0h 指令起点（3C 操作码）在命中点内的偏移
constexpr std::uint8_t kLeadOriginal = 0xA0;

struct TargetValues {
    std::uint8_t charset;
    std::uint8_t lead_upper;
};

TargetValues values_of(ExePatchTarget target) {
    switch (target) {
    case ExePatchTarget::gbk:
    case ExePatchTarget::gbk_zh_cn:
        return {0x86, 0xFE};  // GB2312_CHARSET + GBK 前导字节上界
    }
    throw Error(ErrorCode::invalid_content, "未知的补丁目标。");
}

struct CodeHit {
    const PeSection* section = nullptr;
    std::size_t offset = 0;  // 命中点相对该节磁盘数据起点的偏移
};

// 在可执行节的磁盘数据区内查找特征；wildcard 为 nullptr 表示全部字节固定。
std::vector<CodeHit> find_pattern(const std::vector<std::uint8_t>& bytes, const PeImage& image,
                                  const std::uint8_t* pattern, const bool* wildcard,
                                  std::size_t length) {
    std::vector<CodeHit> hits;
    for (const auto& section : image.sections) {
        if (!section.executable()) continue;
        const std::size_t size = section.data_size();
        if (size < length) continue;
        const std::uint8_t* base = bytes.data() + section.raw_offset;
        for (std::size_t i = 0; i + length <= size; ++i) {
            bool match = true;
            for (std::size_t j = 0; j < length; ++j) {
                if (wildcard != nullptr && wildcard[j]) continue;
                if (base[i + j] != pattern[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                hits.push_back({&section, i});
            }
        }
    }
    return hits;
}

ExePatchSite make_site(const PeImage& image, const CodeHit& hit, std::size_t byte_in_hit,
                       std::size_t instruction_in_hit, ExePatchSiteKind kind,
                       std::uint8_t original, std::uint8_t patched,
                       const std::vector<std::uint8_t>& bytes) {
    const std::size_t offset = hit.offset + byte_in_hit;
    const std::size_t file_offset = hit.section->raw_offset + offset;
    ExePatchSite site;
    site.kind = kind;
    site.instruction_va = image.file_offset_to_va(
        *hit.section, hit.section->raw_offset + hit.offset + instruction_in_hit);
    site.file_offset = static_cast<std::uint32_t>(file_offset);
    site.original_bytes = {original};
    site.patched_bytes = {patched};
    site.current_bytes = {bytes[file_offset]};
    return site;
}

ExePatchReport inspect_impl(const std::vector<std::uint8_t>& bytes, ExePatchTarget target) {
    const TargetValues values = values_of(target);
    const PeImage image = detail::parse_pe32(bytes);

    ExePatchReport report;

    // 前导字节判定必须全局唯一；立即数按原版/补丁两种取值分别定位后再合并。
    std::vector<CodeHit> lead_hits;
    for (const std::uint8_t immediate : {kLeadOriginal, values.lead_upper}) {
        std::uint8_t pattern[sizeof(kLeadPattern)];
        for (std::size_t i = 0; i < sizeof(kLeadPattern); ++i) pattern[i] = kLeadPattern[i];
        pattern[kLeadCmpOffset] = immediate;
        const auto hits = find_pattern(bytes, image, pattern, nullptr, sizeof(kLeadPattern));
        lead_hits.insert(lead_hits.end(), hits.begin(), hits.end());
    }
    if (lead_hits.empty()) {
        throw Error(ErrorCode::invalid_content,
                    "未找到前导字节判定函数特征，目标可能不是本游戏版本或已被修改。");
    }
    if (lead_hits.size() > 1) {
        throw Error(ErrorCode::invalid_content, "前导字节判定函数特征出现多次，无法安全定位。");
    }
    report.sites.push_back(make_site(image, lead_hits.front(), kLeadCmpOffset, kLeadCmpStart,
                                     ExePatchSiteKind::lead_byte_range, kLeadOriginal,
                                     values.lead_upper, bytes));

    // 字体字符集写入：立即数按原版/补丁两版定位；disp 必须恰好覆盖已知的 5 个栈偏移。
    std::vector<CodeHit> charset_hits;
    for (const std::uint8_t immediate : {kCharsetOriginal, values.charset}) {
        std::uint8_t pattern[sizeof(kCharsetPattern)];
        for (std::size_t i = 0; i < sizeof(kCharsetPattern); ++i) pattern[i] = kCharsetPattern[i];
        pattern[kCharsetImmediateOffset] = immediate;
        const auto hits =
            find_pattern(bytes, image, pattern, kCharsetWildcard, sizeof(kCharsetPattern));
        charset_hits.insert(charset_hits.end(), hits.begin(), hits.end());
    }
    std::array<bool, kCharsetCells.size()> used{};
    std::size_t charset_count = 0;
    for (const auto& hit : charset_hits) {
        const std::uint8_t cell = bytes[hit.section->raw_offset + hit.offset + 3];
        std::size_t index = kCharsetCells.size();
        for (std::size_t i = 0; i < kCharsetCells.size(); ++i) {
            if (kCharsetCells[i] == cell) {
                index = i;
                break;
            }
        }
        // 集合外的 disp 或重复命中不属于本补丁，跳过；数量不足时在下面统一报错。
        if (index == kCharsetCells.size() || used[index]) continue;
        used[index] = true;
        ++charset_count;
        report.sites.push_back(make_site(image, hit, kCharsetImmediateOffset, 0,
                                         ExePatchSiteKind::font_charset, kCharsetOriginal,
                                         values.charset, bytes));
    }
    if (charset_count != kCharsetCells.size()) {
        throw Error(ErrorCode::invalid_content,
                    "字体字符集写入位置数量与预期不符（期望 5 处，实际 " +
                        std::to_string(charset_count) + " 处）。");
    }

    const std::size_t base_count = report.sites.size();
    const bool base_patched = std::all_of(report.sites.begin(), report.sites.end(),
        [](const ExePatchSite& site) { return site.current_bytes == site.patched_bytes; });
    if (target == ExePatchTarget::gbk_zh_cn) {
        detail::typography::append_sites(bytes, image, report);
    }
    std::sort(report.sites.begin(), report.sites.end(),
              [](const ExePatchSite& a, const ExePatchSite& b) {
                  return a.file_offset < b.file_offset;
              });

    std::size_t original_count = 0;
    std::size_t patched_count = 0;
    std::size_t previous_end = 0;
    for (const auto& site : report.sites) {
        if (site.file_offset < previous_end) {
            throw Error(ErrorCode::invalid_content, "补丁字节块重叠，无法应用。");
        }
        previous_end = site.file_offset + site.current_bytes.size();
        if (site.current_bytes == site.original_bytes) {
            ++original_count;
        } else if (site.current_bytes == site.patched_bytes) {
            ++patched_count;
        }
    }
    if (original_count == report.sites.size()) {
        report.state = ExePatchState::original;
    } else if (patched_count == report.sites.size()) {
        report.state = ExePatchState::patched;
    } else if (target == ExePatchTarget::gbk_zh_cn && base_patched &&
               patched_count == base_count && original_count + patched_count == report.sites.size()) {
        report.state = ExePatchState::base_patched;
    } else {
        report.state = ExePatchState::mixed;
    }
    return report;
}

}  // namespace

ExePatchReport inspect_exe_patch(const std::vector<std::uint8_t>& image, ExePatchTarget target) {
    return inspect_impl(image, target);
}

std::vector<std::uint8_t> apply_exe_patch(const std::vector<std::uint8_t>& image,
                                          ExePatchTarget target) {
    const ExePatchReport report = inspect_impl(image, target);
    if (report.state == ExePatchState::patched) {
        throw Error(ErrorCode::invalid_content, "输入文件已经是打过补丁的版本。");
    }
    if (report.state != ExePatchState::original && report.state != ExePatchState::base_patched) {
        throw Error(ErrorCode::invalid_content,
                    "输入文件的补丁位置状态不一致，拒绝在未识别的文件上继续修改。");
    }
    std::vector<std::uint8_t> patched = image;
    for (const auto& site : report.sites) {
        std::copy(site.patched_bytes.begin(), site.patched_bytes.end(),
                  patched.data() + site.file_offset);
    }
    // 复核：重新定位并逐字节确认，任何不一致都不允许写盘。
    if (inspect_impl(patched, target).state != ExePatchState::patched) {
        throw Error(ErrorCode::invalid_content, "补丁复核失败，输出未写出。");
    }
    return patched;
}

}  // namespace eagls
