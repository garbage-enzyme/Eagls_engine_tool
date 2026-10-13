// "eagls pak ..." 子命令的实现。
//
// 全部字符串使用 UTF-8；路径经 std::filesystem::u8path 转换，
// 保证中文路径与中文条目名在 MinGW 与 MSVC 下行为一致。
// 归档的读取与提取全部委托给 eagls::core，本层只负责参数、呈现与退出码。

#include "pak_commands.hpp"

#include <eagls/archive.hpp>
#include <eagls/error.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace cli {
namespace {

namespace fs = std::filesystem;

void print_usage() {
    std::cout
        << "用法：eagls pak <子命令> <idx> <pak> [选项]\n\n"
        << "子命令：\n"
        << "  info     显示归档信息（种子、条目数、数据大小、布局）\n"
        << "  list     列出全部条目（槽位、名称、物理偏移、大小）\n"
        << "  extract  提取条目（可选择个别条目）\n\n"
        << "extract 选项：\n"
        << "  --out DIR     输出目录（默认：<pak 所在目录>/<pak 主文件名>_out）\n"
        << "  --entry NAME  只提取指定条目，可重复；NAME 可带目录前缀，\n"
        << "                比较时只使用最后一个路径分隔符之后的部分\n"
        << "  --overwrite   允许覆盖输出目录中已存在的同名普通文件\n\n"
        << "示例：\n"
        << "  eagls pak list Script\\SCPACK.idx Script\\SCPACK.pak\n"
        << "  eagls pak extract Wave\\WAVEPACK.idx Wave\\WAVEPACK.pak --out out --entry loop.wav\n";
}

// 把 32 位种子格式化为固定 8 位的大写十六进制文本。
std::string hex8(std::uint32_t value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

// extract 子命令解析后的参数。
struct ExtractArgs {
    fs::path idx_path;
    fs::path pak_path;
    std::optional<fs::path> output_dir;
    std::vector<std::string> entry_names;
    bool overwrite = false;
};

// 去掉条目名里的目录前缀，只保留最后一个路径分隔符之后的部分。
// 分隔符 '/'、'\\' 都是单字节 ASCII，在 UTF-8 中不会出现在多字节字符内部。
std::string trim_entry_name(const std::string& raw) {
    const std::size_t position = raw.find_last_of("\\/");
    return position == std::string::npos ? raw : raw.substr(position + 1);
}

// 解析 extract 的位置参数与选项；失败时把原因写入标准错误。
bool parse_extract_options(const std::vector<std::string>& args, ExtractArgs& parsed) {
    std::vector<std::string> positional;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& item = args[i];
        if (item == "--out") {
            if (i + 1 >= args.size()) {
                std::cerr << "错误：--out 缺少目录参数。\n";
                return false;
            }
            parsed.output_dir = fs::u8path(args[++i]);
        } else if (item == "--entry") {
            if (i + 1 >= args.size()) {
                std::cerr << "错误：--entry 缺少条目名参数。\n";
                return false;
            }
            parsed.entry_names.push_back(trim_entry_name(args[++i]));
        } else if (item == "--overwrite") {
            parsed.overwrite = true;
        } else if (!item.empty() && item[0] == '-') {
            std::cerr << "错误：未知选项 " << item << "。\n";
            return false;
        } else {
            positional.push_back(item);
        }
    }
    if (positional.size() != 2) {
        std::cerr << "错误：需要恰好两个位置参数 <idx> <pak>，实际给出 " << positional.size()
                  << " 个。\n";
        return false;
    }
    parsed.idx_path = fs::u8path(positional[0]);
    parsed.pak_path = fs::u8path(positional[1]);
    return true;
}

// 默认输出目录：<pak 所在目录>/<pak 主文件名>_out。
fs::path default_output_dir(const fs::path& pak_path) {
    return pak_path.parent_path() / fs::u8path(pak_path.stem().u8string() + "_out");
}

void print_info(const eagls::Archive& archive, const fs::path& idx_path, const fs::path& pak_path) {
    std::cout << "索引文件：" << idx_path.u8string() << '\n'
              << "数据文件：" << pak_path.u8string() << '\n'
              << "种子：0x" << hex8(archive.seed()) << '\n'
              << "条目数：" << archive.entries().size() << '\n'
              << "数据大小：" << archive.pak_size() << " 字节\n";

    const eagls::ArchiveLayout& layout = archive.layout();
    std::cout << "布局：";
    if (layout.exact()) {
        std::cout << "条目首尾相接，完全覆盖数据文件\n";
    } else {
        std::cout << "前导 " << layout.leading_bytes << " 字节；条目间空洞 " << layout.gap_bytes
                  << " 字节；尾部 " << layout.trailing_bytes << " 字节；重叠 "
                  << layout.overlap_count << " 条\n";
    }
}

void print_list(const eagls::Archive& archive) {
    std::cout << "共 " << archive.entries().size() << " 个条目（槽位、名称、物理偏移、大小）：\n";
    for (const eagls::ArchiveEntry& entry : archive.entries()) {
        std::cout << '[' << std::setw(4) << std::setfill('0') << entry.slot << ']' << std::setfill(' ')
                  << ' ' << std::left << std::setw(20) << entry.name << std::right << "  "
                  << std::setw(12) << entry.pak_offset << "  " << entry.size << '\n';
    }
}

int run_extract(const std::vector<std::string>& args) {
    ExtractArgs parsed;
    if (!parse_extract_options(args, parsed)) {
        std::cerr << "请使用 eagls pak --help。\n";
        return 2;
    }

    const eagls::Archive archive = eagls::Archive::open(parsed.idx_path, parsed.pak_path);

    // 计算要提取的条目序号；全部条目时保持槽位顺序，指定名称时按名称逐个查找。
    std::vector<std::size_t> indices;
    if (parsed.entry_names.empty()) {
        indices.resize(archive.entries().size());
        for (std::size_t i = 0; i < indices.size(); ++i) {
            indices[i] = i;
        }
    } else {
        std::vector<std::string> missing;
        for (const std::string& name : parsed.entry_names) {
            const std::optional<std::size_t> found = archive.find(name);
            if (found.has_value()) {
                indices.push_back(*found);
            } else {
                missing.push_back(name);
            }
        }
        if (!missing.empty()) {
            std::cerr << "错误：在归档中没有找到以下条目：\n";
            for (const std::string& name : missing) {
                std::cerr << "  " << name << '\n';
            }
            return 1;
        }
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    }

    const fs::path output_dir =
        parsed.output_dir.has_value() ? *parsed.output_dir : default_output_dir(parsed.pak_path);

    std::cout << "正在提取 " << indices.size() << " 个条目到 " << output_dir.u8string() << '\n';

    eagls::ExtractOptions options;
    options.overwrite = parsed.overwrite;
    // 每写完一个条目打印一行；块级回调在条目内部触发时不重复输出。
    options.on_progress = [](const eagls::ExtractProgress& progress) {
        if (progress.entry_bytes_done == progress.entry.size) {
            std::cout << '[' << std::setw(4) << progress.entry_number << '/' << progress.entry_count
                      << "] " << progress.entry.name << "（" << progress.entry.size << " 字节）\n";
        }
        return true;
    };

    const eagls::ExtractResult result = archive.extract(indices, output_dir, options);
    std::cout << "完成：写出 " << result.files_written << " 个文件（" << result.bytes_written
              << " 字节）。\n";
    return 0;
}

int dispatch(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "错误：缺少子命令。请使用 eagls pak --help。\n";
        return 2;
    }

    const std::string& command = args[0];
    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    if (command == "info" || command == "list") {
        if (args.size() != 3) {
            std::cerr << "错误：" << command << " 需要恰好两个位置参数 <idx> <pak>。\n"
                      << "请使用 eagls pak --help。\n";
            return 2;
        }
        const fs::path idx_path = fs::u8path(args[1]);
        const fs::path pak_path = fs::u8path(args[2]);
        const eagls::Archive archive = eagls::Archive::open(idx_path, pak_path);
        if (command == "info") {
            print_info(archive, idx_path, pak_path);
        } else {
            print_list(archive);
        }
        return 0;
    }

    if (command == "extract") {
        return run_extract(args);
    }

    std::cerr << "错误：未知子命令 " << command << "。请使用 eagls pak --help。\n";
    return 2;
}

}  // namespace

int run_pak_command(const std::vector<std::string>& args) {
    try {
        return dispatch(args);
    } catch (const std::exception& error) {
        std::cerr << "错误：" << error.what() << '\n';
        return 1;
    }
}

}  // namespace cli
