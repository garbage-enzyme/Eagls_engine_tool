#include <eagls/archive.hpp>
#include <eagls/content.hpp>
#include <eagls/error.hpp>
#include <eagls/exe_patch.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <iostream>
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
