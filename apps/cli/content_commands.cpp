// 内容命令只负责参数、目录枚举和呈现，算法与单文件读写由核心完成。
#include "content_commands.hpp"

#include <eagls/content.hpp>
#include <eagls/error.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace cli {
namespace {
namespace fs = std::filesystem;

std::string ascii_lower(std::string value) {
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return value;
}

void usage() {
    std::cout
        << "用法：eagls dat decrypt <文件或目录> --out DIR [--overwrite]\n"
        << "      eagls dat text    <文件或目录> --out DIR [--overwrite]\n"
        << "      eagls gr decode   <文件或目录> --out DIR [--overwrite]\n\n"
        << "dat decrypt：保留标签表、终止符及种子的解密 DAT，不可直接回填游戏。\n"
        << "dat text：导出正文为 .txt，保留原始编码与换行，不自动转码。\n"
        << "gr decode：导出 24/32 位 BMP，修正大小字段并去除尾部填充。\n"
        << "目录输入只处理当前层的同扩展名文件；--out 始终表示输出目录。\n"
        << "默认不覆盖；不允许输出与输入指向同一文件。其他格式、编码与回包尚未实现。\n";
}

int dispatch(const std::string& format, const std::vector<std::string>& args) {
    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
        usage();
        return 0;
    }
    const auto invalid = []() {
        std::cerr << "错误：内容命令参数无效。请使用 eagls dat --help 或 eagls gr --help。\n";
        return 2;
    };
    if (args.empty()) return invalid();
    eagls::ContentAction action;
    std::string extension;
    if (format == "dat" && args[0] == "decrypt") {
        action = eagls::ContentAction::decrypt_dat;
        extension = ".dat";
    } else if (format == "dat" && args[0] == "text") {
        action = eagls::ContentAction::extract_dat_text;
        extension = ".txt";
    } else if (format == "gr" && args[0] == "decode") {
        action = eagls::ContentAction::decode_gr;
        extension = ".bmp";
    } else {
        return invalid();
    }
    std::optional<fs::path> input, output;
    bool overwrite = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--out") {
            if (output || ++i == args.size() || args[i].empty() || args[i][0] == '-') return invalid();
            output = fs::u8path(args[i]);
        } else if (args[i] == "--overwrite") {
            if (overwrite) return invalid();
            overwrite = true;
        } else if (input || args[i].empty() || args[i][0] == '-') {
            return invalid();
        } else {
            input = fs::u8path(args[i]);
        }
    }
    if (!input || !output) return invalid();

    std::vector<fs::path> files;
    if (fs::is_directory(*input)) {
        for (const auto& entry : fs::directory_iterator(*input)) {
            if (entry.is_regular_file() && ascii_lower(entry.path().extension().u8string()) == "." + format) {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
    } else if (fs::is_regular_file(*input)) {
        files.push_back(*input);
    } else {
        throw eagls::Error(eagls::ErrorCode::io, "输入不是普通文件或目录：" + input->u8string());
    }
    if (files.empty()) {
        throw eagls::Error(eagls::ErrorCode::io, "输入目录中没有匹配的文件。");
    }
    // 批量预检，防止同名目标或就地操作造成前面已写、后面才因覆盖而失败。
    std::vector<fs::path> targets;
    std::unordered_set<std::string> names;
    for (const auto& file : files) {
        const auto name = file.stem().u8string() + extension;
        if (!names.insert(ascii_lower(name)).second) {
            throw eagls::Error(eagls::ErrorCode::duplicate_name, "多个输入映射到同一输出文件：" + name);
        }
        const auto target = *output / fs::u8path(name);
        std::error_code ec;
        const bool same = fs::equivalent(file, target, ec);
        if (same || fs::weakly_canonical(file) == fs::weakly_canonical(target)) {
            throw eagls::Error(eagls::ErrorCode::output_exists, "禁止就地修改输入文件：" + file.u8string());
        }
        const auto status = fs::symlink_status(target, ec);
        if (status.type() != fs::file_type::not_found &&
            (ec || !overwrite || status.type() != fs::file_type::regular)) {
            throw eagls::Error(eagls::ErrorCode::output_exists, "输出位置已存在或不可写：" + target.u8string());
        }
        targets.push_back(target);
    }
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        total += eagls::convert_content_file(files[i], targets[i], action, overwrite);
        std::cout << '[' << i + 1 << '/' << files.size() << "] " << targets[i].u8string() << '\n';
    }
    std::cout << "完成：写出 " << files.size() << " 个文件（" << total << " 字节）。\n";
    return 0;
}
}  // namespace

int run_content_command(const std::string& format, const std::vector<std::string>& args) {
    try {
        return dispatch(format, args);
    } catch (const fs::filesystem_error& error) {
        std::cerr << "错误：文件系统操作失败（系统错误码 " << error.code().value() << "）。\n";
    } catch (const std::exception& error) {
        std::cerr << "错误：" << error.what() << '\n';
    }
    return 1;
}
}  // namespace cli
