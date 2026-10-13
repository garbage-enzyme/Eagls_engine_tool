#include <eagls/archive.hpp>
#include <eagls/content.hpp>
#include <eagls/error.hpp>
#include <eagls/exe_patch.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;

template <typename Operation>
bool expect_error(eagls::ErrorCode code, Operation operation) {
    try {
        operation();
    } catch (const eagls::Error& error) {
        return error.code() == code;
    }
    return false;
}

// 只有显式传入本地样本目录时才读取这些文件；不修改或执行游戏资源。
std::vector<std::uint8_t> read_sample(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("本地内容样本不可读。");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void require(bool condition) {
    if (!condition) throw std::runtime_error("本地内容样本回归断言失败。");
}

// 合成一张 8x8 的 24 位 BMP：像素用 4 字节短周期图案，保证 LZSS 命中匹配与重叠引用分支。
std::vector<std::uint8_t> make_synthetic_bmp() {
    const std::uint32_t width = 8;
    const std::uint32_t height = 8;
    const std::uint64_t stride = ((static_cast<std::uint64_t>(width) * 24 + 31) / 32) * 4;  // 24
    const std::uint64_t total = 54 + stride * height;                                        // 246
    std::vector<std::uint8_t> bmp(static_cast<std::size_t>(total), 0);
    bmp[0] = 'B';
    bmp[1] = 'M';
    bmp[2] = static_cast<std::uint8_t>(total);             // bfSize 低字节（246）
    bmp[10] = 54;                                          // bfOffBits
    bmp[14] = 40;                                          // biSize
    bmp[18] = static_cast<std::uint8_t>(width);
    bmp[22] = static_cast<std::uint8_t>(height);
    bmp[26] = 1;                                           // planes
    bmp[28] = 24;                                          // bpp
    bmp[34] = static_cast<std::uint8_t>(stride * height);  // biSizeImage 低字节（192）
    static const std::uint8_t pattern[4] = {0x11, 0x22, 0x33, 0x44};
    for (std::size_t i = 54; i < bmp.size(); ++i) {
        bmp[i] = pattern[(i - 54) % 4];
    }
    return bmp;
}

// 合成数据的 GR 编码往返：LZSS 与全字面量两条路径都必须还原出逐字节一致的 BMP。
bool verify_synthetic_encode() {
    using eagls::ErrorCode;
    const auto bmp = make_synthetic_bmp();
    bool ok = true;

    // 非法输入必须明确报错，不猜测其他变体。
    ok &= expect_error(ErrorCode::invalid_content, [] { (void)eagls::encode_gr(std::vector<std::uint8_t>(10)); });
    ok &= expect_error(ErrorCode::invalid_content, [&] {
        auto bad_bpp = bmp;
        bad_bpp[28] = 16;
        (void)eagls::encode_gr(bad_bpp);
    });
    ok &= expect_error(ErrorCode::invalid_content, [&] {
        auto bad_magic = bmp;
        bad_magic[0] = 'X';
        (void)eagls::encode_gr(bad_magic);
    });
    ok &= expect_error(ErrorCode::invalid_content, [&] {
        auto truncated = bmp;
        truncated.pop_back();
        (void)eagls::encode_gr(truncated);
    });

    // LZSS 路径（默认选项，seed 为 0）。
    const auto encoded = eagls::encode_gr(bmp);
    ok &= encoded.size() >= eagls::content_format::gr_encrypted_size + 1;
    const auto decoded = eagls::decode_gr(encoded);
    ok &= decoded.bitmap == bmp;
    ok &= decoded.seed == 0 && decoded.width == 8 && decoded.height == 8 && decoded.bits_per_pixel == 24;

    // 全字面量路径：仍必须可解码；种子按选项写入。
    eagls::GrEncodeOptions store_options;
    store_options.store = true;
    store_options.seed = 0x7D;
    const auto stored = eagls::decode_gr(eagls::encode_gr(bmp, store_options));
    ok &= stored.bitmap == bmp;
    ok &= stored.seed == 0x7D;
    return ok;
}

// ---------------------------------------------------------------------------
// 归档索引布局的合成用例（不读取游戏资源）
// ---------------------------------------------------------------------------

void require_synth(bool condition) {
    if (!condition) throw std::runtime_error("合成归档布局断言失败。");
}

void put_u32_le(std::uint8_t* out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>(value >> (i * 8));
}

void put_u64_le(std::uint8_t* out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>(value >> (i * 8));
}

std::uint32_t get_u32_le(const std::uint8_t* in) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(in[i]) << (i * 8);
    return value;
}

// 在测试内独立实现索引加密（MSVC LCG + 46 字节密钥异或）：与生产实现交叉验证，
// 由这里加密的索引必须能被 Archive::open 按同一算法解密并解析。
std::vector<std::uint8_t> encrypt_index_table(std::vector<std::uint8_t> table, std::uint32_t seed) {
    constexpr std::string_view key = "1qaz2wsx3edc4rfv5tgb6yhn7ujm8ik,9ol.0p;/-@:^[]";
    std::uint32_t state = seed;
    for (std::uint8_t& byte : table) {
        state = state * 214013u + 2531011u;
        byte ^= static_cast<std::uint8_t>(key[((state >> 16) & 0x7FFFu) % key.size()]);
    }
    return table;
}

void write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("无法创建合成归档文件。");
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("写出合成归档文件失败。");
}

// 一条原始槽位记录：字段值由测试直接指定，可用来构造极端或非法的输入。
struct RawRecord {
    std::string name;
    std::uint64_t index_offset = 0;
    std::uint32_t size = 0;
    std::uint32_t reserved = 0;
};

// 高层条目：按顺序放进槽位；名称为空表示保留一个空槽，内容在数据文件里首尾相接。
struct SyntheticEntry {
    std::string name;
    std::vector<std::uint8_t> data;
    std::uint32_t reserved = 0;
};

// 按布局把原始记录写成一个完整的加密索引文件（末尾追加明文种子）。
void write_raw_index(const fs::path& path, const eagls::archive_format::IndexFormat& format,
                     const std::vector<RawRecord>& records, std::uint32_t seed) {
    std::vector<std::uint8_t> table(format.table_size(), 0);
    for (std::size_t slot = 0; slot < records.size(); ++slot) {
        const RawRecord& record = records[slot];
        if (record.name.size() > format.name_size) {
            throw std::runtime_error("合成记录的名称超过布局的名称区大小。");
        }
        std::uint8_t* target = table.data() + slot * format.record_size;
        std::memcpy(target, record.name.data(), record.name.size());
        if (format.offset_width == 8) {
            put_u64_le(target + format.offset_position, record.index_offset);
        } else {
            put_u32_le(target + format.offset_position, static_cast<std::uint32_t>(record.index_offset));
        }
        put_u32_le(target + format.size_position, record.size);
        if (format.record_size - format.size_position > 4) {
            put_u32_le(target + format.size_position + 4, record.reserved);
        }
    }
    std::vector<std::uint8_t> index = encrypt_index_table(std::move(table), seed);
    index.resize(index.size() + eagls::archive_format::seed_size);
    put_u32_le(index.data() + index.size() - eagls::archive_format::seed_size, seed);
    write_file(path, index);
}

// 合成一个归档并写盘：条目按顺序放进槽位 0..N-1，数据文件里首尾相接、
// 偏移从基数累加；返回 <idx 路径, pak 路径>。
std::pair<fs::path, fs::path> write_synthetic_archive(const fs::path& dir,
                                                      const eagls::archive_format::IndexFormat& format,
                                                      const std::vector<SyntheticEntry>& entries,
                                                      std::uint32_t seed) {
    std::vector<RawRecord> records;
    records.reserve(entries.size());
    std::vector<std::uint8_t> pak;
    for (const SyntheticEntry& entry : entries) {
        RawRecord record;
        if (!entry.name.empty()) {
            record.name = entry.name;
            record.index_offset = eagls::archive_format::pak_offset_base + pak.size();
            record.size = static_cast<std::uint32_t>(entry.data.size());
            record.reserved = entry.reserved;
            pak.insert(pak.end(), entry.data.begin(), entry.data.end());
        }
        records.push_back(std::move(record));
    }

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw std::runtime_error("无法创建合成归档目录。");
    const fs::path idx_path = dir / "SYNTH.idx";
    const fs::path pak_path = dir / "SYNTH.pak";
    write_raw_index(idx_path, format, records, seed);
    write_file(pak_path, pak);
    return {idx_path, pak_path};
}

// 40 与 28 字节索引布局的合成用例：覆盖名称边界、零长度终止、64 位偏移、
// 保留区、回包往返与错误拒绝路径，不依赖任何游戏资源。
void verify_synthetic_archive_layouts(const fs::path& scratch) {
    using eagls::ErrorCode;
    using eagls::archive_format::index_28;
    using eagls::archive_format::index_40;

    std::error_code ec;
    fs::remove_all(scratch, ec);
    for (const char* name : {"ok40-empty", "ok28-empty"}) {
        fs::create_directories(scratch / name, ec);
        require_synth(!ec);
    }

    // 一、40 字节布局基本归档：22 字节名称（旧 20 字节名称区放不下）、64 位偏移读写。
    const std::vector<std::uint8_t> data_a(3778, 0x5A);
    const std::vector<std::uint8_t> data_b(6, 0x11);
    const std::vector<std::uint8_t> data_c(3, 0x22);
    const auto [idx40, pak40] = write_synthetic_archive(
        scratch / "ok40", index_40,
        {{"02Char_Move_Single.dat", data_a, 0xAABBCCDD}, {"b.gr", data_b, 0}, {"c.dat", data_c, 0}}, 0x11223344);

    const eagls::Archive archive40 = eagls::Archive::open(idx40, pak40);
    require_synth(archive40.index_format().record_size == 40);
    require_synth(archive40.index_format().name_size == 24);
    require_synth(archive40.index_format().offset_width == 8);
    require_synth(archive40.index_format().slot_count == 10000);
    require_synth(archive40.seed() == 0x11223344);
    require_synth(archive40.entries().size() == 3);
    require_synth(archive40.entries()[0].name == "02Char_Move_Single.dat");
    require_synth(archive40.entries()[0].pak_offset == 0 && archive40.entries()[0].size == 3778);
    require_synth(archive40.entries()[2].pak_offset == 3778 + 6);
    require_synth(archive40.layout().exact());
    require_synth(archive40.read_entry(0) == data_a);
    require_synth(archive40.read_entry(1) == data_b);
    // 解密表里的保留区必须保持原样，不能被清零或被解释成长度高位。
    require_synth(archive40.decrypted_table().size() == index_40.table_size());
    require_synth(get_u32_le(archive40.decrypted_table().data() + 36) == 0xAABBCCDD);

    // 一之二、没有替换文件时回包：两个输出都必须与原文件逐字节一致。
    const eagls::RepackResult empty40 =
        eagls::repack_archive(idx40, pak40, scratch / "ok40-empty", scratch / "repack40-empty" / "SYNTH.idx",
                              scratch / "repack40-empty" / "SYNTH.pak", true);
    require_synth(empty40.replaced == 0 && empty40.entries == 3);
    require_synth(read_sample(scratch / "repack40-empty" / "SYNTH.idx") == read_sample(idx40));
    require_synth(read_sample(scratch / "repack40-empty" / "SYNTH.pak") == read_sample(pak40));

    // 一之三、替换一个条目：新内容可读、其余条目不变、保留区仍原样。
    const std::vector<std::uint8_t> replacement(9, 0x77);
    {
        fs::create_directories(scratch / "ok40-self", ec);
        std::ofstream file(scratch / "ok40-self" / "b.gr", std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(replacement.data()),
                   static_cast<std::streamsize>(replacement.size()));
        require_synth(file.good());
    }
    const eagls::RepackResult self40 =
        eagls::repack_archive(idx40, pak40, scratch / "ok40-self", scratch / "repack40-self" / "SYNTH.idx",
                              scratch / "repack40-self" / "SYNTH.pak", true);
    require_synth(self40.replaced == 1 && self40.entries == 3);
    const eagls::Archive rebuilt40 = eagls::Archive::open(scratch / "repack40-self" / "SYNTH.idx",
                                                          scratch / "repack40-self" / "SYNTH.pak");
    const auto rebuilt_b = rebuilt40.find("b.gr");
    const auto rebuilt_a = rebuilt40.find("02Char_Move_Single.dat");
    const auto rebuilt_c = rebuilt40.find("c.dat");
    require_synth(rebuilt_b.has_value() && rebuilt_a.has_value() && rebuilt_c.has_value());
    require_synth(rebuilt40.read_entry(*rebuilt_b) == replacement);
    require_synth(rebuilt40.read_entry(*rebuilt_a) == data_a);
    require_synth(rebuilt40.read_entry(*rebuilt_c) == data_c);
    require_synth(rebuilt40.layout().exact());
    require_synth(get_u32_le(rebuilt40.decrypted_table().data() + 36) == 0xAABBCCDD);

    // 一之四、替换成空文件：40 字节布局必须拒绝（长度 0 是引擎的扫描终止标记）。
    {
        fs::create_directories(scratch / "ok40-zero", ec);
        std::ofstream file(scratch / "ok40-zero" / "c.dat", std::ios::binary | std::ios::trunc);
        require_synth(file.good());
    }
    require_synth(expect_error(ErrorCode::invalid_content, [&] {
        (void)eagls::repack_archive(idx40, pak40, scratch / "ok40-zero", scratch / "repack40-zero" / "SYNTH.idx",
                                    scratch / "repack40-zero" / "SYNTH.pak", true);
    }));
    require_synth(!fs::exists(scratch / "repack40-zero" / "SYNTH.idx"));
    require_synth(!fs::exists(scratch / "repack40-zero" / "SYNTH.pak"));

    // 二、40 字节布局的零长度终止：长度 0 的已用记录与空槽都会终止扫描，
    // 其后的条目在游戏与工具里都不可见。
    const auto [idx40t, pak40t] = write_synthetic_archive(
        scratch / "term40", index_40, {{"first.dat", data_c, 0}, {"blocker.dat", {}, 0}, {"after.dat", data_b, 0}},
        0x5);
    const eagls::Archive terminated = eagls::Archive::open(idx40t, pak40t);
    require_synth(terminated.entries().size() == 1);
    require_synth(terminated.entries()[0].name == "first.dat");

    const auto [idx40e, pak40e] = write_synthetic_archive(
        scratch / "empty40", index_40, {{"first.dat", data_c, 0}, {"", {}, 0}, {"after.dat", data_b, 0}}, 0x6);
    const eagls::Archive with_empty_slot = eagls::Archive::open(idx40e, pak40e);
    require_synth(with_empty_slot.entries().size() == 1);

    // 三、40 字节布局：名称区写满 24 字节且没有 NUL 必须被拒绝（引擎按 NUL 复制名称）。
    const std::string full_name(24, 'x');
    const auto [idx40f, pak40f] =
        write_synthetic_archive(scratch / "full40", index_40, {{full_name, data_c, 0}}, 0x7);
    require_synth(expect_error(ErrorCode::invalid_entry, [&] { (void)eagls::Archive::open(idx40f, pak40f); }));

    // 四、64 位偏移：高 32 位非零的偏移必须按 64 位读取（若只读低 DWORD，
    // 会误判为偏移 5963 即数据文件起点而错误地通过检查）。
    {
        fs::create_directories(scratch / "high40", ec);
        const fs::path high_idx = scratch / "high40" / "SYNTH.idx";
        const fs::path high_pak = scratch / "high40" / "SYNTH.pak";
        write_file(high_pak, std::vector<std::uint8_t>(100, 0x33));
        write_raw_index(high_idx, index_40, {{"high.dat", 0x100000000ull + 5963, 10, 0}}, 0x77);
        require_synth(
            expect_error(ErrorCode::entry_out_of_range, [&] { (void)eagls::Archive::open(high_idx, high_pak); }));
        // 对照：同一记录把高位清零就合法，证明上面的拒绝来自高位而非其他字段。
        write_raw_index(high_idx, index_40, {{"high.dat", 5963, 10, 0}}, 0x77);
        const eagls::Archive low = eagls::Archive::open(high_idx, high_pak);
        require_synth(low.read_entry(0) == std::vector<std::uint8_t>(10, 0x33));
    }

    // 五、28 字节布局回归：20 字节名称恰好占满（无 NUL）可以接受；
    // 中间的零长度已用记录不终止扫描（与 40 字节布局相反）。
    const std::string name20(20, 'n');
    const auto [idx28, pak28] = write_synthetic_archive(
        scratch / "ok28", index_28,
        {{name20, data_a, 0}, {"zero.dat", {}, 0}, {"tail.dat", data_c, 0}}, 0x28);
    const eagls::Archive archive28 = eagls::Archive::open(idx28, pak28);
    require_synth(archive28.index_format().record_size == 28);
    require_synth(archive28.index_format().offset_width == 4);
    require_synth(archive28.entries().size() == 3);
    require_synth(archive28.entries()[0].name == name20);
    require_synth(archive28.entries()[1].size == 0);
    require_synth(archive28.entries()[2].name == "tail.dat");
    // 28 字节布局的无修改回包同样必须与原文件逐字节一致。
    const eagls::RepackResult empty28 =
        eagls::repack_archive(idx28, pak28, scratch / "ok28-empty", scratch / "repack28-empty" / "SYNTH.idx",
                              scratch / "repack28-empty" / "SYNTH.pak", true);
    require_synth(empty28.replaced == 0 && empty28.entries == 3);
    require_synth(read_sample(scratch / "repack28-empty" / "SYNTH.idx") == read_sample(idx28));
    require_synth(read_sample(scratch / "repack28-empty" / "SYNTH.pak") == read_sample(pak28));

    // 六、索引大小不匹配任何已知布局（含 0 与明显过小）：必须明确拒绝，不做整除猜测。
    const std::size_t bad_index_sizes[] = {400005, 400003, 280005, 100, 0};
    for (const std::size_t size : bad_index_sizes) {
        const fs::path bad_idx = scratch / "bad-size.idx";
        write_file(bad_idx, std::vector<std::uint8_t>(size, 0));
        require_synth(expect_error(ErrorCode::invalid_index, [&] { (void)eagls::Archive::open(bad_idx, pak40); }));
    }

    // 七、条目越过数据文件末尾：必须被拒绝。
    {
        fs::create_directories(scratch / "oob40", ec);
        const fs::path oob_idx = scratch / "oob40" / "SYNTH.idx";
        const fs::path oob_pak = scratch / "oob40" / "SYNTH.pak";
        write_file(oob_pak, std::vector<std::uint8_t>(100, 0x33));
        write_raw_index(oob_idx, index_40, {{"oob.dat", 5963, 101, 0}}, 0x88);
        require_synth(expect_error(ErrorCode::entry_out_of_range, [&] { (void)eagls::Archive::open(oob_idx, oob_pak); }));
    }

    // 八、非 UTF-8 名称必须被拒绝。
    const auto [idx_bad, pak_bad] =
        write_synthetic_archive(scratch / "utf40", index_40, {{std::string("\xFF\x7F", 2), data_c, 0}}, 0x9);
    require_synth(expect_error(ErrorCode::invalid_entry, [&] { (void)eagls::Archive::open(idx_bad, pak_bad); }));
}

void verify_local(const fs::path& root, const fs::path& scratch) {
    using eagls::ErrorCode;
    const auto dat = read_sample(root / "SCPACK" / "00CGFlag.dat");
    const auto script = eagls::decode_dat(dat);
    require(script.seed == 128 && script.decrypted.size() == 3780 && script.labels.size() == 1);
    require(std::string(script.decrypted.begin() + 3600, script.decrypted.begin() + 3606) == "$Start");
    require(script.decrypted[3778] == 0 && script.decrypted.back() == dat.back());
    require(std::equal(dat.begin(), dat.begin() + 3600, script.decrypted.begin()));
    auto bad_label = dat;
    std::fill(bad_label.begin() + 32, bad_label.begin() + 36, std::uint8_t{255});
    require(expect_error(ErrorCode::invalid_content, [&] { (void)eagls::decode_dat(bad_label); }));
    auto bad_nul = dat;
    bad_nul[bad_nul.size() - 2] ^= 1;
    require(expect_error(ErrorCode::invalid_content, [&] { (void)eagls::decode_dat(bad_nul); }));
    require(expect_error(ErrorCode::invalid_content, [&] { (void)eagls::decode_dat(script.decrypted); }));
    const auto gr = read_sample(root / "CGPACK" / "a00.gr");
    const auto image = eagls::decode_gr(gr);
    require(image.width == 800 && image.height == 600 && image.bits_per_pixel == 24);
    require(image.bitmap.size() == 1440054 && image.original_file_size == 1440056);
    require(expect_error(ErrorCode::resource_limit, [&] { (void)eagls::decode_gr(gr, 1024); }));
    auto truncated = gr;
    truncated.resize(5964);
    truncated.back() = gr.back();
    require(expect_error(ErrorCode::truncated_data, [&] { (void)eagls::decode_gr(truncated); }));
    auto bad_magic = gr;
    bad_magic[1] ^= 1;
    require(expect_error(ErrorCode::invalid_content, [&] { (void)eagls::decode_gr(bad_magic); }));
    const auto rgba = eagls::decode_gr(read_sample(root / "CGPACK" / "FBLACK.gr"));
    require(rgba.bits_per_pixel == 32 && rgba.bitmap.size() == 1920054);
    require(rgba.original_file_size == 1440054 && rgba.bitmap.back() == 255);
    const auto padded = eagls::decode_gr(read_sample(root / "CGPACK" / "blackt.gr"));
    require(padded.bitmap.size() == 36918 && padded.compressed_bytes_consumed < 5963);

    // GR 编码往返：解码出的规范化 BMP 重新编码后必须完整还原（两条压缩路径）。
    const auto check_gr_roundtrip = [](const fs::path& path) {
        const auto source = eagls::decode_gr(read_sample(path));
        const auto again = eagls::decode_gr(eagls::encode_gr(source.bitmap));
        require(again.bitmap == source.bitmap);
        require(again.width == source.width && again.height == source.height &&
                again.bits_per_pixel == source.bits_per_pixel);
        require(again.seed == 0);
        eagls::GrEncodeOptions store_options;
        store_options.store = true;
        require(eagls::decode_gr(eagls::encode_gr(source.bitmap, store_options)).bitmap == source.bitmap);
    };
    check_gr_roundtrip(root / "CGPACK" / "a00.gr");
    check_gr_roundtrip(root / "CGPACK" / "FBLACK.gr");
    check_gr_roundtrip(root / "CGPACK" / "blackt.gr");

    // PAK 回包：定位真实归档样本（与 EXE 样本相同的两种目录布局）。
    fs::path scpack_idx;
    for (const auto& candidate : {root / "Script" / "SCPACK.idx",
                                  root.parent_path() / "fixtures" / "local" / "yanyun2" / "Script" / "SCPACK.idx"}) {
        if (fs::exists(candidate)) {
            scpack_idx = candidate;
            break;
        }
    }
    require(!scpack_idx.empty());
    const fs::path scpack_pak = scpack_idx.parent_path() / "SCPACK.pak";
    const eagls::Archive original = eagls::Archive::open(scpack_idx, scpack_pak);
    const std::size_t entry_count = original.entries().size();

    // 每次重建 scratch 目录，避免旧输出触发覆盖保护。
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch / "empty", ec);
    require(!ec);
    fs::create_directories(scratch / "self", ec);
    require(!ec);

    // 一、没有替换文件：两个输出都必须与原文件逐字节一致。
    const auto empty_result = eagls::repack_archive(scpack_idx, scpack_pak, scratch / "empty",
                                                    scratch / "repack-empty" / "SCPACK.idx",
                                                    scratch / "repack-empty" / "SCPACK.pak", true);
    require(empty_result.replaced == 0 && empty_result.entries == entry_count);
    require(read_sample(scratch / "repack-empty" / "SCPACK.idx") == read_sample(scpack_idx));
    require(read_sample(scratch / "repack-empty" / "SCPACK.pak") == read_sample(scpack_pak));

    // 二、用归档自身的内容做替换：字节不变，仍然逐字节一致。
    const auto flag = original.find("00CGFlag.dat");
    require(flag.has_value());
    original.extract({*flag}, scratch / "self");
    const auto self_result = eagls::repack_archive(scpack_idx, scpack_pak, scratch / "self",
                                                   scratch / "repack-self" / "SCPACK.idx",
                                                   scratch / "repack-self" / "SCPACK.pak", true);
    require(self_result.replaced == 1);
    require(read_sample(scratch / "repack-self" / "SCPACK.idx") == read_sample(scpack_idx));
    require(read_sample(scratch / "repack-self" / "SCPACK.pak") == read_sample(scpack_pak));

    // 三、替换成不同内容：条目大小与内容更新，未替换条目保持可读且不变。
    std::vector<std::uint8_t> replacement(100);
    for (std::size_t i = 0; i < replacement.size(); ++i) {
        replacement[i] = static_cast<std::uint8_t>((i * 31 + 7) % 251);
    }
    {
        std::ofstream file(scratch / "self" / "00CGFlag.dat", std::ios::binary | std::ios::trunc);
        require(file.good());
        file.write(reinterpret_cast<const char*>(replacement.data()),
                   static_cast<std::streamsize>(replacement.size()));
        require(file.good());
    }
    const auto modified_result = eagls::repack_archive(scpack_idx, scpack_pak, scratch / "self",
                                                       scratch / "repack-modified" / "SCPACK.idx",
                                                       scratch / "repack-modified" / "SCPACK.pak", true);
    require(modified_result.replaced == 1 && modified_result.entries == entry_count);
    const eagls::Archive rebuilt = eagls::Archive::open(scratch / "repack-modified" / "SCPACK.idx",
                                                        scratch / "repack-modified" / "SCPACK.pak");
    const auto rebuilt_flag = rebuilt.find("00CGFlag.dat");
    require(rebuilt_flag.has_value());
    require(rebuilt.read_entry(*rebuilt_flag) == replacement);
    require(rebuilt.layout().exact());
    const auto other = original.find("00CP.dat");
    const auto rebuilt_other = rebuilt.find("00CP.dat");
    require(other.has_value() && rebuilt_other.has_value());
    require(rebuilt.read_entry(*rebuilt_other) == original.read_entry(*other));
    require(original.index_format().record_size == 28);

    // SAIMIN（40 字节记录布局）回归：与 yanyun2 并列的第二个实测样本，40 字节记录、
    // 24 字节名称区、64 位偏移。样本只读；没有该样本时跳过，所有输出写入 scratch。
    const fs::path saimin_root = root.parent_path() / "fixtures" / "local" / "OLE-M" / "SAIMIN";
    if (fs::exists(saimin_root / "Script" / "SCPACK.idx")) {
        const fs::path saimin_idx = saimin_root / "Script" / "SCPACK.idx";
        const fs::path saimin_pak = saimin_root / "Script" / "SCPACK.pak";
        const eagls::Archive saimin = eagls::Archive::open(saimin_idx, saimin_pak);
        require(saimin.index_format().record_size == 40);
        require(saimin.index_format().name_size == 24);
        require(saimin.index_format().offset_width == 8);
        require(saimin.index_format().slot_count == 10000);
        require(saimin.seed() == 0xFFFFFFA6u);
        require(saimin.entries().size() == 140);
        require(saimin.layout().exact());
        // 22 字节名称反例：旧 20 字节名称区装不下，必须按 24 字节名称区读取。
        const auto long_name = saimin.find("02Char_Move_Single.dat");
        require(long_name.has_value());
        require(saimin.entries()[*long_name].name.size() == 22);
        require(saimin.read_entry(*long_name).size() == 4706);

        // 全量提取：140 个条目、总字节数与数据文件一致（exact 布局）。
        const fs::path saimin_out = scratch / "saimin-extract";
        fs::remove_all(saimin_out, ec);
        const eagls::ExtractResult extracted = saimin.extract_all(saimin_out);
        require(extracted.files_written == 140 && extracted.bytes_written == saimin.pak_size());

        // 无修改回包：两个输出必须与原文件逐字节一致。
        const eagls::RepackResult saimin_repack = eagls::repack_archive(
            saimin_idx, saimin_pak, scratch / "empty", scratch / "saimin-repack" / "SCPACK.idx",
            scratch / "saimin-repack" / "SCPACK.pak", true);
        require(saimin_repack.replaced == 0 && saimin_repack.entries == 140);
        require(read_sample(scratch / "saimin-repack" / "SCPACK.idx") == read_sample(saimin_idx));
        require(read_sample(scratch / "saimin-repack" / "SCPACK.pak") == read_sample(saimin_pak));

        // 替换成空文件必须被拒绝：长度 0 是引擎的扫描终止标记。
        const fs::path saimin_zero_dir = scratch / "saimin-zero";
        fs::remove_all(saimin_zero_dir, ec);
        fs::create_directories(saimin_zero_dir, ec);
        require(!ec);
        {
            std::ofstream empty_file(saimin_zero_dir / "02Char_Move_Single.dat", std::ios::binary | std::ios::trunc);
            require(empty_file.good());
        }
        require(expect_error(ErrorCode::invalid_content, [&] {
            (void)eagls::repack_archive(saimin_idx, saimin_pak, saimin_zero_dir,
                                        scratch / "saimin-zero-out" / "SCPACK.idx",
                                        scratch / "saimin-zero-out" / "SCPACK.pak", true);
        }));
        require(!fs::exists(scratch / "saimin-zero-out" / "SCPACK.idx"));

        // 替换成新内容：重新打开后新内容可读，未替换条目保持不变。
        const fs::path saimin_patch_dir = scratch / "saimin-patch";
        fs::remove_all(saimin_patch_dir, ec);
        fs::create_directories(saimin_patch_dir, ec);
        require(!ec);
        const std::vector<std::uint8_t> patched(64, 0x6C);
        {
            std::ofstream file(saimin_patch_dir / "02Char_Move_Single.dat", std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(patched.data()), static_cast<std::streamsize>(patched.size()));
            require(file.good());
        }
        const eagls::RepackResult patched_result =
            eagls::repack_archive(saimin_idx, saimin_pak, saimin_patch_dir, scratch / "saimin-patch-out" / "SCPACK.idx",
                                  scratch / "saimin-patch-out" / "SCPACK.pak", true);
        require(patched_result.replaced == 1 && patched_result.entries == 140);
        const eagls::Archive rebuilt_saimin = eagls::Archive::open(scratch / "saimin-patch-out" / "SCPACK.idx",
                                                                   scratch / "saimin-patch-out" / "SCPACK.pak");
        const auto rebuilt_long = rebuilt_saimin.find("02Char_Move_Single.dat");
        require(rebuilt_long.has_value());
        require(rebuilt_saimin.read_entry(*rebuilt_long) == patched);
        require(rebuilt_saimin.layout().exact());
        const auto saimin_rebuilt_flag = rebuilt_saimin.find("00CGFlag.dat");
        const auto saimin_original_flag = saimin.find("00CGFlag.dat");
        require(saimin_rebuilt_flag.has_value() && saimin_original_flag.has_value());
        require(rebuilt_saimin.read_entry(*saimin_rebuilt_flag) == saimin.read_entry(*saimin_original_flag));

        // 另外两个归档只校验索引层（数据文件过大，不做全量提取）。
        const eagls::Archive cgpack =
            eagls::Archive::open(saimin_root / "CG" / "CGPACK.idx", saimin_root / "CG" / "CGPACK.pak");
        require(cgpack.seed() == 0x45u && cgpack.entries().size() == 5185 && cgpack.layout().exact());
        const eagls::Archive wavepack =
            eagls::Archive::open(saimin_root / "Wave" / "WAVEPACK.idx", saimin_root / "Wave" / "WAVEPACK.pak");
        require(wavepack.seed() == 0x46u && wavepack.entries().size() == 5443 && wavepack.layout().exact());
    }

    // EAGLS.exe 中文显示补丁：真实样本 6 处位置全局唯一，内存应用只改 6 字节。
    // 样本可能位于 root 之内或相邻的游戏目录，两种布局都尝试。
    fs::path exe_path;
    bool found_exe = false;
    for (const auto& candidate :
         {root / "EAGLS.exe",
          root.parent_path() / "fixtures" / "local" / "yanyun2" / "EAGLS.exe"}) {
        if (fs::exists(candidate)) {
            exe_path = candidate;
            found_exe = true;
            break;
        }
    }
    require(found_exe);
    const auto exe = read_sample(exe_path);
    const auto exe_report = eagls::inspect_exe_patch(exe);
    require(exe_report.sites.size() == 6 && exe_report.state == eagls::ExePatchState::original);
    require(exe_report.sites.front().file_offset == 0x1909 &&
            exe_report.sites.front().patched_bytes == std::vector<std::uint8_t>{0xFE});
    require(exe_report.sites.back().file_offset == 0xA118);
    const auto exe_patched = eagls::apply_exe_patch(exe);
    require(exe_patched.size() == exe.size());
    std::size_t exe_changed = 0;
    for (std::size_t i = 0; i < exe.size(); ++i) {
        if (exe[i] != exe_patched[i]) ++exe_changed;
    }
    require(exe_changed == 6);
    require(eagls::inspect_exe_patch(exe_patched).state == eagls::ExePatchState::patched);
    require(expect_error(ErrorCode::invalid_content,
                         [&] { (void)eagls::apply_exe_patch(exe_patched); }));

    // 中文排版模式：原版直出与旧 GBK 补丁升级必须相同，真实字体槽位与代码块共 26 处。
    const auto typography = eagls::ExePatchTarget::gbk_zh_cn;
    const auto typography_report = eagls::inspect_exe_patch(exe, typography);
    require(typography_report.sites.size() == 26);
    require(eagls::inspect_exe_patch(exe_patched, typography).state == eagls::ExePatchState::base_patched);
    const auto layout_patched = eagls::apply_exe_patch(exe, typography);
    require(layout_patched.size() == exe.size());
    require(eagls::apply_exe_patch(exe_patched, typography) == layout_patched);
    require(eagls::inspect_exe_patch(layout_patched, typography).state == eagls::ExePatchState::patched);
    std::vector<bool> permitted(exe.size(), false);
    for (const auto& site : typography_report.sites) {
        for (std::size_t i = 0; i < site.patched_bytes.size(); ++i) permitted.at(site.file_offset + i) = true;
    }
    for (std::size_t i = 0; i < exe.size(); ++i) require(permitted[i] || exe[i] == layout_patched[i]);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        try {
            // scratch 放在测试二进制目录下，所有新输出都不落在样本目录里。
            const fs::path scratch = fs::absolute(fs::u8path(argv[0])).parent_path() / "local-scratch";
            verify_local(fs::u8path(argv[1]), scratch);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }

    using eagls::ErrorCode;
    // 无需游戏资源的边界检查与合成往返；真实样本回归在 argv[1] 给出时执行。
    bool ok = verify_synthetic_encode();
    try {
        // 合成归档用例的临时文件写在二进制目录下（CTest 在构建目录内运行），不接触游戏样本。
        const fs::path exe_dir = fs::absolute(fs::u8path(argv[0])).parent_path();
        verify_synthetic_archive_layouts(exe_dir / "archive-synth-scratch");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        ok = false;
    }
    ok &= expect_error(ErrorCode::truncated_data, [] { (void)eagls::decode_dat({}); });
    ok &= expect_error(ErrorCode::truncated_data, [] {
        (void)eagls::decode_dat(std::vector<std::uint8_t>(3601));
    });
    ok &= expect_error(ErrorCode::truncated_data, [] { (void)eagls::decode_gr({}); });
    ok &= expect_error(ErrorCode::truncated_data, [] {
        (void)eagls::decode_gr(std::vector<std::uint8_t>(5963));
    });
    ok &= expect_error(ErrorCode::resource_limit, [] {
        (void)eagls::decode_gr(std::vector<std::uint8_t>(5964), 53);
    });
    ok &= expect_error(ErrorCode::invalid_content, [] {
        (void)eagls::decode_dat(std::vector<std::uint8_t>(3602));
    });
    ok &= expect_error(ErrorCode::invalid_content, [] {
        (void)eagls::decode_gr(std::vector<std::uint8_t>(5964));
    });
    if (!ok) std::cerr << "内容解码边界检查失败。\n";
    return ok ? 0 : 1;
}
