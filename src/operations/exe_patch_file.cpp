// exe 补丁的文件级操作：读取、定位与应用，供 CLI 与未来的 GUI 共用。
#include <eagls/exe_patch.hpp>

#include <eagls/error.hpp>

#include "support/file_io.hpp"

namespace eagls {
namespace fs = std::filesystem;

ExePatchReport inspect_exe_file(const fs::path& input, ExePatchTarget target) {
    return inspect_exe_patch(detail::read_binary_file(input), target);
}

ExePatchReport patch_exe_file(const fs::path& input, const fs::path& output, bool overwrite,
                              ExePatchTarget target) {
    // equivalent 也识别硬链接；路径归一化覆盖输出尚未存在的情况。
    std::error_code ec;
    const bool same = fs::equivalent(input, output, ec);
    if (same || fs::weakly_canonical(input) == fs::weakly_canonical(output)) {
        throw Error(ErrorCode::output_exists, "输出不能与输入为同一文件，不覆盖游戏目录中的原始文件。");
    }
    detail::check_target_available(output, overwrite);
    const auto bytes = detail::read_binary_file(input);
    const auto patched = apply_exe_patch(bytes, target);
    const auto report = inspect_exe_patch(bytes, target);
    detail::write_binary_file(output, patched, overwrite);
    return report;
}

}  // namespace eagls
