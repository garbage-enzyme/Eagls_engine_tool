#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace eagls {

// —— EAGLS.exe 中文显示补丁 ——
// 原版引擎有两类 Shift-JIS 硬编码，必须同时修改，GBK 译文才能正确显示：
//   1. 字体字符集：创建字体前用 mov byte ptr [esp+D], 80h 写入 LOGFONT.lfCharSet
//      （SHIFTJIS_CHARSET）。改为 GB2312_CHARSET(0x86) 后，双字节字符按 GBK 取字形。
//   2. 前导字节判定：唯一的双字节判定函数按 (81..A0 | E0..FC) 断字，脚本解析、排版与
//      出字共用。把第一段上界 A0 改为 FE 后，等价于 GBK 前导字节规则 81..FE。
// 默认只识别旧 GBK 补丁；gbk_zh_cn 还识别中文排版，原字号/紧缩系数与单次悬挂策略不变。
// 基础 GBK 补丁已游戏内验证，新增中文排版仍需游戏内验收。

enum class ExePatchTarget {
    gbk,        // 原有 6 字节补丁，默认行为保持不变
    gbk_zh_cn,  // GBK 基础补丁 + 黑体/宋体 + 中文禁则和标点字距分类
};

enum class ExePatchSiteKind {
    lead_byte_range,    // 双字节判定函数中 cmp al, 0A0h 的立即数
    font_charset,       // mov byte ptr [esp+D], 80h 写入的 lfCharSet
    font_face,          // 原位替换含 NUL 的字体名字节
    typography_code,    // 禁则与字距分派共用的原位指令块
};

struct ExePatchSite {
    ExePatchSiteKind kind = ExePatchSiteKind::font_charset;
    std::uint32_t instruction_va = 0;  // 指令或数据的虚拟地址，便于对照反汇编
    std::uint32_t file_offset = 0;     // 修改块的文件偏移
    // 三个字节块等长且非空；旧补丁的位置仍各自只有一个字节。
    std::vector<std::uint8_t> original_bytes;
    std::vector<std::uint8_t> patched_bytes;
    std::vector<std::uint8_t> current_bytes;
};

enum class ExePatchState {
    original,      // 全部为原版字节，可以打补丁
    patched,       // 已完成当前所选目标
    mixed,         // 未识别的部分修改
    base_patched,  // 已有完整的旧 GBK 补丁，中文排版部分仍为原版，可以升级
};

struct ExePatchReport {
    std::vector<ExePatchSite> sites;  // 按文件偏移升序
    ExePatchState state = ExePatchState::original;
};

// 纯内存：解析 32 位 x86 PE 映像并定位补丁位置，不修改输入。
// 不是 PE32、找不到特征或判定函数特征不唯一时抛出 Error（invalid_content / truncated_data）。
[[nodiscard]] ExePatchReport inspect_exe_patch(const std::vector<std::uint8_t>& image,
                                               ExePatchTarget target = ExePatchTarget::gbk);

// 纯内存：返回打好补丁的副本，并重新定位复核结果。
// 输入必须为 original 或已识别的 base_patched；重复应用及未知混改会被拒绝。
[[nodiscard]] std::vector<std::uint8_t> apply_exe_patch(const std::vector<std::uint8_t>& image,
                                                        ExePatchTarget target = ExePatchTarget::gbk);

// —— 文件级操作（CLI 与未来的 GUI 共用）——

ExePatchReport inspect_exe_file(const std::filesystem::path& input,
                                ExePatchTarget target = ExePatchTarget::gbk);

// 写出补丁副本：拒绝输出与输入为同一文件，默认不覆盖已有输出，失败时不留下半成品。
// 返回输入的定位报告（current_bytes 为补丁前的字节）。
ExePatchReport patch_exe_file(const std::filesystem::path& input,
                              const std::filesystem::path& output, bool overwrite = false,
                              ExePatchTarget target = ExePatchTarget::gbk);

}  // namespace eagls
