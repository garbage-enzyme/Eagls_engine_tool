#include "codec/pe_image.hpp"

#include <eagls/error.hpp>

#include "support/bytes.hpp"

#include <cstddef>
#include <string>

namespace eagls::detail {
namespace {

// 复制节名并在末尾附加 NUL，供错误信息使用；非 ASCII 字节替换为 ?。
std::string section_name(const std::uint8_t* raw) {
    std::string name;
    for (int i = 0; i < 8 && raw[i] != 0; ++i) {
        const char c = static_cast<char>(raw[i]);
        name.push_back((c >= 0x20 && c <= 0x7E) ? c : '?');
    }
    return name;
}

}  // namespace

bool PeSection::executable() const noexcept {
    // IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE
    return (characteristics & (0x00000020u | 0x20000000u)) != 0;
}

std::size_t PeSection::data_size() const noexcept {
    const std::size_t raw = raw_size;
    return virtual_size == 0 ? raw : (raw < virtual_size ? raw : virtual_size);
}

std::uint32_t PeImage::file_offset_to_va(const PeSection& section,
                                         std::size_t file_offset) const noexcept {
    const std::uint32_t delta = static_cast<std::uint32_t>(file_offset - section.raw_offset);
    return image_base + section.virtual_address + delta;
}

PeImage parse_pe32(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 0x40 || bytes[0] != 'M' || bytes[1] != 'Z') {
        throw Error(ErrorCode::invalid_content, "不是可执行文件（缺少 MZ 头）。");
    }
    const std::uint32_t pe_offset = read_u32_le(bytes.data() + 0x3C);
    if (pe_offset > bytes.size() || bytes.size() - pe_offset < 24) {
        throw Error(ErrorCode::truncated_data, "PE 头超出文件末尾。");
    }
    const std::uint8_t* pe = bytes.data() + pe_offset;
    if (pe[0] != 'P' || pe[1] != 'E' || pe[2] != 0 || pe[3] != 0) {
        throw Error(ErrorCode::invalid_content, "缺少 PE 签名。");
    }
    if (read_u16_le(pe + 4) != 0x014Cu) {
        throw Error(ErrorCode::invalid_content,
                    "仅支持 32 位 x86 可执行文件（IMAGE_FILE_MACHINE_I386）。");
    }
    const std::uint16_t section_count = read_u16_le(pe + 6);
    const std::uint16_t optional_size = read_u16_le(pe + 20);
    if (section_count == 0) {
        throw Error(ErrorCode::invalid_content, "PE 没有节表。");
    }
    if (optional_size < 96 || bytes.size() - pe_offset < static_cast<std::size_t>(24) + optional_size) {
        throw Error(ErrorCode::truncated_data, "可选头超出文件末尾。");
    }
    const std::uint8_t* optional = pe + 24;
    if (read_u16_le(optional) != 0x010Bu) {
        throw Error(ErrorCode::invalid_content, "可选头不是 PE32（0x10B）。");
    }

    PeImage image;
    image.image_base = read_u32_le(optional + 28);

    const std::size_t section_table = pe_offset + 24 + optional_size;
    if (bytes.size() - section_table < static_cast<std::size_t>(section_count) * 40) {
        throw Error(ErrorCode::truncated_data, "节表超出文件末尾。");
    }
    image.sections.reserve(section_count);
    for (std::uint16_t i = 0; i < section_count; ++i) {
        const std::uint8_t* entry = bytes.data() + section_table + static_cast<std::size_t>(i) * 40;
        PeSection section;
        section.name = section_name(entry);
        section.virtual_size = read_u32_le(entry + 8);
        section.virtual_address = read_u32_le(entry + 12);
        section.raw_size = read_u32_le(entry + 16);
        section.raw_offset = read_u32_le(entry + 20);
        section.characteristics = read_u32_le(entry + 36);
        // 磁盘区必须完整落在文件内；不做节重叠或对齐的额外校验（纯本地工具）。
        if (section.raw_size != 0) {
            if (section.raw_offset > bytes.size() ||
                section.raw_size > bytes.size() - section.raw_offset) {
                throw Error(ErrorCode::truncated_data,
                            "节 " + section.name + " 的数据超出文件末尾。");
            }
        } else if (section.raw_offset > bytes.size()) {
            throw Error(ErrorCode::truncated_data, "节 " + section.name + " 的偏移超出文件末尾。");
        }
        image.sections.push_back(std::move(section));
    }
    return image;
}

}  // namespace eagls::detail
