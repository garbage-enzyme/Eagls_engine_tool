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

void print_sites(const eagls::ExePatchReport& report) {
    for (const auto& site : report.sites) {
        std::cout << "  VA " << hex_va(site.instruction_va) << "  " << kind_text(site.kind)
                  << "  文件偏移 " << hex_offset(site.file_offset) << "："
                  << hex_byte(site.original_value) << " -> " << hex_byte(site.patched_value)
                  << '\n';
    }
}

void usage() {
    std::cout
        << "用法：eagls exe check <EAGLS.exe>\n"
        << "      eagls exe patch <EAGLS.exe> [--out FILE] [--overwrite]\n\n"
        << "exe check：定位 GBK 中文补丁位置并报告当前状态，不写文件。\n"
        << "exe patch：写出补丁副本；默认 <主文件名>-cn.exe，拒绝覆盖输入文件本身。\n"
        << "补丁同时修改字体字符集与双字节判定，使 GBK 译文正确显示；\n"
        << "文本请用 text apply --encoding gbk 打包。目前仅炎孕人生 EAGLS.exe 经游戏内验证。\n";
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
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--out") {
            if (out || ++i == args.size() || args[i].empty() || args[i][0] == '-') return invalid();
            out = fs::u8path(args[i]);
        } else if (args[i] == "--overwrite") {
            if (overwrite) return invalid();
            overwrite = true;
        } else if (input || args[i].empty() || args[i][0] == '-') {
            return invalid();
        } else {
            input = fs::u8path(args[i]);
        }
    }
    if (!input) return invalid();

    if (check) {
        if (out || overwrite) return invalid();
        const auto report = eagls::inspect_exe_file(*input);
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
    const auto report = eagls::patch_exe_file(*input, target, overwrite);
    std::cout << "已按 " << report.sites.size() << " 个位置写出补丁副本：\n";
    print_sites(report);
    std::cout << "已写出：" << target.u8string() << '\n';
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
