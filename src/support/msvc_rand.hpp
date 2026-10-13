#pragma once

#include <cstdint>

namespace eagls::detail {

// 复现 MSVC C 运行库 rand() 的线性同余生成器：
//   state = state * 214013 + 2531011      （uint32 回绕）
//   返回 (state >> 16) & 0x7FFF
//
// 种子必须按无符号 32 位整数处理，种子本身不会作为第一个输出。
// 已知答案：种子 1 依次产生 41、18467、6334、26500、19169……
class MsvcRand {
public:
    explicit constexpr MsvcRand(std::uint32_t seed) noexcept : state_(seed) {}

    // 返回下一个 0..32767 的伪随机数。
    constexpr int next() noexcept {
        state_ = state_ * 214013u + 2531011u;
        return static_cast<int>((state_ >> 16) & 0x7FFFu);
    }

private:
    std::uint32_t state_;
};

}  // namespace eagls::detail
