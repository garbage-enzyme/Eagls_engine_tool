#pragma once

#include <string>
#include <vector>

namespace cli {

// 执行 "eagls pak ..." 命令。args 是 "pak" 之后的全部参数（UTF-8 编码）。
// 返回进程退出码：0 成功；1 运行失败（归档错误、条目未命中、读写失败）；
// 2 用法错误（缺少参数、未知子命令或选项）。
int run_pak_command(const std::vector<std::string>& args);

}  // namespace cli
