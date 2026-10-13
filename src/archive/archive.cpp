#include <eagls/archive.hpp>

#include <eagls/error.hpp>

#include "archive/entry_name.hpp"
#include "archive/index_cipher.hpp"
#include "archive/index_record.hpp"
#include "support/utf8.hpp"
#include "support/bytes.hpp"
#include "support/file_io.hpp"

#include <algorithm>
#include <fstream>
#include <numeric>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace eagls {

namespace fs = std::filesystem;
namespace format = archive_format;

namespace {

// 每次拷贝的块大小：足够大以保持顺序读写的吞吐，又不会占用太多内存。
constexpr std::size_t copy_chunk_size = 1024 * 1024;

std::string path_text(const fs::path& path) {
    return path.u8string();
}

// 系统错误只给出数值：error_code::message() 在 Windows 上使用本地代码页，
// 直接拼进 UTF-8 文本会产生乱码。常见原因已由下面的明确检查给出中文说明。
std::string error_code_text(const std::error_code& ec) {
    return "（系统错误码 " + std::to_string(ec.value()) + "）";
}

std::string hex_text(const std::uint8_t* data, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (std::size_t i = 0; i < size; ++i) {
        if (i != 0) {
            text += ' ';
        }
        text += digits[data[i] >> 4];
        text += digits[data[i] & 0x0F];
    }
    return text;
}

using detail::read_u32_le;

// 确认路径指向存在的普通文件并返回大小，这样才能给出明确的中文原因。
using detail::regular_file_size;

// 已登记的索引记录布局：识别时要求索引大小与某一布局的完整大小（槽位表加种子）精确
// 相等。只接受有真实样本与引擎证据的布局，不做「能整除就接受」的猜测；未来取得新
// 证据后在这里登记。
constexpr archive_format::IndexFormat known_index_formats[] = {archive_format::index_28,
                                                              archive_format::index_40};

// 已知布局中最小的完整索引大小；比它还小的文件不可能是索引。
constexpr std::size_t min_known_index_file_size = archive_format::index_28.index_file_size();
static_assert(archive_format::index_28.index_file_size() <= archive_format::index_40.index_file_size(),
              "最小索引大小假定 28 字节布局最小；新增布局时需复核这个假定");

// 索引读取上限：已知布局最大约 400 KB，这里给一个远高于它的护栏，
// 避免把明显不是索引的超大文件整个读进内存。
constexpr std::uint64_t max_index_file_size = 16 * 1024 * 1024;

std::vector<std::uint8_t> read_index_file(const fs::path& path) {
    const std::uint64_t size = regular_file_size(path, "索引文件");
    if (size < min_known_index_file_size) {
        throw Error(ErrorCode::invalid_index,
                    "索引文件只有 " + std::to_string(size) + " 字节，小于最小已知布局的 " +
                        std::to_string(min_known_index_file_size) + " 字节；它可能不是索引文件：" +
                        path_text(path));
    }
    if (size > max_index_file_size) {
        throw Error(ErrorCode::invalid_index,
                    "索引文件为 " + std::to_string(size) + " 字节，超出已知索引布局的范围：" + path_text(path));
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw Error(ErrorCode::io, "无法打开索引文件：" + path_text(path));
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw Error(ErrorCode::io, "读取索引文件失败：" + path_text(path));
    }
    return bytes;
}

// 解析已解密的槽位表。遇到第一个非法槽位就抛出，不尝试跳过或猜测。
std::vector<ArchiveEntry> parse_table(const std::uint8_t* table, std::size_t table_size, std::uint64_t pak_size,
                                      const archive_format::IndexFormat& format) {
    // 调用方已按布局的完整大小精确匹配过索引文件，这里再确认一次，避免越界读取。
    if (table_size != format.table_size()) {
        throw Error(ErrorCode::invalid_index,
                    "内部错误：解密后的槽位表大小 " + std::to_string(table_size) + " 与布局要求的 " +
                        std::to_string(format.table_size()) + " 不一致。");
    }

    std::vector<ArchiveEntry> entries;
    for (std::size_t slot = 0; slot < format.slot_count; ++slot) {
        const std::uint8_t* record = table + slot * format.record_size;
        const std::uint32_t size = read_u32_le(record + format.size_position);

        // 「长度 0 终止」的布局（40 字节记录）下，引擎在第一个长度字段为 0 的记录处
        // 停止扫描——空槽也一样——其后的槽位游戏不可访问。这里同样停止，不把工具
        // 能读出但游戏读不到的槽位当成条目（引擎依据：SAIMIN.exe 0x41DD90 cmp [rec+20h],0）。
        if (format.zero_length_terminates && size == 0) {
            break;
        }

        if (record[0] == 0) {
            continue;  // 首字节为 0 即空槽位
        }

        std::size_t name_length = 0;
        while (name_length < format.name_size && record[name_length] != 0) {
            ++name_length;
        }

        const std::string slot_text = "槽位 " + std::to_string(slot);
        if (format.name_requires_nul && name_length == format.name_size) {
            throw Error(ErrorCode::invalid_entry,
                        slot_text + " 的文件名区（" + std::to_string(format.name_size) +
                            " 字节）内没有 NUL 终止符；该布局要求名称以 NUL 结尾（原始字节 " +
                            hex_text(record, format.name_size) + "）");
        }

        ArchiveEntry entry;
        entry.slot = slot;
        entry.name.assign(reinterpret_cast<const char*>(record), name_length);

        if (!detail::is_valid_utf8(entry.name)) {
            throw Error(ErrorCode::invalid_entry,
                        slot_text + " 的文件名不是合法的 UTF-8（原始字节 " + hex_text(record, name_length) +
                            "）。目前只验证过 ASCII/UTF-8 文件名，其他编码需要新的样本证据。");
        }
        for (const char c : entry.name) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte == 0x7F) {
                throw Error(ErrorCode::invalid_entry,
                            slot_text + " 的文件名含控制字符（原始字节 " + hex_text(record, name_length) + "）");
            }
        }

        entry.index_offset = detail::read_index_offset(record, format);
        entry.size = size;
        if (entry.index_offset < format::pak_offset_base) {
            throw Error(ErrorCode::entry_out_of_range,
                        slot_text + "（" + entry.name + "）的偏移 " + std::to_string(entry.index_offset) +
                            " 小于数据基数 " + std::to_string(format::pak_offset_base));
        }
        entry.pak_offset = entry.index_offset - format::pak_offset_base;

        // 用减法形式做范围检查，避免加法溢出；错误消息分列起点与长度，同样不做加法。
        if (entry.pak_offset > pak_size || entry.size > pak_size - entry.pak_offset) {
            throw Error(ErrorCode::entry_out_of_range,
                        slot_text + "（" + entry.name + "）的数据范围起点 " + std::to_string(entry.pak_offset) +
                            "、长度 " + std::to_string(entry.size) + " 超出数据文件（共 " +
                            std::to_string(pak_size) + " 字节）");
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

// 识别到的索引内容：布局、种子、解密后的槽位表与条目。
struct ParsedIndex {
    const archive_format::IndexFormat* format = nullptr;
    std::uint32_t seed = 0;
    std::vector<std::uint8_t> table;
    std::vector<ArchiveEntry> entries;
};

// 按精确大小匹配识别索引布局，再解密并解析。只接受已登记的布局；
// 大小不匹配时给出实际大小与全部已知布局的明确错误。
ParsedIndex parse_index(const std::vector<std::uint8_t>& index, std::uint64_t pak_size, const fs::path& path) {
    const archive_format::IndexFormat* matched = nullptr;
    for (const archive_format::IndexFormat& candidate : known_index_formats) {
        if (index.size() != candidate.index_file_size()) {
            continue;
        }
        if (matched != nullptr) {
            throw Error(ErrorCode::invalid_index,
                        "索引文件大小同时匹配多个已知布局，无法唯一确定：" + path_text(path));
        }
        matched = &candidate;
    }
    if (matched == nullptr) {
        std::string known;
        for (const archive_format::IndexFormat& candidate : known_index_formats) {
            known += "\n  " + std::to_string(candidate.index_file_size()) + " 字节（" +
                     std::to_string(candidate.slot_count) + " 个 " + std::to_string(candidate.record_size) +
                     " 字节槽位加 4 字节种子）";
        }
        throw Error(ErrorCode::invalid_index,
                    "索引文件大小为 " + std::to_string(index.size()) + " 字节，与任何已知记录布局都不匹配：" +
                        known + "\n文件：" + path_text(path));
    }

    ParsedIndex parsed;
    parsed.format = matched;
    parsed.seed = read_u32_le(index.data() + matched->table_size());
    parsed.table.assign(index.begin(), index.end() - static_cast<std::ptrdiff_t>(format::seed_size));
    detail::apply_index_cipher(parsed.table.data(), parsed.table.size(), parsed.seed);
    parsed.entries = parse_table(parsed.table.data(), parsed.table.size(), pak_size, *matched);
    return parsed;
}

// 提取按名称定位条目，重名会让结果含糊，因此不接受。
void reject_duplicate_names(const std::vector<ArchiveEntry>& entries) {
    std::unordered_map<std::string, std::size_t> first_slot_by_name;
    first_slot_by_name.reserve(entries.size());
    for (const ArchiveEntry& entry : entries) {
        const auto inserted = first_slot_by_name.emplace(detail::fold_ascii_case(entry.name), entry.slot);
        if (!inserted.second) {
            throw Error(ErrorCode::duplicate_name,
                        "槽位 " + std::to_string(inserted.first->second) + " 与槽位 " + std::to_string(entry.slot) +
                            " 的文件名重复（忽略 ASCII 大小写）：" + entry.name);
        }
    }
}

ArchiveLayout compute_layout(const std::vector<ArchiveEntry>& entries, std::uint64_t pak_size) {
    // 长度为 0 的条目不占用任何字节，不参与空洞与重叠统计。
    std::vector<const ArchiveEntry*> ranges;
    for (const ArchiveEntry& entry : entries) {
        if (entry.size != 0) {
            ranges.push_back(&entry);
        }
    }
    std::stable_sort(ranges.begin(), ranges.end(), [](const ArchiveEntry* left, const ArchiveEntry* right) {
        return left->pak_offset < right->pak_offset;
    });

    ArchiveLayout layout;
    std::uint64_t covered_end = 0;  // 已被条目覆盖到的最远物理位置
    bool first = true;
    for (const ArchiveEntry* entry : ranges) {
        const std::uint64_t begin = entry->pak_offset;
        if (first) {
            layout.leading_bytes = begin;
            first = false;
        } else if (begin > covered_end) {
            layout.gap_bytes += begin - covered_end;
        } else if (begin < covered_end) {
            ++layout.overlap_count;
        }
        covered_end = std::max(covered_end, begin + entry->size);
    }
    layout.trailing_bytes = pak_size > covered_end ? pak_size - covered_end : 0;
    return layout;
}

std::ifstream open_pak(const fs::path& path) {
    std::ifstream pak(path, std::ios::binary);
    if (!pak) {
        throw Error(ErrorCode::io, "无法打开数据文件：" + path_text(path));
    }
    return pak;
}

void seek_to_entry(std::ifstream& pak, const ArchiveEntry& entry, const fs::path& pak_path) {
    pak.clear();  // 上一次读到文件末尾会留下 eofbit，必须先清除才能重新定位
    pak.seekg(static_cast<std::streamoff>(entry.pak_offset), std::ios::beg);
    if (!pak) {
        throw Error(ErrorCode::io, "无法定位到条目 " + entry.name + " 的数据：" + path_text(pak_path));
    }
}

// 必须读满 count 字节；短读说明 .pak 在打开之后被截断或改动过。
void read_exact(std::ifstream& pak, char* buffer, std::size_t count, const ArchiveEntry& entry,
                const fs::path& pak_path) {
    pak.read(buffer, static_cast<std::streamsize>(count));
    if (pak.gcount() != static_cast<std::streamsize>(count)) {
        throw Error(ErrorCode::truncated_data,
                    "读取条目 " + entry.name + " 时数据文件提前结束，索引与数据文件不一致：" + path_text(pak_path));
    }
}

// 目标位置是否可以写入。覆盖只允许替换普通文件，不会碰目录或符号链接。
using detail::check_target_available;

// 除非调用 release()，否则析构时关闭并删除没写完的输出文件，
// 这样失败、取消或回调抛出异常都不会留下一个看起来完整的残缺文件。
class PartialFileGuard {
public:
    PartialFileGuard(fs::path path, std::ofstream& stream) : path_(std::move(path)), stream_(stream) {}
    PartialFileGuard(const PartialFileGuard&) = delete;
    PartialFileGuard& operator=(const PartialFileGuard&) = delete;

    ~PartialFileGuard() {
        if (!armed_) {
            return;
        }
        if (stream_.is_open()) {
            stream_.close();  // Windows 上必须先关闭才能删除
        }
        std::error_code ec;
        fs::remove(path_, ec);
    }

    void release() noexcept { armed_ = false; }

private:
    fs::path path_;
    std::ofstream& stream_;
    bool armed_ = true;
};

}  // namespace

Archive Archive::open(const fs::path& index_path, const fs::path& pak_path) {
    const std::vector<std::uint8_t> index = read_index_file(index_path);
    const std::uint64_t pak_size = regular_file_size(pak_path, "数据文件");

    ParsedIndex parsed = parse_index(index, pak_size, index_path);

    Archive archive;
    archive.pak_path_ = pak_path;
    archive.pak_size_ = pak_size;
    archive.format_ = *parsed.format;
    archive.seed_ = parsed.seed;
    // 保留解密后的槽位表作为回包模板；不含末尾 4 字节明文种子。
    archive.table_ = std::move(parsed.table);
    archive.entries_ = std::move(parsed.entries);
    reject_duplicate_names(archive.entries_);
    archive.layout_ = compute_layout(archive.entries_, pak_size);
    return archive;
}

std::optional<std::size_t> Archive::find(std::string_view name) const {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (detail::equals_ignoring_ascii_case(entries_[i].name, name)) {
            return i;
        }
    }
    return std::nullopt;
}

std::vector<std::uint8_t> Archive::read_entry(std::size_t entry_index) const {
    if (entry_index >= entries_.size()) {
        throw Error(ErrorCode::entry_not_found, "条目序号超出范围：" + std::to_string(entry_index));
    }
    const ArchiveEntry& entry = entries_[entry_index];

    std::ifstream pak = open_pak(pak_path_);
    seek_to_entry(pak, entry, pak_path_);
    std::vector<std::uint8_t> bytes(entry.size);
    if (!bytes.empty()) {
        read_exact(pak, reinterpret_cast<char*>(bytes.data()), bytes.size(), entry, pak_path_);
    }
    return bytes;
}

ExtractResult Archive::extract(const std::vector<std::size_t>& entry_indices, const fs::path& output_dir,
                               const ExtractOptions& options) const {
    // 一、校验序号并去重。
    std::vector<std::size_t> selected;
    selected.reserve(entry_indices.size());
    std::vector<bool> already_selected(entries_.size(), false);
    for (const std::size_t index : entry_indices) {
        if (index >= entries_.size()) {
            throw Error(ErrorCode::entry_not_found, "条目序号超出范围：" + std::to_string(index));
        }
        if (!already_selected[index]) {
            already_selected[index] = true;
            selected.push_back(index);
        }
    }

    ExtractResult result;
    if (selected.empty()) {
        return result;
    }

    // 按 .pak 内的物理顺序读取，避免机械硬盘上来回寻道。
    std::stable_sort(selected.begin(), selected.end(), [this](std::size_t left, std::size_t right) {
        return entries_[left].pak_offset < entries_[right].pak_offset;
    });

    // 二、预检：全部目标都合法且不会覆盖已有内容之后，才创建目录并写入。
    std::vector<fs::path> targets;
    targets.reserve(selected.size());
    std::uint64_t bytes_total = 0;
    for (const std::size_t index : selected) {
        const ArchiveEntry& entry = entries_[index];
        if (!detail::is_safe_file_name(entry.name)) {
            throw Error(ErrorCode::unsafe_name, "条目名不能安全地用作文件名，已拒绝提取：" + entry.name);
        }
        fs::path target = output_dir / fs::u8path(entry.name);  // 条目名是 UTF-8，必须经 u8path 转换
        check_target_available(target, options.overwrite);
        targets.push_back(std::move(target));
        bytes_total += entry.size;
    }

    std::error_code ec;
    fs::create_directories(output_dir, ec);
    if (ec || !fs::is_directory(output_dir)) {
        const std::string reason = ec ? error_code_text(ec) : std::string();
        throw Error(ErrorCode::io, "无法创建输出目录：" + path_text(output_dir) + reason);
    }

    // 三、逐个条目拷贝。
    std::ifstream pak = open_pak(pak_path_);
    std::vector<char> buffer(copy_chunk_size);
    std::uint64_t bytes_done = 0;

    for (std::size_t position = 0; position < selected.size(); ++position) {
        const ArchiveEntry& entry = entries_[selected[position]];
        const fs::path& target = targets[position];

        // 写入前再确认一次：即使两个不同名称在文件系统中等价，也不会静默覆盖刚写出的文件。
        check_target_available(target, options.overwrite);

        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw Error(ErrorCode::io, "无法创建输出文件：" + path_text(target));
        }
        PartialFileGuard guard(target, output);

        seek_to_entry(pak, entry, pak_path_);

        std::uint64_t entry_done = 0;
        const auto keep_going = [&]() {
            if (!options.on_progress) {
                return true;
            }
            return options.on_progress(
                ExtractProgress{entry, position + 1, selected.size(), entry_done, bytes_done, bytes_total});
        };

        if (entry.size == 0 && !keep_going()) {
            result.cancelled = true;
            return result;
        }
        while (entry_done < entry.size) {
            const auto count =
                static_cast<std::size_t>(std::min<std::uint64_t>(entry.size - entry_done, buffer.size()));
            read_exact(pak, buffer.data(), count, entry, pak_path_);
            output.write(buffer.data(), static_cast<std::streamsize>(count));
            if (!output) {
                throw Error(ErrorCode::io, "写入输出文件失败（可能是磁盘空间不足或权限问题）：" + path_text(target));
            }
            entry_done += count;
            bytes_done += count;
            if (!keep_going()) {
                result.cancelled = true;
                return result;  // guard 会删除没写完的文件
            }
        }

        output.close();
        if (output.fail()) {
            throw Error(ErrorCode::io, "关闭输出文件失败：" + path_text(target));
        }
        guard.release();
        ++result.files_written;
        result.bytes_written += entry.size;
    }
    return result;
}

ExtractResult Archive::extract_all(const fs::path& output_dir, const ExtractOptions& options) const {
    std::vector<std::size_t> all_indices(entries_.size());
    std::iota(all_indices.begin(), all_indices.end(), std::size_t{0});
    return extract(all_indices, output_dir, options);
}

}  // namespace eagls
