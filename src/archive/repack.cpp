// IDX/PAK 回包：用替换目录重建归档，索引以打开时解密出的槽位表为模板。
//
// 无损重建策略（证据见 docs/eagls-archive.md）：
// - 只接受 layout().exact() 的归档：条目首尾相接、完全覆盖数据文件。
//   这样「没有替换文件」时按物理顺序复制就与原 .pak 逐字节一致。
// - 索引只重写受影响的偏移/长度字段：名称字段不重写、空槽位保持原样，
//   末尾种子保持原值，重新加密后写出；没有替换时索引也逐字节一致。
// - 未替换条目从原 .pak 分块复制，被替换文件同样分块读入，内存占用与文件大小无关。

#include <eagls/archive.hpp>
#include <eagls/error.hpp>

#include "archive/index_cipher.hpp"
#include "support/bytes.hpp"
#include "support/file_io.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <optional>
#include <system_error>
#include <vector>

namespace eagls {
namespace {

namespace fs = std::filesystem;
namespace format = archive_format;

std::string path_text(const fs::path& path) {
    return path.u8string();
}

// 每次拷贝的块大小：足够大以保持顺序读写的吞吐，又不会占用太多内存。
constexpr std::size_t copy_chunk_size = 1024 * 1024;

// 除非调用 release()，否则析构时关闭并删除没写完的输出文件，
// 这样失败或中断都不会留下一个看起来完整的残缺文件。
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

// 路径是否指向同一个文件：equivalent 也识别硬链接；归一化覆盖目标尚未存在的情况。
bool same_file(const fs::path& left, const fs::path& right) {
    std::error_code ec;
    const bool equivalent = fs::equivalent(left, right, ec);
    if (equivalent && !ec) {
        return true;
    }
    return fs::weakly_canonical(left) == fs::weakly_canonical(right);
}

}  // namespace

RepackResult repack_archive(const fs::path& index_path, const fs::path& pak_path, const fs::path& from_dir,
                            const fs::path& out_index_path, const fs::path& out_pak_path, bool overwrite) {
    // 一、打开归档：复用完整校验（索引大小、条目范围、重名等）。
    const Archive archive = Archive::open(index_path, pak_path);
    if (!archive.layout().exact()) {
        throw Error(ErrorCode::invalid_content,
                    "回包只支持条目首尾相接、完全覆盖数据文件的归档；该归档存在前导、空洞、尾部或重叠，"
                    "重建无法保证与原文件一致。");
    }

    // 二、输出位置检查：不允许覆盖输入，两个输出也不能指向同一路径。
    const fs::path inputs[] = {index_path, pak_path};
    const fs::path outputs[] = {out_index_path, out_pak_path};
    for (const fs::path& output : outputs) {
        for (const fs::path& input : inputs) {
            if (same_file(output, input)) {
                throw Error(ErrorCode::output_exists,
                            "输出不能与输入为同一文件，不允许就地修改游戏资源：" + path_text(output));
            }
        }
    }
    if (same_file(out_index_path, out_pak_path)) {
        throw Error(ErrorCode::output_exists, "两个输出文件不能指向同一路径：" + path_text(out_index_path));
    }
    detail::check_target_available(out_index_path, overwrite);
    detail::check_target_available(out_pak_path, overwrite);

    // 三、读取替换目录：每个普通文件都必须唯一匹配到一个条目。
    std::error_code ec;
    if (!fs::is_directory(from_dir, ec)) {
        throw Error(ErrorCode::io, "替换目录不存在或不是目录：" + path_text(from_dir));
    }

    const std::size_t entry_count = archive.entries().size();
    std::vector<bool> replaced(entry_count, false);
    std::vector<fs::path> replacement_paths(entry_count);
    std::vector<std::uint64_t> new_sizes(entry_count);
    for (std::size_t i = 0; i < entry_count; ++i) {
        new_sizes[i] = archive.entries()[i].size;
    }

    // 目录枚举顺序不稳定；排序后处理，报错列表与执行顺序都可复现。
    std::vector<fs::directory_entry> items;
    for (const fs::directory_entry& item : fs::directory_iterator(from_dir)) {
        std::error_code item_ec;
        if (item.is_regular_file(item_ec) && !item_ec) {
            items.push_back(item);
        }
        // 子目录与其他特殊项不参与替换。
    }
    std::sort(items.begin(), items.end(), [](const fs::directory_entry& left, const fs::directory_entry& right) {
        return left.path().filename().u8string() < right.path().filename().u8string();
    });

    std::vector<std::string> unmatched;
    for (const fs::directory_entry& item : items) {
        const fs::path& path = item.path();
        const std::string name = path.filename().u8string();
        const std::optional<std::size_t> found = archive.find(name);
        if (!found.has_value()) {
            unmatched.push_back(name);
            continue;
        }
        if (replaced[*found]) {
            throw Error(ErrorCode::duplicate_name,
                        "多个文件映射到同一个条目（忽略 ASCII 大小写）：" + archive.entries()[*found].name);
        }
        std::error_code size_ec;
        const std::uintmax_t size = fs::file_size(path, size_ec);
        if (size_ec) {
            throw Error(ErrorCode::io, "无法获取替换文件大小：" + path_text(path));
        }
        if (size > 0xFFFFFFFFu) {
            throw Error(ErrorCode::resource_limit,
                        "替换文件超过 4 GiB，索引的长度字段无法表示：" + path_text(path));
        }
        replaced[*found] = true;
        replacement_paths[*found] = path;
        new_sizes[*found] = static_cast<std::uint64_t>(size);
    }
    if (!unmatched.empty()) {
        std::string message = "以下文件没有匹配到归档条目（名称忽略 ASCII 大小写，且必须唯一）：";
        for (const std::string& name : unmatched) {
            message += "\n  " + name;
        }
        throw Error(ErrorCode::entry_not_found, message);
    }

    // 四、计算新布局：按原物理顺序排列有数据的条目，累加得到新偏移。
    std::vector<std::size_t> order;
    order.reserve(entry_count);
    for (std::size_t i = 0; i < entry_count; ++i) {
        if (new_sizes[i] != 0) {
            order.push_back(i);
        }
    }
    // 原始偏移互不相同（exact 布局已排除重叠），排序结果唯一。
    std::sort(order.begin(), order.end(), [&archive](std::size_t left, std::size_t right) {
        return archive.entries()[left].pak_offset < archive.entries()[right].pak_offset;
    });

    std::vector<std::uint64_t> new_offsets(entry_count, 0);
    std::uint64_t cursor = 0;
    for (const std::size_t index : order) {
        new_offsets[index] = cursor;
        cursor += new_sizes[index];
        if (cursor > 0xFFFFFFFFu - format::pak_offset_base) {
            throw Error(ErrorCode::resource_limit, "重建后的数据文件超过索引偏移字段能表示的上限。");
        }
    }

    // 五、写出数据文件：被替换的读替换文件，其余从原 .pak 分块复制。
    std::ifstream source(pak_path, std::ios::binary);
    if (!source) {
        throw Error(ErrorCode::io, "无法打开数据文件：" + path_text(pak_path));
    }
    if (!out_pak_path.parent_path().empty()) {
        fs::create_directories(out_pak_path.parent_path(), ec);
        if (ec) {
            throw Error(ErrorCode::io, "无法创建输出目录：" + path_text(out_pak_path.parent_path()));
        }
    }
    std::ofstream pak_output(out_pak_path, std::ios::binary | std::ios::trunc);
    if (!pak_output) {
        throw Error(ErrorCode::io, "无法创建输出文件：" + path_text(out_pak_path));
    }
    PartialFileGuard pak_guard(out_pak_path, pak_output);

    std::vector<char> buffer(copy_chunk_size);
    for (const std::size_t index : order) {
        const ArchiveEntry& entry = archive.entries()[index];
        std::ifstream replacement;
        std::istream* input = nullptr;
        if (replaced[index]) {
            replacement.open(replacement_paths[index], std::ios::binary);
            if (!replacement) {
                throw Error(ErrorCode::io, "无法打开替换文件：" + path_text(replacement_paths[index]));
            }
            input = &replacement;
        } else {
            source.clear();  // 上一次读到文件末尾会留下 eofbit，必须先清除才能重新定位
            source.seekg(static_cast<std::streamoff>(entry.pak_offset), std::ios::beg);
            if (!source) {
                throw Error(ErrorCode::io, "无法定位到条目 " + entry.name + " 的数据：" + path_text(pak_path));
            }
            input = &source;
        }

        std::uint64_t remaining = new_sizes[index];
        while (remaining > 0) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            input->read(buffer.data(), static_cast<std::streamsize>(count));
            if (input->gcount() != static_cast<std::streamsize>(count)) {
                throw Error(ErrorCode::truncated_data,
                            "读取条目 " + entry.name + " 的数据时提前结束；输入文件已被改动。");
            }
            pak_output.write(buffer.data(), static_cast<std::streamsize>(count));
            if (!pak_output) {
                throw Error(ErrorCode::io,
                            "写入输出文件失败（可能是磁盘空间不足或权限问题）：" + path_text(out_pak_path));
            }
            remaining -= count;
        }
    }
    pak_output.close();
    if (pak_output.fail()) {
        throw Error(ErrorCode::io, "关闭输出文件失败：" + path_text(out_pak_path));
    }

    // 六、写出索引：以解密表为模板，只改受影响的字段。
    // 没有替换时所有字段的新值都等于原值，因此表内容与原文件逐字节一致。
    std::vector<std::uint8_t> table = archive.decrypted_table();
    std::size_t replaced_count = 0;
    for (std::size_t i = 0; i < entry_count; ++i) {
        const ArchiveEntry& entry = archive.entries()[i];
        std::uint8_t* slot = table.data() + i * format::slot_size;
        if (replaced[i]) {
            ++replaced_count;
            // 长度字段必须与新内容一致（包括替换成空文件的情况）。
            if (new_sizes[i] != entry.size) {
                detail::write_u32_le(slot + format::name_field_size + 4,
                                     static_cast<std::uint32_t>(new_sizes[i]));
            }
        }
        if (new_sizes[i] > 0) {
            const auto index_offset = static_cast<std::uint32_t>(new_offsets[i] + format::pak_offset_base);
            if (index_offset != entry.index_offset) {
                detail::write_u32_le(slot + format::name_field_size, index_offset);
            }
        }
    }
    detail::apply_index_cipher(table.data(), format::table_size, archive.seed());
    table.resize(format::index_file_size);
    detail::write_u32_le(table.data() + format::table_size, archive.seed());

    // .idx 写入失败会自动删除自己的残片；此时 .pak 的 guard 仍未释放，同样会被清理。
    detail::write_binary_file(out_index_path, table, overwrite);
    pak_guard.release();

    return RepackResult{replaced_count, entry_count, cursor};
}

}  // namespace eagls
