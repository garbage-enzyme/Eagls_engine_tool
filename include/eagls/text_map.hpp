#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "eagls/content.hpp"

namespace eagls {

// —— 文本清单（翻译工作流）——
// 所有字符串字段均为 UTF-8。original/from 是提取时的只读原文，text/to 是译者可编辑的
// 译文，回填只依据 text/to。offset 为"正文区"（解密后 3600 字节之后）内的字节偏移，
// 指向文本内容的首字节（即开引号之后）。id 对应脚本中的 &ID；无 &ID 的条目使用
// no_text_id。编码结论：本样本正文为 CP932，其余版本尚未验证。
inline constexpr std::uint32_t no_text_id = 0xFFFFFFFFu;

struct TextEntry {
    std::uint32_t id = no_text_id;
    std::uint32_t offset = 0;
    std::string original;
    std::string text;
};

struct SpeakerPair {
    std::string from;
    std::string to;
};

struct TextFileMap {
    std::string name;  // 归档条目名（如 a001.dat）
    std::vector<TextEntry> entries;
    // 提取输出辅助字段：该文件出现的说话者名（按首现顺序、已去重）；应用时不读取。
    std::vector<std::string> speakers;
};

// 完整清单：含文本的文件 + 全局说话者表（无文本的文件不列出）。
struct TextMap {
    std::string encoding = "cp932";  // 原文解码所用代码页名（当前仅支持 "cp932"）
    std::vector<TextFileMap> files;
    std::vector<SpeakerPair> speakers;
};

// 提取：解密脚本 → 条目（&ID 对话与其他含日文字面量）与说话者。
[[nodiscard]] TextFileMap extract_text_map(std::string name, const DatScript& script);

// 回填：校验清单并重建加密 DAT 字节。codepage 决定译文编码（932 默认 / 936 等）。
// 未修改条目直接保留原字节；全部未修改时输出与原始文件逐字节一致。
// 校验失败（清单与文件不一致、译文含禁止字符、无法按目标编码表示）抛出
// eagls::Error(invalid_content)，信息中包含文件与偏移上下文。
[[nodiscard]] std::vector<std::uint8_t> apply_text_map(const DatScript& script,
                                                       const TextFileMap& file_map,
                                                       const std::vector<SpeakerPair>& speakers,
                                                       unsigned codepage);

// JSON 清单读写（UTF-8；严格校验字段与取值，错误信息带行列）。
[[nodiscard]] std::string serialize_text_map(const TextMap& map);
[[nodiscard]] TextMap parse_text_map(std::string_view json);

// —— 目录级工作流（CLI 与未来的 GUI 共用）——

struct TextExtractSummary {
    std::size_t file_count = 0;     // 写入清单的文件数（无文本文件跳过）
    std::size_t entry_count = 0;    // 条目总数
    std::size_t speaker_count = 0;  // 全局说话者数
};

struct TextApplySummary {
    std::size_t file_count = 0;      // 重建的 DAT 文件数
    std::size_t changed_entries = 0;  // text != original 的条目数
    std::size_t changed_speakers = 0;  // to != from 的说话者条目数
    std::size_t skipped_files = 0;    // 输入目录中清单未包含的 DAT 数
};

// 从单个 DAT 或目录提取文本清单并写入 table_path（JSON）。--out 已存在时需 overwrite。
TextExtractSummary extract_text_table(const std::filesystem::path& input,
                                      const std::filesystem::path& table_path,
                                      bool overwrite);

// 按清单重建 DAT 到输出目录（只写清单内文件）。批量预检目标可用性；
// 逐文件处理，失败时保留已完成的结果并在错误信息中说明。
TextApplySummary apply_text_table(const std::filesystem::path& input,
                                  const std::filesystem::path& table_path,
                                  const std::filesystem::path& output_dir, bool overwrite,
                                  unsigned codepage = 932);

}  // namespace eagls
