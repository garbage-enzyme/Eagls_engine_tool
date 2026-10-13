#include <eagls/version.hpp>

#include <iostream>
#include <string_view>

namespace {

void print_help() {
    std::cout
        << "EAGLS Engine Tool（重启开发中）\n\n"
        << "用法：eagls [--help | --version]\n\n"
        << "  -h, --help     显示帮助\n"
        << "  --version      显示核心版本\n\n"
        << "当前仅提供工程骨架，尚未实现资源读取、提取、转换或回包。\n";
}

}

int main(int argc, char* argv[]) {
    if (argc == 1) {
        print_help();
        return 0;
    }

    if (argc == 2) {
        const std::string_view argument{argv[1]};
        if (argument == "--help" || argument == "-h") {
            print_help();
            return 0;
        }
        if (argument == "--version") {
            std::cout << "eagls " << eagls::version() << '\n';
            return 0;
        }
    }

    // 未实现的命令与非法参数明确失败，不返回伪成功。
    std::cerr << "错误：参数无效或命令尚未实现。请使用 eagls --help。\n";
    return 2;
}
