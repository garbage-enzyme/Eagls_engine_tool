// exe 子命令只负责参数、调用与呈现；PE 解析与补丁定位由核心完成。
#include "exe_commands.hpp"

#include <eagls/error.hpp>
#include <eagls/exe_patch.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace cli {
namespace {
namespace fs = std::filesystem;

const char* state_text(eagls::ExePatchState state) {
    switch (state) {
    case eagls::ExePatchState::original:
        return "原版（未打补丁）";
    case eagls::ExePatchState::patched:
        return "已打补丁";
    case eagls::ExePatchState::base_patched:
        return "仅基础 GBK 补丁（可升级中文排版）";
    case eagls::ExePatchState::mixed:
        return "部分修改（文件可能被其他工具改动过）";
    }
    return "未知";
}

const char* kind_text(eagls::ExePatchSiteKind kind) {
    switch (kind) {
    case eagls::ExePatchSiteKind::lead_byte_range:
        return "前导字节判定";
    case eagls::ExePatchSiteKind::font_charset:
        return "字体字符集";
    case eagls::ExePatchSiteKind::font_face:
        return "字体名称";
    case eagls::ExePatchSiteKind::typography_code:
        return "中文禁则/字距指令块";
    }
    return "未知";
}

std::string hex_va(std::uint32_t value) {
    char buffer[9];
    std::snprintf(buffer, sizeof buffer, "%08X", static_cast<unsigned>(value));
    return buffer;
}

std::string hex_offset(std::uint32_t value) {
    char buffer[9];
    std::snprintf(buffer, sizeof buffer, "%08X", static_cast<unsigned>(value));
    return buffer;
}

std::string hex_byte(std::uint8_t value) {
    char buffer[3];
    std::snprintf(buffer, sizeof buffer, "%02X", static_cast<unsigned>(value));
    return buffer;
}

std::string hex_bytes(const std::vector<std::uint8_t>& bytes) {
    std::string text;
    for (std::size_t i = 0; i < bytes.size() && i < 14; ++i) {
        if (i != 0) text += ' ';
        text += hex_byte(bytes[i]);
    }
    if (bytes.size() > 14) text += " ...";
    return text;
}

void print_profile(eagls::ExePatchProfile profile) {
    std::cout << "识别版本：" << eagls::exe_patch_profile_name(profile) << '\n';
}

void print_sites(const eagls::ExePatchReport& report) {
    for (const auto& site : report.sites) {
        std::cout << "  VA " << hex_va(site.instruction_va) << "  " << kind_text(site.kind)
                  << "  文件偏移 " << hex_offset(site.file_offset) << "："
                  << hex_bytes(site.current_bytes) << " -> " << hex_bytes(site.patched_bytes);
        if (site.current_bytes.size() > 1) {
            std::cout << "（" << site.current_bytes.size() << " 字节）";
        }
        std::cout << '\n';
    }
}

void usage() {
    std::cout
        << "用法：eagls exe check <EAGLS.exe|SAIMIN.exe> [--typography zh-cn]\n"
        << "      eagls exe patch <EAGLS.exe|SAIMIN.exe> [--out FILE] [--overwrite] [--typography zh-cn]\n\n"
        << "exe check：自动识别 EAGLS / SAIMIN 版本，定位所选补丁位置并报告当前状态，不写文件。\n"
        << "exe patch：写出补丁副本；默认 <主文件名>-cn.exe，拒绝覆盖输入文件本身。\n"
        << "默认是已游戏内验证的 GBK 补丁（EAGLS 6 处 / SAIMIN 5 处），配合 text apply --encoding gbk。\n"
        << "--typography zh-cn：另换用 SimHei/SimSun、中文行首禁则及标点字距分类；仅 EAGLS 版可用。\n"
        << "可升级旧 GBK 补丁，仅支持已识别的游戏版本。沿用原紧缩系数与单次悬挂；\n"
        << "不含行尾禁则或连续标点成组处理，新增排版效果仍需游戏内验收。\n";
}

int invalid() {
    std::cerr << "错误：exe 命令参数无效。请使用 eagls exe --help。\n";
    return 2;
}

int dispatch(const std::vector<std::string>& args) {
    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
        usage();
        return 0;
    }
    if (args.empty()) return invalid();
    const bool check = args[0] == "check";
    const bool patch = args[0] == "patch";
    if (!check && !patch) return invalid();

    std::optional<fs::path> input;
    std::optional<fs::path> out;
    bool overwrite = false;
    bool typography = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--out") {
            if (out || ++i == args.size() || args[i].empty() || args[i][0] == '-') return invalid();
            out = fs::u8path(args[i]);
        } else if (args[i] == "--overwrite") {
            if (overwrite) return invalid();
            overwrite = true;
        } else if (args[i] == "--typography") {
            if (typography || ++i == args.size() || args[i] != "zh-cn") return invalid();
            typography = true;
        } else if (input || args[i].empty() || args[i][0] == '-') {
            return invalid();
        } else {
            input = fs::u8path(args[i]);
        }
    }
    if (!input) return invalid();
    const auto profile = typography ? eagls::ExePatchTarget::gbk_zh_cn : eagls::ExePatchTarget::gbk;

    if (check) {
        if (out || overwrite) return invalid();
        const auto report = eagls::inspect_exe_file(*input, profile);
        print_profile(report.profile);
        print_sites(report);
        std::cout << "补丁位置共 " << report.sites.size() << " 处；状态：" << state_text(report.state)
                  << "。\n";
        return 0;
    }

    fs::path target = *input;
    if (out) {
        target = *out;
    } else {
        // 默认与输入同目录，文件名附加 -cn（缺省扩展名时补 .exe）。
        const std::string stem = input->stem().u8string();
        std::string extension = input->extension().u8string();
        if (extension.empty()) extension = ".exe";
        target = input->parent_path() / fs::u8path(stem + "-cn" + extension);
    }
    const auto report = eagls::patch_exe_file(*input, target, overwrite, profile);
    print_profile(report.profile);
    std::cout << "已按 " << report.sites.size() << " 个位置写出补丁副本：\n";
    print_sites(report);
    std::cout << "已写出：" << target.u8string() << '\n';
    if (typography) {
        std::cout << "中文排版使用 SimHei/SimSun；沿用原紧缩系数与单次悬挂，游戏内效果待验收。\n";
    }
    return 0;
}
}  // namespace

int run_exe_command(const std::vector<std::string>& args) {
    try {
        return dispatch(args);
    } catch (const fs::filesystem_error& error) {
        std::cerr << "错误：文件系统操作失败（系统错误码 " << error.code().value() << "）。\n";
    } catch (const std::exception& error) {
        std::cerr << "错误：" << error.what() << '\n';
    }
    return 1;
}
}  // namespace cli
