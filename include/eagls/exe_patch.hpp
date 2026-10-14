#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace eagls {

// —— EAGLS.exe / SAIMIN.exe 中文显示补丁 ——
// 两个样本是同一引擎的不同编译版本，原版各有两类 Shift-JIS 硬编码，必须同时修改，
// GBK 译文才能正确显示：
//   1. 字体字符集：创建字体前用 mov byte ptr [esp+D], 80h 写入 LOGFONT.lfCharSet
//      （SHIFTJIS_CHARSET）。改为 GB2312_CHARSET(0x86) 后，双字节字符按 GBK 取字形。
//   2. 前导字节判定：唯一的双字节判定函数按 (81..A0 | E0..FC) 断字，脚本解析、排版与
//      出字共用。把第一段上界 A0 改为 FE 后，等价于 GBK 前导字节规则 81..FE。
// 两个版本的判定函数与字符集写入形态不同（EAGLS 29 字节判定 + 5 处短位移字符集；
// SAIMIN 27 字节判定 + 1 短 + 3 长位移字符集），定位按特征码自动探测版本：
// 每个版本必须唯一命中，两个版本同时命中或都不命中都明确失败，绝不在未识别的文件上落字节。
// 默认只识别旧 GBK 补丁；gbk_zh_cn 还识别中文排版（仅 EAGLS 版），
// 原字号/紧缩系数与单次悬挂策略不变。基础 GBK 补丁已游戏内验证，新增中文排版仍需游戏内验收。

enum class ExePatchTarget {
    gbk,        // 原有补丁，默认行为保持不变
    gbk_zh_cn,  // GBK 基础补丁 + 黑体/宋体 + 中文禁则和标点字距分类
};

// 目标程序的编译版本；由特征码自动识别，不作为输入参数。
enum class ExePatchProfile {
    eagls,   // EAGLS.exe（炎孕人生等）
    saimin,  // SAIMIN.exe（OLE-M 版）
};

// 识别到的版本名称（"EAGLS" / "SAIMIN"），用于报告与界面展示；返回静态字符串。
[[nodiscard]] const char* exe_patch_profile_name(ExePatchProfile profile) noexcept;

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
    ExePatchProfile profile = ExePatchProfile::eagls;  // 识别到的程序版本
};

// 纯内存：解析 32 位 x86 PE 映像并定位补丁位置，不修改输入。
// 不是 PE32、找不到任一已知版本的特征、判定特征不唯一或同时匹配多个版本时
// 抛出 Error（invalid_content / truncated_data）；gbk_zh_cn 仅 EAGLS 版支持。
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
