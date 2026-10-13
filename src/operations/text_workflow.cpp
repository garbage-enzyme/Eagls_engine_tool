// 文本清单的目录级编排：文件枚举、聚合、清单读写与批量重建。
// 参数、目录枚举和呈现由 CLI 负责；这里只做共用的业务编排，供 CLI 与未来的 GUI 复用。
#include <eagls/text_map.hpp>

#include <eagls/error.hpp>

#include "support/file_io.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace eagls {
namespace {
namespace fs = std::filesystem;

std::string ascii_lower(std::string value) {
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return value;
}

std::string path_text(const fs::path& path) { return path.u8string(); }

// 收集输入中的 .dat：目录只扫描一层（与内容命令一致），大小写不敏感，按名称排序。
std::vector<fs::path> collect_dat_files(const fs::path& input) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (fs::is_directory(input, ec)) {
        for (const auto& entry : fs::directory_iterator(input)) {
            if (entry.is_regular_file() &&
                ascii_lower(entry.path().extension().u8string()) == ".dat") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
    } else if (fs::is_regular_file(input, ec)) {
        files.push_back(input);
    } else {
        throw Error(ErrorCode::io, "输入不是普通文件或目录：" + path_text(input));
    }
    if (files.empty()) {
        throw Error(ErrorCode::io, "输入目录中没有 .dat 文件。");
    }
    return files;
}

void write_text_file(const fs::path& path, const std::string& text, bool overwrite) {
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    detail::write_binary_file(path, bytes, overwrite);
}

std::string read_text_file(const fs::path& path) {
    const auto bytes = detail::read_binary_file(path);
    return std::string(bytes.begin(), bytes.end());
}

}  // namespace

TextExtractSummary extract_text_table(const fs::path& input, const fs::path& table_path,
                                      bool overwrite) {
    // 输出清单不能与输入（单文件）重叠。
    std::error_code ec;
    const bool same = fs::equivalent(input, table_path, ec);
    if (same || fs::weakly_canonical(input) == fs::weakly_canonical(table_path)) {
        throw Error(ErrorCode::output_exists, "清单输出位置不能与输入相同： " + path_text(table_path));
    }

    const auto files = collect_dat_files(input);

    TextMap map;
    for (const auto& file : files) {
        try {
            const auto bytes = detail::read_binary_file(file);
            const auto script = decode_dat(bytes);
            auto file_map = extract_text_map(file.filename().u8string(), script);
            if (file_map.entries.empty() && file_map.speakers.empty()) {
                continue;  // 无文本的文件不进入清单
            }
            for (const auto& speaker : file_map.speakers) {
                const auto found = std::find_if(map.speakers.begin(), map.speakers.end(),
                                                [&](const SpeakerPair& pair) {
                                                    return pair.from == speaker;
                                                });
                if (found == map.speakers.end()) {
                    map.speakers.push_back({speaker, speaker});
                }
            }
            file_map.speakers.clear();  // 辅助字段已合并进全局表
            map.files.push_back(std::move(file_map));
        } catch (const Error& error) {
            throw Error(error.code(), "处理 " + file.filename().u8string() + " 失败：" + error.what());
        }
    }

    TextExtractSummary summary;
    summary.file_count = map.files.size();
    summary.speaker_count = map.speakers.size();
    for (const auto& file : map.files) {
        summary.entry_count += file.entries.size();
    }

    write_text_file(table_path, serialize_text_map(map), overwrite);
    return summary;
}

TextApplySummary apply_text_table(const fs::path& input, const fs::path& table_path,
                                  const fs::path& output_dir, bool overwrite, unsigned codepage) {
    const auto map = parse_text_map(read_text_file(table_path));
    const auto files = collect_dat_files(input);

    // 按 ASCII 大小写不敏感的文件名索引输入文件。
    std::vector<std::pair<std::string, fs::path>> index;
    index.reserve(files.size());
    for (const auto& file : files) {
        index.emplace_back(ascii_lower(file.filename().u8string()), file);
    }

    // 预检：清单中的每个文件都必须存在，输出目标必须可用、且不与输入互为同一文件。
    struct Job {
        fs::path source;
        const TextFileMap* map;
        fs::path target;
    };
    std::vector<Job> jobs;
    for (const auto& file_map : map.files) {
        const auto found = std::find_if(index.begin(), index.end(), [&](const auto& item) {
            return item.first == ascii_lower(file_map.name);
        });
        if (found == index.end()) {
            throw Error(ErrorCode::io, "清单中的文件在输入中不存在：" + file_map.name);
        }
        const fs::path target = output_dir / fs::path(file_map.name).filename();
        std::error_code ec;
        const bool same = fs::equivalent(found->second, target, ec);
        if (same || fs::weakly_canonical(found->second) == fs::weakly_canonical(target)) {
            throw Error(ErrorCode::output_exists,
                        "禁止就地覆盖输入文件（输出目录与输入相同？）：" + path_text(target));
        }
        detail::check_target_available(target, overwrite);
        jobs.push_back({found->second, &file_map, target});
    }
    if (jobs.empty()) {
        throw Error(ErrorCode::invalid_content, "清单中没有任何文件。");
    }

    TextApplySummary summary;
    // 说话者修改数按清单全局统计一次（与文件数量无关）。
    for (const auto& pair : map.speakers) {
        if (pair.to != pair.from) ++summary.changed_speakers;
    }
    for (const auto& job : jobs) {
        try {
            const auto bytes = detail::read_binary_file(job.source);
            const auto script = decode_dat(bytes);
            const auto rebuilt = apply_text_map(script, *job.map, map.speakers, codepage);
            detail::write_binary_file(job.target, rebuilt, overwrite);
            for (const auto& entry : job.map->entries) {
                if (entry.text != entry.original) ++summary.changed_entries;
            }
        } catch (const Error& error) {
            throw Error(error.code(),
                        "处理 " + job.source.filename().u8string() + " 失败：" + error.what());
        }
        ++summary.file_count;
    }
    summary.skipped_files = files.size() - jobs.size();
    return summary;
}

}  // namespace eagls
