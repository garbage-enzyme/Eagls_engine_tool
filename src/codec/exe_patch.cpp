// EAGLS.exe / SAIMIN.exe 中文显示补丁的定位与应用。
//
// 不依赖固定偏移：在可执行节中按特征码定位两类硬编码，并自动探测程序版本。
// 定位结果先与 IA-32 指令语义逐字节核对，再用于修改；命中数不符时明确失败，
// 绝不在未识别的文件上落字节。
#include <eagls/exe_patch.hpp>

#include <eagls/error.hpp>

#include "codec/pe_image.hpp"
#include "codec/exe_typography_profile.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace eagls {
namespace {

using detail::PeImage;
using detail::PeSection;

// —— 两类硬编码的目标值（两版通用）——
// 前导字节判定：把第一段上界 0A0h 改为 0FEh，等价于 GBK 规则 81..FE 断字。
// 字体字符集：把 SHIFTJIS_CHARSET(80h) 改为 GB2312_CHARSET(86h)，双字节按 GBK 取字形。
constexpr std::uint8_t kLeadOriginal = 0xA0;
constexpr std::uint8_t kLeadPatched = 0xFE;
constexpr std::uint8_t kCharsetOriginal = 0x80;
constexpr std::uint8_t kCharsetPatched = 0x86;

// EAGLS 版判定函数（29 字节）：
//   mov al,[esp+4] / cmp al,81h / jb +4 / cmp al,0A0h / jbe +8 /
//   cmp al,0E0h / jb +10 / cmp al,0FCh / ja +6 / mov eax,1 / ret / xor eax,eax / ret
// 全游戏唯一的双字节判定，被脚本解析、排版与出字共用，因此整体特征必须唯一。
constexpr std::uint8_t kEaglsLead[] = {
    0x8A, 0x44, 0x24, 0x04, 0x3C, 0x81, 0x72, 0x04, 0x3C, 0xA0, 0x76, 0x08, 0x3C, 0xE0, 0x72,
    0x0A, 0x3C, 0xFC, 0x77, 0x06, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0x33, 0xC0, 0xC3};

// SAIMIN 版判定函数（27 字节，第二段用 add 折算，比 EAGLS 少 2 字节）：
//   mov al,[esp+4] / cmp al,81h / jb +4 / cmp al,0A0h / jbe +6 /
//   add al,20h / cmp al,1Ch / ja +6 / mov eax,1 / ret / xor eax,eax / ret
// 与 EAGLS 版语义相同、编译形态不同（首处差异在函数 +0x0B），两版特征互不命中。
constexpr std::uint8_t kSaiminLead[] = {
    0x8A, 0x44, 0x24, 0x04, 0x3C, 0x81, 0x72, 0x04, 0x3C, 0xA0, 0x76, 0x06, 0x04, 0x20, 0x3C,
    0x1C, 0x77, 0x06, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0x33, 0xC0, 0xC3};

// cmp al, 0A0h 的立即数与指令起点在命中点内的偏移（两版相同，均在函数 +9 / +8）。
constexpr std::size_t kLeadCmpOffset = 9;
constexpr std::size_t kLeadCmpStart = 8;

// 字体字符集写入的两种编译形态：
//   短位移 mov byte ptr [esp+disp8],  imm = C6 44 24 disp8 imm   （EAGLS 5 处；SAIMIN 1 处）
//   长位移 mov byte ptr [esp+disp32], imm = C6 84 24 disp32 imm  （SAIMIN 3 处）
// disp 由各 profile 的 cells 集合过滤（通配匹配后按小端读取核对，长位移必须整体读 32 位）；
// imm 按原版/补丁两种取值分别生成完整特征后合并，因此原版与已打补丁的文件都能被定位。
struct CharsetForm {
    enum class Kind {
        short_disp,  // C6 44 24 disp8 imm
        long_disp,   // C6 84 24 disp32 imm
    };
    Kind kind;
    const std::uint32_t* cells;  // 允许的栈偏移集合，每个必须恰好命中一次
    std::size_t cell_count;
    std::size_t immediate_offset;  // 立即数在命中点内的偏移（短 4、长 7）
};

struct PatchProfile {
    ExePatchProfile id;
    const std::uint8_t* lead_pattern;
    std::size_t lead_size;
    const CharsetForm* charset_forms;
    std::size_t charset_form_count;
    bool supports_typography;
};

constexpr std::uint32_t kEaglsCharsetCells[] = {0x3F, 0x2F, 0x3B, 0x33, 0x5B};
constexpr std::uint32_t kSaiminShortCells[] = {0x33};
constexpr std::uint32_t kSaiminLongCells[] = {0xE7, 0x15B, 0x203};

constexpr CharsetForm kEaglsForms[] = {
    {CharsetForm::Kind::short_disp, kEaglsCharsetCells, 5, 4},
};
constexpr CharsetForm kSaiminForms[] = {
    {CharsetForm::Kind::short_disp, kSaiminShortCells, 1, 4},
    {CharsetForm::Kind::long_disp, kSaiminLongCells, 3, 7},
};

constexpr PatchProfile kProfiles[] = {
    {ExePatchProfile::eagls, kEaglsLead, sizeof(kEaglsLead), kEaglsForms, 1, true},
    {ExePatchProfile::saimin, kSaiminLead, sizeof(kSaiminLead), kSaiminForms, 2, false},
};

std::string hex32(std::uint32_t value) {
    char buffer[11];
    std::snprintf(buffer, sizeof buffer, "0x%X", static_cast<unsigned>(value));
    return buffer;
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

// 单个 profile 的判定函数定位：立即数按原版/补丁两种取值分别生成完整特征后合并。
// 返回 false 表示该版本特征不存在；命中多于一次时直接失败（特征必须唯一才能安全定位）。
bool find_profile_lead(const std::vector<std::uint8_t>& bytes, const PeImage& image,
                       const PatchProfile& profile, CodeHit& out) {
    std::vector<CodeHit> hits;
    for (const std::uint8_t immediate : {kLeadOriginal, kLeadPatched}) {
        std::vector<std::uint8_t> pattern(profile.lead_pattern,
                                          profile.lead_pattern + profile.lead_size);
        pattern[kLeadCmpOffset] = immediate;
        const auto found = find_pattern(bytes, image, pattern.data(), nullptr, pattern.size());
        hits.insert(hits.end(), found.begin(), found.end());
    }
    if (hits.empty()) return false;
    if (hits.size() > 1) {
        throw Error(ErrorCode::invalid_content,
                    std::string(exe_patch_profile_name(profile.id)) +
                        " 版前导字节判定函数特征出现多次，无法安全定位。");
    }
    out = hits.front();
    return true;
}

struct ProbeResult {
    const PatchProfile* profile = nullptr;
    CodeHit lead_hit;
};

// 自动探测程序版本：每个版本各自唯一命中；零命中或同时匹配多个版本都拒绝。
ProbeResult probe_profile(const std::vector<std::uint8_t>& bytes, const PeImage& image) {
    std::vector<ProbeResult> matched;
    for (const PatchProfile& profile : kProfiles) {
        ProbeResult result;
        result.profile = &profile;
        if (find_profile_lead(bytes, image, profile, result.lead_hit)) {
            matched.push_back(result);
        }
    }
    if (matched.empty()) {
        throw Error(ErrorCode::invalid_content,
                    "未找到前导字节判定函数特征，目标可能不是已知的游戏版本或已被修改。");
    }
    if (matched.size() > 1) {
        std::string message = "同时匹配多个已知版本的特征（";
        for (std::size_t i = 0; i < matched.size(); ++i) {
            if (i != 0) message += "、";
            message += exe_patch_profile_name(matched[i].profile->id);
        }
        message += "），无法确定目标版本。";
        throw Error(ErrorCode::invalid_content, message);
    }
    return matched.front();
}

// 生成字符集写入特征：disp 通配，其余字节固定；返回特征长度（短 5、长 8）。
std::size_t build_charset_pattern(CharsetForm::Kind kind, std::uint8_t immediate,
                                  std::uint8_t (&pattern)[8], bool (&wildcard)[8]) {
    if (kind == CharsetForm::Kind::short_disp) {
        pattern[0] = 0xC6;
        pattern[1] = 0x44;
        pattern[2] = 0x24;
        pattern[4] = immediate;
        wildcard[3] = true;
        return 5;
    }
    pattern[0] = 0xC6;
    pattern[1] = 0x84;
    pattern[2] = 0x24;
    pattern[7] = immediate;
    wildcard[3] = true;
    wildcard[4] = true;
    wildcard[5] = true;
    wildcard[6] = true;
    return 8;
}

// 读取命中点内的栈偏移：短位移 1 字节；长位移必须整体读取 32 位小端，
// 只读低字节会把 0x15B 这类偏移误判成 0x5B。
std::uint32_t read_charset_cell(const std::uint8_t* hit, CharsetForm::Kind kind) {
    if (kind == CharsetForm::Kind::short_disp) return hit[3];
    return static_cast<std::uint32_t>(hit[3]) |
           (static_cast<std::uint32_t>(hit[4]) << 8) |
           (static_cast<std::uint32_t>(hit[5]) << 16) |
           (static_cast<std::uint32_t>(hit[6]) << 24);
}

// 定位一个 profile 的全部字符集写入：cells 中每个栈偏移必须恰好命中一次；
// 集合外命中的同形指令不属于本补丁，忽略；同一栈偏移重复命中直接失败。
void append_charset_sites(const std::vector<std::uint8_t>& bytes, const PeImage& image,
                          const PatchProfile& profile, ExePatchReport& report) {
    for (std::size_t form_index = 0; form_index < profile.charset_form_count; ++form_index) {
        const CharsetForm& form = profile.charset_forms[form_index];
        std::vector<CodeHit> hits;
        for (const std::uint8_t immediate : {kCharsetOriginal, kCharsetPatched}) {
            std::uint8_t pattern[8] = {};
            bool wildcard[8] = {};
            const std::size_t length =
                build_charset_pattern(form.kind, immediate, pattern, wildcard);
            const auto found = find_pattern(bytes, image, pattern, wildcard, length);
            hits.insert(hits.end(), found.begin(), found.end());
        }
        std::vector<bool> used(form.cell_count, false);
        std::size_t count = 0;
        for (const auto& hit : hits) {
            const std::uint8_t* base = bytes.data() + hit.section->raw_offset + hit.offset;
            const std::uint32_t cell = read_charset_cell(base, form.kind);
            std::size_t index = form.cell_count;
            for (std::size_t i = 0; i < form.cell_count; ++i) {
                if (form.cells[i] == cell) {
                    index = i;
                    break;
                }
            }
            if (index == form.cell_count) continue;  // 集合外：不是本补丁的写入
            if (used[index]) {
                throw Error(ErrorCode::invalid_content,
                            std::string(exe_patch_profile_name(profile.id)) +
                                " 版字体字符集写入位置重复（栈偏移 " + hex32(cell) + " 出现多次）。");
            }
            used[index] = true;
            ++count;
            report.sites.push_back(make_site(image, hit, form.immediate_offset, 0,
                                             ExePatchSiteKind::font_charset, kCharsetOriginal,
                                             kCharsetPatched, bytes));
        }
        if (count != form.cell_count) {
            throw Error(ErrorCode::invalid_content,
                        std::string(exe_patch_profile_name(profile.id)) +
                            " 版字体字符集写入位置数量与预期不符（期望 " +
                            std::to_string(form.cell_count) + " 处，实际 " + std::to_string(count) +
                            " 处）。");
        }
    }
}

ExePatchReport inspect_impl(const std::vector<std::uint8_t>& bytes, ExePatchTarget target) {
    const PeImage image = detail::parse_pe32(bytes);

    // 版本自动探测：判定函数特征必须唯一命中；两个版本同时命中或都不命中都拒绝。
    const ProbeResult probe = probe_profile(bytes, image);
    const PatchProfile& profile = *probe.profile;

    ExePatchReport report;
    report.profile = profile.id;
    report.sites.push_back(make_site(image, probe.lead_hit, kLeadCmpOffset, kLeadCmpStart,
                                     ExePatchSiteKind::lead_byte_range, kLeadOriginal, kLeadPatched,
                                     bytes));
    append_charset_sites(bytes, image, profile, report);

    const std::size_t base_count = report.sites.size();
    const bool base_patched = std::all_of(report.sites.begin(), report.sites.end(),
        [](const ExePatchSite& site) { return site.current_bytes == site.patched_bytes; });
    if (target == ExePatchTarget::gbk_zh_cn) {
        // 中文排版依赖 EAGLS 独有的指令窗口锚点，SAIMIN 的排版代码为独立编译产物。
        if (!profile.supports_typography) {
            throw Error(ErrorCode::invalid_content,
                        std::string(exe_patch_profile_name(profile.id)) +
                            " 版暂不支持中文排版（--typography zh-cn），仅 EAGLS 版可用。");
        }
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

const char* exe_patch_profile_name(ExePatchProfile profile) noexcept {
    switch (profile) {
    case ExePatchProfile::eagls:
        return "EAGLS";
    case ExePatchProfile::saimin:
        return "SAIMIN";
    }
    return "未知";
}

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
