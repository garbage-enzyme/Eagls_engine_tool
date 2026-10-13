#include "support/file_io.hpp"
#include <eagls/error.hpp>

#include <fstream>
#include <limits>
#include <system_error>

namespace eagls::detail {
namespace fs = std::filesystem;
namespace {
std::string path_text(const fs::path& path) {
    return path.u8string();
}

// 系统错误只给出数值：error_code::message() 在 Windows 上使用本地代码页，
// 直接拼进 UTF-8 文本会产生乱码。常见原因已由下面的明确检查给出中文说明。
std::string error_code_text(const std::error_code& ec) {
    return "（系统错误码 " + std::to_string(ec.value()) + "）";
}

}  // namespace

std::uint64_t regular_file_size(const fs::path& path, const std::string& role) {
    std::error_code ec;
    const fs::file_status status = fs::status(path, ec);
    if (status.type() == fs::file_type::not_found) {
        throw Error(ErrorCode::io, role + "不存在：" + path_text(path));
    }
    if (ec) {
        throw Error(ErrorCode::io, role + "无法访问：" + path_text(path) + error_code_text(ec));
    }
    if (!fs::is_regular_file(status)) {
        throw Error(ErrorCode::io, role + "不是普通文件：" + path_text(path));
    }

    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        throw Error(ErrorCode::io, "无法获取" + role + "的大小：" + path_text(path) + error_code_text(ec));
    }
    return static_cast<std::uint64_t>(size);
}

void check_target_available(const fs::path& target, bool overwrite) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(target, ec);
    if (status.type() == fs::file_type::not_found) {
        return;
    }
    if (ec) {
        throw Error(ErrorCode::io, "无法检查输出位置：" + path_text(target) + error_code_text(ec));
    }
    if (!overwrite) {
        throw Error(ErrorCode::output_exists, "输出位置已存在，且未指定覆盖：" + path_text(target));
    }
    if (status.type() != fs::file_type::regular) {
        throw Error(ErrorCode::output_exists, "输出位置已存在且不是普通文件，无法覆盖：" + path_text(target));
    }
}

std::vector<std::uint8_t> read_binary_file(const fs::path& path) {
    const auto size = regular_file_size(path, "输入文件");
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()) ||
        size > std::vector<std::uint8_t>().max_size()) {
        throw Error(ErrorCode::resource_limit, "输入文件过大，无法读入内存。");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw Error(ErrorCode::io, "无法打开输入文件：" + path_text(path));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (input.gcount() != static_cast<std::streamsize>(size)) {
            throw Error(ErrorCode::truncated_data, "输入文件读取不完整：" + path_text(path));
        }
    }
    return bytes;
}

void write_binary_file(const fs::path& path, const std::vector<std::uint8_t>& bytes, bool overwrite) {
    check_target_available(path, overwrite);
    std::error_code ec;
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) throw Error(ErrorCode::io, "无法创建输出目录：" + path_text(path.parent_path()));
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw Error(ErrorCode::io, "无法创建输出文件：" + path_text(path));
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    output.close();
    if (!output) {
        fs::remove(path, ec);
        throw Error(ErrorCode::io, "写入输出文件失败：" + path_text(path));
    }
}
}  // namespace eagls::detail
