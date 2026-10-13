#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eagls::detail {

// 受控 JSON 子集解析器，仅服务于本项目文本清单的读写。
// 支持对象、数组、字符串（含 \uXXXX 与代理对）、数字、null、true/false；
// 严格解析：不允许注释、尾随逗号或未转义控制字符；错误信息带行号列号。
struct JsonValue {
    enum class Type { null_value, boolean, number, string, array, object };
    using Array = std::vector<JsonValue>;
    using Object = std::vector<std::pair<std::string, JsonValue>>;

    Type type = Type::null_value;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    Array array;
    Object object;

    // 对象按名取成员；非对象或不存在时返回 nullptr。
    [[nodiscard]] const JsonValue* find(std::string_view key) const;
};

// 解析一个完整 JSON 文档；失败时抛 eagls::Error(invalid_content)，附带行列信息。
[[nodiscard]] JsonValue parse_json(std::string_view text);

// 生成含首尾引号的 JSON 字符串字面量；非 ASCII 保留 UTF-8 原文不转义。
[[nodiscard]] std::string escape_json_string(std::string_view text);

}  // namespace eagls::detail
