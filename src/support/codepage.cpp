#include "support/codepage.hpp"

#include <eagls/error.hpp>

#include <cstddef>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace eagls::detail {

std::string codepage_to_utf8(std::string_view bytes, unsigned codepage) {
    if (bytes.empty()) {
        return {};
    }
    const auto size = static_cast<int>(bytes.size());
    const int wide_size =
        ::MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, bytes.data(), size, nullptr, 0);
    if (wide_size <= 0) {
        throw Error(ErrorCode::invalid_content,
                    "字节序列无法按代码页 " + std::to_string(codepage) + " 解码。");
    }
    std::vector<wchar_t> wide(static_cast<std::size_t>(wide_size));
    ::MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, bytes.data(), size, wide.data(), wide_size);
    const int utf8_size =
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size, nullptr, 0, nullptr, nullptr);
    if (utf8_size <= 0) {
        throw Error(ErrorCode::invalid_content, "文本转换为 UTF-8 失败。");
    }
    std::string result(static_cast<std::size_t>(utf8_size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size, result.data(), utf8_size, nullptr,
                          nullptr);
    return result;
}

std::string utf8_to_codepage(std::string_view utf8, unsigned codepage) {
    if (utf8.empty()) {
        return {};
    }
    const auto size = static_cast<int>(utf8.size());
    const int wide_size =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), size, nullptr, 0);
    if (wide_size <= 0) {
        throw Error(ErrorCode::invalid_content, "文本不是合法的 UTF-8。");
    }
    std::vector<wchar_t> wide(static_cast<std::size_t>(wide_size));
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), size, wide.data(), wide_size);
    // WC_ERR_INVALID_CHARS 仅适用于 UTF-7/UTF-8/Symbol 等少数代码页，对 CP932/936
    // 传入会直接以 ERROR_INVALID_FLAGS 失败；改用 WC_NO_BEST_FIT_CHARS + lpUsedDefaultChar：
    // 无法精确表示的字符会退回默认字符 '?' 并置位 used_default，据此严格报错而不是静默替换。
    BOOL used_default = FALSE;
    const int cp_size = ::WideCharToMultiByte(codepage, WC_NO_BEST_FIT_CHARS, wide.data(),
                                              wide_size, nullptr, 0, nullptr, &used_default);
    if (cp_size <= 0 || used_default) {
        throw Error(ErrorCode::invalid_content,
                    "文本含代码页 " + std::to_string(codepage) + " 无法表示的字符。");
    }
    std::string result(static_cast<std::size_t>(cp_size), '\0');
    ::WideCharToMultiByte(codepage, WC_NO_BEST_FIT_CHARS, wide.data(), wide_size, result.data(),
                          cp_size, nullptr, nullptr);
    return result;
}

}  // namespace eagls::detail
