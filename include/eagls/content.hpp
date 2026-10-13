#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace eagls {

namespace content_format {
inline constexpr std::size_t dat_header_size = 3600;
inline constexpr std::size_t gr_encrypted_size = 5963;
// 这是工具的默认资源上限，不是格式字段；库调用者可按需要传入其他上限。
inline constexpr std::size_t default_image_limit = 256 * 1024 * 1024;
}  // namespace content_format

struct DatLabel {
    std::size_t slot;
    std::string name;  // 原始字节，不推断编码
    std::uint32_t text_offset;
};

struct DatScript {
    // 保留标签表、正文终止 NUL 和原始种子；仅正文区完成异或。
    // 这是研究用的解密 DAT，不可直接回填游戏，也不能再次调用 decode_dat。
    std::vector<std::uint8_t> decrypted;
    std::vector<DatLabel> labels;
    std::uint8_t seed;
};

struct GrImage {
    // 标准 BI_RGB BMP；保留像素及其余头字段，修正 bfSize/biSizeImage，丢弃填充尾部。
    std::vector<std::uint8_t> bitmap;
    std::uint32_t width;
    std::int32_t height;
    std::uint16_t bits_per_pixel;
    std::uint32_t original_file_size;
    std::size_t compressed_bytes_consumed;
    std::uint8_t seed;
};

// 已验证的游戏格式；无文件操作，不转码，不修改传入的原始字节。
// 格式、截断和上限错误通过 eagls::Error 报告。
[[nodiscard]] DatScript decode_dat(const std::vector<std::uint8_t>& data);
[[nodiscard]] GrImage decode_gr(const std::vector<std::uint8_t>& data,
                               std::size_t max_image_bytes = content_format::default_image_limit);

// GR 编码选项。
struct GrEncodeOptions {
    // 为 true 时不做 LZSS 压缩，全部以字面量写出（文件更大，便于排查）。
    bool store = false;

    // 写入文件末尾的种子字节；只影响容器自洽，解码端会读取它。
    std::uint8_t seed = 0;
};

// 把 24/32 位 BI_RGB BMP 编码为 GR：LZSS 压缩（或全字面量）、压缩流不足
// 5963 字节时补零、前 5963 字节异或、末尾写入种子。
[[nodiscard]] std::vector<std::uint8_t> encode_gr(const std::vector<std::uint8_t>& bitmap,
                                                  const GrEncodeOptions& options = {});

enum class ContentAction { decrypt_dat, extract_dat_text, decode_gr, encode_gr };

// 单文件转换的选项。
struct ContentConvertOptions {
    // 为 true 时允许覆盖已存在的输出文件。
    bool overwrite = false;

    // 仅 encode_gr 使用：全字面量写出。
    bool gr_store = false;
};

// 单文件操作供 CLI/GUI 共用。先完成转换再写入，拒绝输入与输出为同一文件。
// 输出父目录不存在时创建；默认不覆盖，失败时不保留未写完的文件。
// 正文导出移除标签表、终止 NUL 和种子，但保留原编码和换行。
std::size_t convert_content_file(const std::filesystem::path& input,
                                 const std::filesystem::path& output,
                                 ContentAction action,
                                 const ContentConvertOptions& options = {});

}  // namespace eagls
