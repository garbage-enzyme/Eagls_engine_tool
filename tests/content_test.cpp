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
std::vector<std::uint8_t> read_sample(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("本地内容样本不可读。");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void require(bool condition) {
    if (!condition) throw std::runtime_error("本地内容样本回归断言失败。");
}
void verify_local(const std::filesystem::path& root) {
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

    // EAGLS.exe 中文显示补丁：真实样本 6 处位置全局唯一，内存应用只改 6 字节。
    // 样本可能位于 root 之内或相邻的游戏目录，两种布局都尝试。
    std::filesystem::path exe_path;
    bool found_exe = false;
    for (const auto& candidate :
         {root / "EAGLS.exe",
          root.parent_path() / "fixtures" / "local" / "yanyun2" / "EAGLS.exe"}) {
        if (std::filesystem::exists(candidate)) {
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
            exe_report.sites.front().patched_value == 0xFE);
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
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        try {
            verify_local(std::filesystem::u8path(argv[1]));
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }

    using eagls::ErrorCode;
    // 无需游戏资源的边界检查；正确性由真实样本与独立 Python 校验负责。
    bool ok = expect_error(ErrorCode::truncated_data, [] { (void)eagls::decode_dat({}); });
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
