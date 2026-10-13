// exe 补丁定位与应用的单元测试：构造带目标特征的最小 PE32 映像。
// 不需要游戏资源；真实 EAGLS.exe 的回归由 content_test.cpp 的本地样本分支负责。
#include <eagls/error.hpp>
#include <eagls/exe_patch.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

void put_u16(Bytes& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFF);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put_u32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Operation>
bool expect_error(eagls::ErrorCode expected, Operation operation) {
    try {
        operation();
    } catch (const eagls::Error& error) {
        return error.code() == expected;
    }
    return false;
}

// 合成映像布局（本例中文件偏移恰等于 RVA）：
//   0x0000 MZ/PE 头；0x0178 节表（1 个可执行节 .text）；0x0400 节数据；总大小 0x0C00。
constexpr std::size_t kPeOffset = 0x80;
constexpr std::size_t kSectionTable = kPeOffset + 24 + 0xE0;
constexpr std::size_t kTextRaw = 0x400;
constexpr std::size_t kTextSize = 0x800;
constexpr std::size_t kFileSize = kTextRaw + kTextSize;

// 前导字节判定函数（29 字节），与 kLeadPattern 一致。
const Bytes kLead = {
    0x8A, 0x44, 0x24, 0x04, 0x3C, 0x81, 0x72, 0x04, 0x3C, 0xA0, 0x76, 0x08, 0x3C, 0xE0, 0x72,
    0x0A, 0x3C, 0xFC, 0x77, 0x06, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0x33, 0xC0, 0xC3};
constexpr std::size_t kLeadOffset = 0x10;  // 命中点在节内的偏移

// 5 处字体字符集写入：mov byte ptr [esp+cell], 80h，cell 与 kCharsetCells 一致。
struct CharsetSite {
    std::size_t offset;  // 指令在节内的偏移
    std::uint8_t cell;   // [esp+disp] 的 disp
};
const CharsetSite kCharsetSites[] = {
    {0x80, 0x3F}, {0x90, 0x2F}, {0xA0, 0x3B}, {0xB0, 0x33}, {0xC0, 0x5B}};

Bytes build_image() {
    Bytes bytes(kFileSize, 0x90);  // 未用位置填 NOP（0x90）
    bytes[0] = 'M';
    bytes[1] = 'Z';
    put_u32(bytes, 0x3C, kPeOffset);
    bytes[kPeOffset] = 'P';
    bytes[kPeOffset + 1] = 'E';
    bytes[kPeOffset + 2] = 0;
    bytes[kPeOffset + 3] = 0;
    put_u16(bytes, kPeOffset + 4, 0x014C);        // IMAGE_FILE_MACHINE_I386
    put_u16(bytes, kPeOffset + 6, 1);            // 1 个节
    put_u16(bytes, kPeOffset + 20, 0x00E0);      // 可选头大小
    put_u16(bytes, kPeOffset + 24, 0x010B);      // PE32
    put_u32(bytes, kPeOffset + 24 + 28, 0x400000);  // ImageBase
    const std::size_t s = kSectionTable;
    const char name[8] = {'.', 't', 'e', 'x', 't', 0, 0, 0};
    std::copy(name, name + 8, bytes.begin() + s);
    put_u32(bytes, s + 8, static_cast<std::uint32_t>(kTextSize));   // VirtualSize
    put_u32(bytes, s + 12, 0x1000);                                 // VirtualAddress
    put_u32(bytes, s + 16, static_cast<std::uint32_t>(kTextSize));  // SizeOfRawData
    put_u32(bytes, s + 20, static_cast<std::uint32_t>(kTextRaw));   // PointerToRawData
    put_u32(bytes, s + 36, 0x60000020);                             // CODE | EXECUTE | READ

    std::copy(kLead.begin(), kLead.end(), bytes.begin() + kTextRaw + kLeadOffset);
    for (const auto& site : kCharsetSites) {
        const std::size_t o = kTextRaw + site.offset;
        bytes[o] = 0xC6;
        bytes[o + 1] = 0x44;
        bytes[o + 2] = 0x24;
        bytes[o + 3] = site.cell;
        bytes[o + 4] = 0x80;
    }
    return bytes;
}

}  // namespace

int main() {
    try {
        const Bytes original = build_image();

        // 定位：6 处、升序、原版状态。
        const auto report = eagls::inspect_exe_patch(original);
        require(report.sites.size() == 6, "补丁位置数不为 6。");
        require(report.state == eagls::ExePatchState::original, "原版映像状态判定错误。");
        require(report.sites.front().file_offset == kTextRaw + kLeadOffset + 9,
                "前导字节判定位置错误。");
        require(report.sites.front().instruction_va == 0x400000 + 0x1000 + kLeadOffset + 8,
                "前导字节判定指令 VA 错误。");
        require(std::is_sorted(report.sites.begin(), report.sites.end(),
                               [](const eagls::ExePatchSite& a, const eagls::ExePatchSite& b) {
                                   return a.file_offset < b.file_offset;
                               }),
                "补丁位置未按文件偏移升序。");

        // 应用：仅 6 字节变化，重新定位为已打补丁。
        const auto patched = eagls::apply_exe_patch(original);
        require(patched.size() == original.size(), "补丁改变了文件大小。");
        std::size_t changed = 0;
        for (std::size_t i = 0; i < original.size(); ++i) {
            if (original[i] != patched[i]) ++changed;
        }
        require(changed == 6, "补丁修改字节数不为 6。");
        require(patched[kTextRaw + kLeadOffset + 9] == 0xFE, "前导字节上界未改为 FE。");
        require(patched[kTextRaw + 0x80 + 4] == 0x86, "字体字符集未改为 86。");
        require(eagls::inspect_exe_patch(patched).state == eagls::ExePatchState::patched,
                "补丁后状态判定错误。");

        // 重复打补丁、混合状态必须拒绝。
        require(expect_error(eagls::ErrorCode::invalid_content,
                             [&] { (void)eagls::apply_exe_patch(patched); }),
                "重复打补丁未被拒绝。");
        Bytes mixed = original;
        mixed[kTextRaw + 0x80 + 4] = 0x86;
        require(eagls::inspect_exe_patch(mixed).state == eagls::ExePatchState::mixed,
                "混合状态判定错误。");
        require(expect_error(eagls::ErrorCode::invalid_content,
                             [&] { (void)eagls::apply_exe_patch(mixed); }),
                "混合状态未被拒绝。");

        // 非 PE、截断、非 i386、非 PE32、节数据越界。
        require(expect_error(eagls::ErrorCode::invalid_content,
                             [&] { (void)eagls::inspect_exe_patch(Bytes(64, 0)); }),
                "非 PE 文件未被拒绝。");
        {
            Bytes truncated = original;
            truncated.resize(kPeOffset + 10);
            require(expect_error(eagls::ErrorCode::truncated_data,
                                 [&] { (void)eagls::inspect_exe_patch(truncated); }),
                    "截断的 PE 头未被拒绝。");
        }
        {
            Bytes arm = original;
            put_u16(arm, kPeOffset + 4, 0x01C4);
            require(expect_error(eagls::ErrorCode::invalid_content,
                                 [&] { (void)eagls::inspect_exe_patch(arm); }),
                    "非 i386 文件未被拒绝。");
        }
        {
            Bytes pe32plus = original;
            put_u16(pe32plus, kPeOffset + 24, 0x020B);
            require(expect_error(eagls::ErrorCode::invalid_content,
                                 [&] { (void)eagls::inspect_exe_patch(pe32plus); }),
                    "PE32+ 文件未被拒绝。");
        }
        {
            Bytes out_of_range = original;
            put_u32(out_of_range, kSectionTable + 16, 0x100000);
            require(expect_error(eagls::ErrorCode::truncated_data,
                                 [&] { (void)eagls::inspect_exe_patch(out_of_range); }),
                    "节数据越界未被拒绝。");
        }

        // 特征缺失、重复、数量不符。
        {
            Bytes broken = original;
            broken[kTextRaw + kLeadOffset + 4] = 0x99;  // 破坏 cmp al,81h
            require(expect_error(eagls::ErrorCode::invalid_content,
                                 [&] { (void)eagls::inspect_exe_patch(broken); }),
                    "缺失前导字节判定特征未被拒绝。");
        }
        {
            Bytes doubled = original;
            std::copy(kLead.begin(), kLead.end(), doubled.begin() + kTextRaw + 0x300);
            require(expect_error(eagls::ErrorCode::invalid_content,
                                 [&] { (void)eagls::inspect_exe_patch(doubled); }),
                    "重复的前导字节判定特征未被拒绝。");
        }
        {
            Bytes missing = original;
            missing[kTextRaw + 0x90 + 4] = 0x81;  // 某处字符集写入不再是 80h
            require(expect_error(eagls::ErrorCode::invalid_content,
                                 [&] { (void)eagls::inspect_exe_patch(missing); }),
                    "字体字符集写入数量不符未被拒绝。");
        }
    } catch (const std::exception& error) {
        std::cerr << "exe 补丁单元测试失败：" << error.what() << '\n';
        return 1;
    }
    return 0;
}
