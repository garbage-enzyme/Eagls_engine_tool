#include "archive/entry_name.hpp"

#include <array>
#include <cstddef>

namespace eagls::detail {

namespace {

char to_lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Windows 保留设备名：取第一个 '.' 之前的部分，忽略结尾空格，再忽略大小写比较。
bool is_reserved_device_name(std::string_view name) noexcept {
    std::string_view stem = name.substr(0, name.find('.'));
    while (!stem.empty() && stem.back() == ' ') {
        stem.remove_suffix(1);
    }

    constexpr std::array<std::string_view, 4> fixed_names{"con", "prn", "aux", "nul"};
    for (const auto reserved : fixed_names) {
        if (equals_ignoring_ascii_case(stem, reserved)) {
            return true;
        }
    }

    if (stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9') {
        const std::string_view prefix = stem.substr(0, 3);
        return equals_ignoring_ascii_case(prefix, "com") || equals_ignoring_ascii_case(prefix, "lpt");
    }
    return false;
}

}  // namespace

std::string fold_ascii_case(std::string_view name) {
    std::string folded(name);
    for (char& c : folded) {
        c = to_lower_ascii(c);
    }
    return folded;
}

bool equals_ignoring_ascii_case(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (to_lower_ascii(left[i]) != to_lower_ascii(right[i])) {
            return false;
        }
    }
    return true;
}

bool is_safe_file_name(std::string_view name) noexcept {
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            return false;
        }
        switch (c) {
            case '<': case '>': case ':': case '"': case '/': case '\\': case '|': case '?': case '*':
                return false;
            default:
                break;
        }
    }
    if (name.back() == ' ' || name.back() == '.') {
        return false;
    }
    return !is_reserved_device_name(name);
}

}  // namespace eagls::detail
