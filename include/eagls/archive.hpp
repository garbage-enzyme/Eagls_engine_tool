#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eagls {

// EAGLS 的 IDX/PAK 归档由一个索引文件（.idx）和一个数据文件（.pak）组成。
//
// 下列常量都有真实样本作为证据（炎孕人生，SQUEEZ，2006，共三组归档），
// 证据与验证方法见 docs/eagls-archive.md。其他游戏或其他引擎版本是否沿用
// 同一格式尚未验证，遇到不符的文件时会明确报错，而不是猜测另一种布局。
namespace archive_format {

// 索引表固定有 10000 个槽位；未使用的槽位首字节为 0。
inline constexpr std::size_t slot_count = 10000;

// 每个槽位 28 字节：20 字节文件名 + u32 偏移 + u32 长度（整数均为小端）。
inline constexpr std::size_t slot_size = 28;

// 文件名字段宽度。名称不足 20 字节时以 NUL 补齐；恰好 20 字节时没有结束符。
inline constexpr std::size_t name_field_size = 20;

// 槽位表总字节数（10000 * 28）。
inline constexpr std::size_t table_size = slot_count * slot_size;

// 索引文件末尾 4 字节是明文的小端 u32 种子，用于解密前面的槽位表，本身不参与解密。
inline constexpr std::size_t seed_size = 4;

// 索引文件的总字节数，也是唯一接受的索引文件大小。
inline constexpr std::size_t index_file_size = table_size + seed_size;

// 槽位里记录的偏移 = 条目在 .pak 文件内的物理位置 + 此基数。
// 即真实数据从 .pak 的第 0 字节开始，而槽位里记录的首个偏移是 5963（0x174B）。
inline constexpr std::uint32_t pak_offset_base = 5963;

}  // namespace archive_format

// 索引中的一个条目（对应一个已使用的槽位）。
struct ArchiveEntry {
    // 在索引表中的槽位序号，范围 0..9999。
    std::size_t slot = 0;

    // 条目名。取自 20 字节名称字段的首个 NUL 之前（或全部 20 字节），
    // 已验证为合法 UTF-8 且不含控制字符，因此原始字节与展示文本一致。
    std::string name;

    // 索引里记录的偏移，包含 pak_offset_base。
    std::uint32_t index_offset = 0;

    // 条目数据的字节数。
    std::uint32_t size = 0;

    // 条目数据在 .pak 文件内的物理偏移，等于 index_offset - pak_offset_base。
    std::uint64_t pak_offset = 0;
};

// 条目与 .pak 文件字节之间的对应情况。
//
// 三组真实样本都是“完全对应”（条目首尾相接、覆盖整个文件），但这只是观察结果，
// 并非已证实的格式规则，所以这里只做报告，不会因此拒绝打开归档。
struct ArchiveLayout {
    // .pak 开头未被任何条目引用的字节数。
    std::uint64_t leading_bytes = 0;

    // 相邻条目之间（按物理偏移排序后）未被引用的字节数。
    std::uint64_t gap_bytes = 0;

    // 最后一个条目之后未被引用的字节数。
    std::uint64_t trailing_bytes = 0;

    // 起点落在前面条目范围内的条目数（重叠）。长度为 0 的条目不参与统计。
    std::size_t overlap_count = 0;

    // 条目是否恰好首尾相接并覆盖整个 .pak 文件。
    [[nodiscard]] bool exact() const noexcept {
        return leading_bytes == 0 && gap_bytes == 0 && trailing_bytes == 0 && overlap_count == 0;
    }
};

// 提取过程中传给进度回调的快照。
struct ExtractProgress {
    // 当前正在写入的条目。引用只在回调期间有效。
    const ArchiveEntry& entry;

    // 当前条目在本次提取中的序号（从 1 开始）与总数。
    std::size_t entry_number;
    std::size_t entry_count;

    // 当前条目已写入的字节数；等于 entry.size 表示这个条目写完了。
    std::uint64_t entry_bytes_done;

    // 本次提取累计已写入的字节数，以及预计写入的总字节数。
    std::uint64_t bytes_done;
    std::uint64_t bytes_total;
};

struct ExtractOptions {
    // 为 true 时允许覆盖输出目录里的同名普通文件。
    // 默认为 false：只要有一个目标已存在，就在写入任何文件之前整体失败。
    bool overwrite = false;

    // 进度回调。在调用 extract 的线程上同步触发（每写完约 1 MiB 一次，
    // 长度为 0 的条目触发一次）；返回 false 表示请求取消。
    // 回调不应抛出异常；若抛出，当前文件的残留会被清理，异常继续向外传播。
    std::function<bool(const ExtractProgress&)> on_progress;
};

struct ExtractResult {
    // 完整写出的文件数与字节数（被取消或失败的文件不计入）。
    std::size_t files_written = 0;
    std::uint64_t bytes_written = 0;

    // 回调请求取消时为 true。已写完的文件保留，正在写的文件会被删除。
    bool cancelled = false;
};

// 回包（repack）的结果统计。
struct RepackResult {
    // 从替换目录实际替换（重新写入）的条目数。
    std::size_t replaced = 0;

    // 归档里的条目总数。
    std::size_t entries = 0;

    // 写出的 .pak 字节数（即重建后数据文件的大小）。
    std::uint64_t bytes_written = 0;
};

// 一个已打开的 IDX/PAK 归档。
//
// 打开时读取并解密整个索引（约 280 KB），校验每个条目，但不读取 .pak 内容。
// 对象不持有文件句柄，每次读取数据都会重新打开 .pak，因此可以复制，
// 多个线程可以同时调用 const 成员函数。
//
// 失败时抛出 eagls::Error。
class Archive {
public:
    // 打开归档。index_path 为 .idx，pak_path 为 .pak，两者通常同名同目录。
    //
    // 校验内容：索引大小必须为 280004 字节；每个已使用槽位的文件名必须是合法 UTF-8
    // 且无控制字符；偏移不得小于基数；条目不得越过 .pak 末尾；不允许（忽略 ASCII
    // 大小写后）重名。不检查条目之间是否连续，该信息见 layout()。
    [[nodiscard]] static Archive open(const std::filesystem::path& index_path,
                                      const std::filesystem::path& pak_path);

    // 全部条目，按槽位顺序排列。
    [[nodiscard]] const std::vector<ArchiveEntry>& entries() const noexcept { return entries_; }

    // 索引末尾记录的种子（按无符号 32 位整数解释）。
    [[nodiscard]] std::uint32_t seed() const noexcept { return seed_; }

    // 打开时 .pak 文件的大小。
    [[nodiscard]] std::uint64_t pak_size() const noexcept { return pak_size_; }

    [[nodiscard]] const ArchiveLayout& layout() const noexcept { return layout_; }

    // 打开时解密出的槽位表（table_size 字节，不含末尾 4 字节明文种子）。
    // 回包以它为模板，未触及的原始字节（例如名称字段）保持原样。
    [[nodiscard]] const std::vector<std::uint8_t>& decrypted_table() const noexcept { return table_; }

    [[nodiscard]] const std::filesystem::path& pak_path() const noexcept { return pak_path_; }

    // 按名称查找条目，只忽略 ASCII 字母的大小写。返回 entries() 中的位置。
    [[nodiscard]] std::optional<std::size_t> find(std::string_view name) const;

    // 把一个条目的原始字节读入内存，不做任何解密或转换。
    // 条目序号超出范围时抛出 entry_not_found；.pak 比索引声明的短时抛出 truncated_data。
    [[nodiscard]] std::vector<std::uint8_t> read_entry(std::size_t entry_index) const;

    // 把指定条目原样写入 output_dir，文件名即条目名，不做任何解密或转换。
    //
    // entry_indices 是 entries() 中的位置，重复项只处理一次；实际按 .pak 内的物理
    // 顺序读取。写入前会先检查全部目标：条目名必须能安全地作为单个文件名
    // （否则 unsafe_name），目标已存在且未允许覆盖时抛出 output_exists。
    // 检查通过之后才创建输出目录并开始写入。
    //
    // 读写失败或回调抛出异常时，正在写的文件会被删除；已完整写出的文件保留。
    ExtractResult extract(const std::vector<std::size_t>& entry_indices,
                          const std::filesystem::path& output_dir,
                          const ExtractOptions& options = {}) const;

    // 提取全部条目，规则同 extract。
    ExtractResult extract_all(const std::filesystem::path& output_dir,
                              const ExtractOptions& options = {}) const;

private:
    Archive() = default;

    std::filesystem::path pak_path_;
    std::vector<ArchiveEntry> entries_;
    std::vector<std::uint8_t> table_;  // 解密后的槽位表，见 decrypted_table()
    std::uint32_t seed_ = 0;
    std::uint64_t pak_size_ = 0;
    ArchiveLayout layout_;
};

// 用替换目录重建归档：目录中的文件名（忽略 ASCII 大小写）与条目一一对应，
// 未出现在目录中的条目按原字节复制；索引以打开时解密出的槽位表为模板，
// 只重写受影响的偏移与长度字段，末尾种子保持原值。
//
// 只有 layout().exact() 的归档才能回包——条目首尾相接、完全覆盖数据文件，
// 这样「没有替换文件」时两个输出文件与原文件逐字节一致（已验证）。
// 输出路径不能与任一输入相同，两个输出也不能指向同一路径；默认不覆盖。
// 替换目录里每个普通文件都必须匹配到条目，未匹配或重复映射会明确报错。
RepackResult repack_archive(const std::filesystem::path& index_path,
                            const std::filesystem::path& pak_path,
                            const std::filesystem::path& from_dir,
                            const std::filesystem::path& out_index_path,
                            const std::filesystem::path& out_pak_path,
                            bool overwrite = false);

}  // namespace eagls
