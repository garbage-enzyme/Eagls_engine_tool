#include <eagls/content.hpp>
#include <eagls/error.hpp>

#include "support/file_io.hpp"

namespace eagls {

std::size_t convert_content_file(const std::filesystem::path& input,
                                 const std::filesystem::path& output,
                                 ContentAction action, const ContentConvertOptions& options) {
    namespace fs = std::filesystem;
    // equivalent 也识别硬链接；路径归一化覆盖输出尚未存在的情况。
    std::error_code ec;
    const bool same = fs::equivalent(input, output, ec);
    if (same || fs::weakly_canonical(input) == fs::weakly_canonical(output)) {
        throw Error(ErrorCode::output_exists, "输出不能与输入为同一文件，不允许就地修改游戏资源。");
    }
    detail::check_target_available(output, options.overwrite);
    const auto bytes = detail::read_binary_file(input);
    std::vector<std::uint8_t> decoded;
    switch (action) {
    case ContentAction::decode_gr:
        decoded = decode_gr(bytes).bitmap;
        break;
    case ContentAction::encode_gr: {
        GrEncodeOptions encode_options;
        encode_options.store = options.gr_store;
        decoded = encode_gr(bytes, encode_options);
        break;
    }
    case ContentAction::decrypt_dat:
        decoded = decode_dat(bytes).decrypted;
        break;
    case ContentAction::extract_dat_text: {
        const auto script = decode_dat(bytes);
        decoded.assign(script.decrypted.begin() + content_format::dat_header_size,
                       script.decrypted.end() - 2);
        break;
    }
    default:
        throw Error(ErrorCode::invalid_content, "未知的内容转换操作。");
    }
    detail::write_binary_file(output, decoded, options.overwrite);
    return decoded.size();
}

}  // namespace eagls
