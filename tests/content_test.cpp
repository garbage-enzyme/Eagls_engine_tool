#include <eagls/content.hpp>
#include <eagls/error.hpp>

#include <cstdint>
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
}  // namespace

int main() {
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
