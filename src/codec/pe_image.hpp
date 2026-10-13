#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eagls::detail {

// 最小 PE32 解析：只读取补丁定位需要的映像基址与节表，不处理导入表、重定位或资源。
struct PeSection {
    std::string name;                   // 节名（去掉 NUL 填充，非可打印字符替换为 ?）
    std::uint32_t virtual_address = 0;  // RVA
    std::uint32_t virtual_size = 0;
    std::uint32_t raw_offset = 0;       // PointerToRawData
    std::uint32_t raw_size = 0;         // SizeOfRawData（已校验不越出文件）
    std::uint32_t characteristics = 0;

    // 含代码或可执行（IMAGE_SCN_CNT_CODE / IMAGE_SCN_MEM_EXECUTE）。
    [[nodiscard]] bool executable() const noexcept;
    // 节在文件中的有效数据长度：VirtualSize 非零时取两者较小值，去掉对齐填充。
    [[nodiscard]] std::size_t data_size() const noexcept;
};

struct PeImage {
    std::uint32_t image_base = 0;
    std::vector<PeSection> sections;

    // 文件偏移（必须位于 section 的数据区内）换算为虚拟地址。
    [[nodiscard]] std::uint32_t file_offset_to_va(const PeSection& section,
                                                  std::size_t file_offset) const noexcept;
};

// 校验 MZ/PE 签名、i386 机器类型与 PE32 可选头；不符时抛出 Error(invalid_content)，
// 头部、节表或节数据超出文件末尾时抛出 Error(truncated_data)。
[[nodiscard]] PeImage parse_pe32(const std::vector<std::uint8_t>& bytes);

}  // namespace eagls::detail
