#include <eagls/version.hpp>

#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    // 使用显式检查，避免 Release 构建关闭 assert 后出现空测试。
    if (argc != 2) {
        std::cerr << "测试需要传入 CMake 项目版本。\n";
        return 1;
    }

    const auto actual = eagls::version();
    if (actual.empty() || actual != std::string_view{argv[1]}) {
        std::cerr << "核心版本与构建配置不一致。\n";
        return 1;
    }

    static_assert(noexcept(eagls::version()));
    return 0;
}
