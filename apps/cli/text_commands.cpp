// 文本清单命令只负责参数、调用与呈现；提取、校验与重建由核心完成。
#include "text_commands.hpp"

#include <eagls/error.hpp>
#include <eagls/text_map.hpp>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace cli {
namespace {
namespace fs = std::filesystem;

void usage() {
    std::cout
        << "用法：eagls text extract <文件或目录> --out FILE.json [--overwrite]\n"
        << "      eagls text apply   <文件或目录> --table FILE.json --out DIR [--overwrite] "
           "[--encoding cp932|gbk]\n\n"
        << "text extract：从原始 DAT（或目录中的 .dat）提取含日文文本与说话者的 JSON 清单。\n"
        << "              未改动的清单回填后与原始文件逐字节一致。\n"
        << "text apply：按清单重建加密 DAT 到输出目录；--encoding 选择译文编码（默认 cp932）。\n"
        << "              禁止就地覆盖输入；译文不得含 \" $ # & 或回车、换行。\n"
        << "目录输入只处理当前层的 .dat；--out 在 extract 中表示清单文件，在 apply 中表示目录。\n";
}

int invalid() {
    std::cerr << "错误：文本命令参数无效。请使用 eagls text --help。\n";
    return 2;
}

int dispatch(const std::vector<std::string>& args) {
    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
        usage();
        return 0;
    }
    if (args.empty()) return invalid();
    const bool extract = args[0] == "extract";
    const bool apply = args[0] == "apply";
    if (!extract && !apply) return invalid();

    std::optional<fs::path> input;
    std::optional<fs::path> out;
    std::optional<fs::path> table;
    bool overwrite = false;
    bool encoding_seen = false;
    unsigned codepage = 932;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--out") {
            if (out || ++i == args.size() || args[i].empty() || args[i][0] == '-') return invalid();
            out = fs::u8path(args[i]);
        } else if (args[i] == "--table") {
            if (table || ++i == args.size() || args[i].empty() || args[i][0] == '-') return invalid();
            table = fs::u8path(args[i]);
        } else if (args[i] == "--overwrite") {
            if (overwrite) return invalid();
            overwrite = true;
        } else if (args[i] == "--encoding") {
            if (encoding_seen || ++i == args.size()) return invalid();
            encoding_seen = true;
            if (args[i] == "cp932") {
                codepage = 932;
            } else if (args[i] == "gbk") {
                codepage = 936;
            } else {
                return invalid();
            }
        } else if (input || args[i].empty() || args[i][0] == '-') {
            return invalid();
        } else {
            input = fs::u8path(args[i]);
        }
    }
    if (!input || !out) return invalid();
    if (extract) {
        if (table || encoding_seen) return invalid();
        const auto summary = eagls::extract_text_table(*input, *out, overwrite);
        std::cout << "完成：清单包含 " << summary.file_count << " 个文件、" << summary.entry_count
                  << " 条文本、" << summary.speaker_count << " 个说话者。\n";
        return 0;
    }
    if (!table) return invalid();
    const auto summary = eagls::apply_text_table(*input, *table, *out, overwrite, codepage);
    std::cout << "完成：重建 " << summary.file_count << " 个 DAT（修改 " << summary.changed_entries
              << " 条文本、" << summary.changed_speakers << " 个说话者；输入中另有 "
              << summary.skipped_files << " 个清单外文件未处理）。\n";
    return 0;
}
}  // namespace

int run_text_command(const std::vector<std::string>& args) {
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
