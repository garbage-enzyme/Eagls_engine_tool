#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace eagls::detail {
std::uint64_t regular_file_size(const std::filesystem::path& path, const std::string& role);
void check_target_available(const std::filesystem::path& target, bool overwrite);
std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path);
void write_binary_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes,
                       bool overwrite);
}  // namespace eagls::detail
