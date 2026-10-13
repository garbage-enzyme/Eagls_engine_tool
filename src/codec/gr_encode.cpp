// GR 编码：把标准 24/32 位 BI_RGB BMP 压成引擎可读的 .gr。
//
// 容器格式与 decode_gr 的读取口径一一对应（证据见 docs/eagls-content.md）：
//   [前 5963 字节：LZSS 压缩流，整体异或][压缩流可能继续、不再异或][末尾 1 字节种子]
// 压缩流不足 5963 字节时补零；LZSS 位流格式由 content.cpp 的 GrLzssReader 定义。

#include <eagls/content.hpp>
#include <eagls/error.hpp>

#include "codec/gr_cipher.hpp"
#include "support/bytes.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace eagls {
namespace {

// 与 decode_gr 相同的图像约束；不做额外猜测，长度多一字节少一字节都报错。
// 不使用 bfSize（引擎忽略它，部分历史文件填的是旧值），按像素布局计算应有长度。
void validate_bitmap(const std::vector<std::uint8_t>& bitmap) {
    if (bitmap.size() < 54) {
        throw Error(ErrorCode::invalid_content, "BMP 小于 54 字节的文件头，无法编码。");
    }
    const auto* header = bitmap.data();
    if (header[0] != 'B' || header[1] != 'M' || detail::read_u32_le(header + 14) != 40 ||
        detail::read_u32_le(header + 10) != 54 || detail::read_u16_le(header + 26) != 1 ||
        detail::read_u32_le(header + 30) != 0) {
        throw Error(ErrorCode::invalid_content,
                    "只支持像素偏移为 54 的 40 字节 BITMAPINFOHEADER、BI_RGB 的 BMP。");
    }
    const auto width = detail::read_i32_le(header + 18);
    const auto height = detail::read_i32_le(header + 22);
    const auto bits = detail::read_u16_le(header + 28);
    if ((bits != 24 && bits != 32) || width <= 0 || height == 0) {
        throw Error(ErrorCode::invalid_content, "只支持 24/32 位、宽为正且高非零的 BMP。");
    }
    const auto rows = static_cast<std::uint64_t>(height < 0 ? -static_cast<std::int64_t>(height) : height);
    const std::uint64_t stride = ((static_cast<std::uint64_t>(width) * bits + 31) / 32) * 4;
    const std::uint64_t expected = 54 + stride * rows;
    if (bitmap.size() != expected) {
        throw Error(ErrorCode::invalid_content,
                    "BMP 长度 " + std::to_string(bitmap.size()) + " 与像素布局应有的 " +
                        std::to_string(expected) + " 字节不符；不接受截断或带填充的文件。");
    }
}

// 全字面量流：每控制字节 0xFF 后跟 8 个字节，末尾不足 8 个补零。
// 解码器读满图像字节后即停止，补零不会被使用。
std::vector<std::uint8_t> store_stream(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out;
    out.reserve(data.size() + data.size() / 8 + 2);
    for (std::size_t position = 0; position < data.size(); position += 8) {
        out.push_back(0xFF);
        for (std::size_t i = 0; i < 8; ++i) {
            out.push_back(position + i < data.size() ? data[position + i] : 0);
        }
    }
    return out;
}

// LZSS 压缩：产出与 GrLzssReader 兼容的位流。
//
// 解码器的窗口模型：4096 字节环形窗口初始全零、写指针从 0xFEE 开始、
// 每控制字节 8 个项目、低位优先、引用为 [12 位偏移][4 位长度-3]，长度 3..18。
// 编码器不需要复现窗口内容——匹配比较直接在原始字节上进行即可：
// 历史窗口槽位的内容等于对应位置的历史字节（写指针偏移恒为 4078），
// 而重叠引用（距离小于长度）在原始数据上等价于「周期性相等」。
class LzssEncoder {
public:
    explicit LzssEncoder(const std::vector<std::uint8_t>& data) : data_(data) {}

    std::vector<std::uint8_t> run() {
        std::vector<std::uint8_t> out;
        out.reserve(data_.size() + data_.size() / 8 + 16);

        // 经典哈希链：head 是每个 3 字节散列最近出现的位置，
        // prev 按位置低 12 位串起同散列的历史位置。
        std::array<std::ptrdiff_t, hash_size> head{};
        std::array<std::ptrdiff_t, window_size> prev{};
        head.fill(-1);
        prev.fill(-1);

        // 把一个位置登记进哈希链；末尾不足 3 字节的位置不可能作为匹配源，直接跳过。
        const auto insert = [&](std::size_t index) {
            if (index + min_length > data_.size()) {
                return;
            }
            const std::uint32_t hash = hash3(data_.data() + index);
            prev[index & (window_size - 1)] = head[hash];
            head[hash] = static_cast<std::ptrdiff_t>(index);
        };

        std::uint8_t control = 0;
        unsigned bits_used = 0;
        std::size_t control_position = out.size();
        out.push_back(0);

        std::size_t position = 0;
        while (position < data_.size()) {
            const std::size_t max_match = std::min<std::size_t>(max_length, data_.size() - position);
            std::size_t best_length = 0;
            std::size_t best_distance = 0;
            if (max_match >= min_length) {
                std::ptrdiff_t candidate = head[hash3(data_.data() + position)];
                int chain = 0;
                while (candidate >= 0 && chain < max_chain) {
                    const auto candidate_position = static_cast<std::size_t>(candidate);
                    const std::size_t distance = position - candidate_position;
                    if (distance > window_limit) {
                        break;  // 链按位置降序，更早的候选只会更远
                    }
                    std::size_t length = 0;
                    while (length < max_match &&
                           data_[candidate_position + length] == data_[position + length]) {
                        ++length;
                    }
                    if (length > best_length) {
                        best_length = length;
                        best_distance = distance;
                        if (best_length == max_match) {
                            break;
                        }
                    }
                    candidate = prev[candidate_position & (window_size - 1)];
                    ++chain;
                }
            }

            if (best_length >= min_length) {
                // 引用：偏移是匹配源在解码器窗口中的位置（写指针减距离）。
                const unsigned offset =
                    static_cast<unsigned>((write_start + position - best_distance) & (window_size - 1));
                out.push_back(static_cast<std::uint8_t>(offset & 0xFFu));
                out.push_back(
                    static_cast<std::uint8_t>((((offset >> 8) & 0x0Fu) << 4) | (best_length - min_length)));
                for (std::size_t i = 0; i < best_length; ++i) {
                    insert(position + i);
                }
                position += best_length;
            } else {
                control |= static_cast<std::uint8_t>(1u << bits_used);
                out.push_back(data_[position]);
                insert(position);
                ++position;
            }

            ++bits_used;
            if (bits_used == 8) {
                out[control_position] = control;
                control = 0;
                bits_used = 0;
                control_position = out.size();
                out.push_back(0);
            }
        }

        out[control_position] = control;
        if (bits_used == 0) {
            out.pop_back();  // 上一组刚好写满后新开的控制字节没有任何项目
        }
        return out;
    }

private:
    static constexpr std::size_t window_size = 4096;
    static constexpr unsigned write_start = 4078;
    static constexpr std::size_t window_limit = 4095;  // 12 位偏移可表示的最大距离
    static constexpr std::size_t min_length = 3;
    static constexpr std::size_t max_length = 18;
    static constexpr int max_chain = 64;
    static constexpr std::size_t hash_size = 4096;

    // 3 字节散列（12 位）：冲突只会让候选多比较几次，不影响正确性。
    static std::uint32_t hash3(const std::uint8_t* bytes) noexcept {
        const std::uint32_t value = (static_cast<std::uint32_t>(bytes[0]) << 16) |
                                    (static_cast<std::uint32_t>(bytes[1]) << 8) |
                                    static_cast<std::uint32_t>(bytes[2]);
        return (value * 2654435761u) >> 20;
    }

    const std::vector<std::uint8_t>& data_;
};

}  // namespace

std::vector<std::uint8_t> encode_gr(const std::vector<std::uint8_t>& bitmap, const GrEncodeOptions& options) {
    validate_bitmap(bitmap);
    std::vector<std::uint8_t> stream = options.store ? store_stream(bitmap) : LzssEncoder(bitmap).run();
    // 压缩流可能比加密前缀短（小图或全零数据），补零到 5963 字节再参与异或。
    if (stream.size() < content_format::gr_encrypted_size) {
        stream.resize(content_format::gr_encrypted_size, 0);
    }
    stream.push_back(options.seed);
    detail::xor_gr_prefix(stream.data(), content_format::gr_encrypted_size, options.seed);
    return stream;
}

}  // namespace eagls
