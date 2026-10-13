#include <eagls/content.hpp>
#include <eagls/error.hpp>

#include "support/bytes.hpp"
#include "support/msvc_rand.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace eagls {
namespace {
constexpr std::string_view key = "EAGLS_SYSTEM";

// 按引擎 sub_422530 的 Schrage 分解实现，避免有符号乘法溢出。
class GrRandom {
public:
    explicit GrRandom(std::uint8_t seed) : state_(seed ^ 123459876) {}

    unsigned next() {
        state_ = 48271 * (state_ % 44488) - 3399 * (state_ / 44488);
        if (state_ <= 0) {
            state_ += 2147483647;
        }
        return static_cast<unsigned>(static_cast<double>(state_) * 4.656612875245797e-10 * 256.0);
    }

private:
    std::int32_t state_;
};

// 逐字节读取，保留跨请求的引用状态，允许输出终点落在最后一个匹配项中间。
// 环形窗口初始为零，起点 0xFEE；位标志从低到高，1 为字面量。
class GrLzssReader {
public:
    explicit GrLzssReader(const std::vector<std::uint8_t>& input) : input_(input) {}

    std::uint8_t next() {
        std::uint8_t value;
        if (remaining_ == 0) {
            if (mask_ == 0) {
                control_ = take();
                mask_ = 1;
            }
            const bool literal = (control_ & mask_) != 0;
            mask_ = (mask_ << 1) & 0xFFu;
            if (literal) {
                value = take();
                put(value);
                return value;
            }
            const auto low = take();
            const auto high = take();
            offset_ = low | ((high & 0xF0u) << 4);
            remaining_ = (high & 0x0Fu) + 3;
        }
        value = window_[offset_];
        offset_ = (offset_ + 1) & 0xFFFu;
        --remaining_;
        put(value);
        return value;
    }

    std::size_t consumed() const noexcept { return cursor_; }

private:
    std::uint8_t take() {
        if (cursor_ == input_.size()) {
            throw Error(ErrorCode::truncated_data, "GR 压缩流在图像完成之前结束。");
        }
        return input_[cursor_++];
    }
    void put(std::uint8_t value) {
        window_[position_] = value;
        position_ = (position_ + 1) & 0xFFFu;
    }

    const std::vector<std::uint8_t>& input_;
    std::array<std::uint8_t, 4096> window_{};
    std::size_t cursor_ = 0;
    unsigned position_ = 4078;
    unsigned offset_ = 0;
    unsigned remaining_ = 0;
    unsigned control_ = 0;
    unsigned mask_ = 0;
};
}  // namespace

DatScript decode_dat(const std::vector<std::uint8_t>& data) {
    constexpr auto header_size = content_format::dat_header_size;
    if (data.size() < header_size + 2) {
        throw Error(ErrorCode::truncated_data, "DAT 缺少完整标签表、终止符或种子。");
    }
    DatScript result{data, {}, data.back()};
    // movsx byte -> srand：高位为 1 的种子必须先符号扩展，再作为 uint32 传入。
    const int signed_seed = result.seed < 128 ? result.seed : static_cast<int>(result.seed) - 256;
    detail::MsvcRand random(static_cast<std::uint32_t>(signed_seed));
    for (std::size_t i = header_size; i < data.size() - 1; i += 2) {
        result.decrypted[i] ^= static_cast<std::uint8_t>(key[static_cast<std::size_t>(random.next()) % key.size()]);
    }
    const auto text_begin = result.decrypted.begin() + header_size;
    const auto text_end = result.decrypted.end() - 2;
    if (*text_end != 0 || std::find(text_begin, text_end, 0) != text_end) {
        throw Error(ErrorCode::invalid_content, "DAT 正文的终止符不符合已验证格式；可能已解密或属于其他版本。");
    }
    const std::size_t text_size = data.size() - header_size - 2;
    for (std::size_t slot = 0; slot < 100; ++slot) {
        const auto* record = data.data() + slot * 36;
        if (record[0] == 0) {
            continue;
        }
        const auto* name_end = std::find(record, record + 32, 0);
        const std::size_t name_size = static_cast<std::size_t>(name_end - record);
        const auto offset = detail::read_u32_le(record + 32);
        if (offset >= text_size || name_size > text_size - offset - 1 ||
            result.decrypted[header_size + offset] != '$' ||
            !std::equal(record, name_end, result.decrypted.data() + header_size + offset + 1)) {
            throw Error(ErrorCode::invalid_content, "DAT 标签 " + std::to_string(slot) + " 未指向对应的正文标签。");
        }
        result.labels.push_back({slot, std::string(reinterpret_cast<const char*>(record), name_size), offset});
    }
    return result;
}

GrImage decode_gr(const std::vector<std::uint8_t>& data, std::size_t max_image_bytes) {
    if (data.size() <= content_format::gr_encrypted_size) {
        throw Error(ErrorCode::truncated_data, "GR 缺少 5963 字节加密前缀或末尾种子。");
    }
    if (max_image_bytes < 54) {
        throw Error(ErrorCode::resource_limit, "图像输出上限不足以容纳 BMP 头。");
    }
    std::vector<std::uint8_t> compressed(data.begin(), data.end() - 1);
    GrRandom random(data.back());
    for (std::size_t i = 0; i < content_format::gr_encrypted_size; ++i) {
        compressed[i] ^= static_cast<std::uint8_t>(key[random.next() % key.size()]);
    }
    GrLzssReader reader(compressed);
    GrImage result{};
    result.seed = data.back();
    result.bitmap.resize(54);
    for (auto& byte : result.bitmap) {
        byte = reader.next();
    }
    auto* header = result.bitmap.data();
    const auto width = detail::read_i32_le(header + 18);
    const auto height = detail::read_i32_le(header + 22);
    const auto bpp = detail::read_u16_le(header + 28);
    if (header[0] != 'B' || header[1] != 'M' || detail::read_u32_le(header + 14) != 40 ||
        detail::read_u32_le(header + 10) != 54 || detail::read_u16_le(header + 26) != 1 ||
        detail::read_u32_le(header + 30) != 0 || (bpp != 24 && bpp != 32) || width <= 0 || height == 0) {
        throw Error(ErrorCode::invalid_content, "GR 未解出支持的 24/32 位 BI_RGB BMP；不猜测其他变体。");
    }
    const auto rows = static_cast<std::uint64_t>(height < 0 ? -static_cast<std::int64_t>(height) : height);
    const std::uint64_t stride = ((static_cast<std::uint64_t>(width) * bpp + 31) / 32) * 4;
    const std::uint64_t pixels = stride * rows;
    const std::uint64_t total = 54 + pixels;
    if (total > max_image_bytes || total > 0xFFFFFFFFu) {
        throw Error(ErrorCode::resource_limit, "GR 图像尺寸超过输出上限。");
    }
    result.width = static_cast<std::uint32_t>(width);
    result.height = height;
    result.bits_per_pixel = bpp;
    result.original_file_size = detail::read_u32_le(header + 2);
    result.bitmap.resize(static_cast<std::size_t>(total));
    for (std::size_t i = 54; i < result.bitmap.size(); ++i) {
        result.bitmap[i] = reader.next();
    }
    result.compressed_bytes_consumed = reader.consumed();
    // 引擎忽略 bfSize，部分 32 位样本仍写入旧的 24 位大小；不能据此截断像素。
    detail::write_u32_le(result.bitmap.data() + 2, static_cast<std::uint32_t>(total));
    detail::write_u32_le(result.bitmap.data() + 34, static_cast<std::uint32_t>(pixels));
    return result;
}
}  // namespace eagls
