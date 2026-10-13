#include <eagls/text_map.hpp>

#include <eagls/error.hpp>

#include "codec/dat_cipher.hpp"
#include "support/bytes.hpp"
#include "support/codepage.hpp"
#include "support/json_mini.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eagls {
namespace {

constexpr std::size_t header_size = content_format::dat_header_size;

// 源文本编码：（SQUEEZ 2006）正文为 CP932；仅此一个已验收样本。
constexpr unsigned source_codepage = 932;

std::string ascii_lower(std::string value) {
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return value;
}

// 译文禁止出现的字节：引号破坏字符串配对；$ # & 是脚本结构标记（原文文本中从未出现）；
// 回车/换行/NUL 破坏正文行与终止结构。
bool is_forbidden_translation_char(char c) {
    return c == '"' || c == '$' || c == '#' || c == '&' || c == '\r' || c == '\n' || c == '\0';
}

void check_translation_text(const std::string& utf8, const std::string& where) {
    for (const char c : utf8) {
        if (is_forbidden_translation_char(c)) {
            throw Error(ErrorCode::invalid_content,
                        where + " 的译文包含禁止字符（\" $ # & 以及回车、换行或 NUL）。");
        }
    }
}

// —— 正文扫描 ——
// 与提取共用同一套规则：含非 ASCII 的引号字面量（可带 &ID 前缀）与 #说话者名。
// 规则由全量样本验证（见 docs/eagls-content.md）；不符合时明确报错而不是猜测。
struct ScannedEntry {
    std::uint32_t id;
    std::size_t begin;  // 文本内容首字节（开引号之后），绝对偏移
    std::size_t end;    // 闭引号位置，不含
};

struct ScannedSpeaker {
    std::size_t begin;  // 名字首字节
    std::size_t end;
};

struct ScannedScript {
    std::vector<ScannedEntry> entries;
    std::vector<ScannedSpeaker> speakers;
};

ScannedScript scan_script(const DatScript& script) {
    const auto& data = script.decrypted;
    if (data.size() < header_size + 2) {
        throw Error(ErrorCode::truncated_data, "DAT 数据不完整，无法扫描正文。");
    }
    const std::size_t body_begin = header_size;
    const std::size_t body_end = data.size() - 2;  // 终止 NUL 的位置
    ScannedScript scanned;
    std::size_t i = body_begin;
    while (i < body_end) {
        const auto c = data[i];
        if (c == '"') {
            std::size_t j = i + 1;
            while (j < body_end && data[j] != '"') {
                ++j;
            }
            if (j >= body_end) {
                throw Error(ErrorCode::invalid_content, "DAT 正文中的字符串引号不配对。");
            }
            bool has_non_ascii = false;
            for (std::size_t k = i + 1; k < j; ++k) {
                if (data[k] >= 0x80) {
                    has_non_ascii = true;
                    break;
                }
            }
            if (has_non_ascii) {
                // 开引号前紧邻"&数字"时记录为对话 ID（最多 6 位；样本 ID 为 0..16824）。
                std::uint32_t id = no_text_id;
                std::size_t q = i;
                std::size_t digits = 0;
                while (q > body_begin && digits < 6 && data[q - 1] >= '0' && data[q - 1] <= '9') {
                    --q;
                    ++digits;
                }
                if (digits > 0 && q > body_begin && data[q - 1] == '&') {
                    std::uint32_t value = 0;
                    for (std::size_t k = q; k < i; ++k) {
                        value = value * 10 + static_cast<std::uint32_t>(data[k] - '0');
                    }
                    id = value;
                }
                scanned.entries.push_back({id, i + 1, j});
            }
            i = j + 1;
        } else if (c == '#') {
            std::size_t j = i + 1;
            std::size_t length = 0;
            while (j < body_end && length < 12 && data[j] != '\r' && data[j] != '\n' &&
                   data[j] != '#' && data[j] != '&' && data[j] != '=') {
                ++j;
                ++length;
            }
            if (length == 0 || j >= body_end || (data[j] != '\r' && data[j] != '=')) {
                throw Error(ErrorCode::invalid_content, "DAT 正文中的 # 标记不符合说话者格式。");
            }
            scanned.speakers.push_back({i + 1, j});
            i = j;
        } else {
            ++i;
        }
    }
    return scanned;
}

std::string body_bytes_to_string(const std::vector<std::uint8_t>& data, std::size_t begin,
                                 std::size_t end) {
    return std::string(reinterpret_cast<const char*>(data.data()) + begin, end - begin);
}

// —— JSON 解析辅助 ——

std::string json_type_name(const detail::JsonValue& value) {
    using Type = detail::JsonValue::Type;
    switch (value.type) {
        case Type::null_value:
            return "null";
        case Type::boolean:
            return "布尔值";
        case Type::number:
            return "数字";
        case Type::string:
            return "字符串";
        case Type::array:
            return "数组";
        case Type::object:
            return "对象";
    }
    return "未知值";
}

void check_object_keys(const detail::JsonValue& value,
                       std::initializer_list<std::string_view> allowed, const std::string& where) {
    if (value.type != detail::JsonValue::Type::object) {
        throw Error(ErrorCode::invalid_content, where + " 应为对象，实际是" + json_type_name(value) + "。");
    }
    for (const auto& [key, member] : value.object) {
        bool known = false;
        for (const auto name : allowed) {
            if (key == name) {
                known = true;
                break;
            }
        }
        if (!known) {
            throw Error(ErrorCode::invalid_content, where + " 含未知字段 \"" + key + "\"。");
        }
    }
}

// 返回指针而不是引用：第三参数常以字符串字面量临时对象传入，
// GCC 13 的 -Wdangling-reference 会把「引用返回 + 临时实参」保守误报为悬垂引用（GCC 14 已修复该误报）。
const detail::JsonValue* require_member(const detail::JsonValue& value, std::string_view key,
                                        const std::string& where) {
    const auto* member = value.find(key);
    if (member == nullptr) {
        throw Error(ErrorCode::invalid_content,
                    where + " 缺少 \"" + std::string(key) + "\" 字段。");
    }
    return member;
}

std::uint32_t require_index(const detail::JsonValue& value, std::string_view field,
                            const std::string& where, std::uint32_t max_value) {
    if (value.type != detail::JsonValue::Type::number) {
        throw Error(ErrorCode::invalid_content,
                    where + " 的 " + std::string(field) + " 应为整数。");
    }
    const double number = value.number;
    if (number < 0 || number > static_cast<double>(max_value) || number != std::floor(number)) {
        throw Error(ErrorCode::invalid_content,
                    where + " 的 " + std::string(field) + " 超出取值范围。");
    }
    return static_cast<std::uint32_t>(number);
}

std::string require_text(const detail::JsonValue& value, std::string_view field,
                         const std::string& where) {
    if (value.type != detail::JsonValue::Type::string || value.string.empty()) {
        throw Error(ErrorCode::invalid_content,
                    where + " 的 " + std::string(field) + " 应为非空字符串。");
    }
    return value.string;
}

}  // namespace

TextFileMap extract_text_map(std::string name, const DatScript& script) {
    const auto scanned = scan_script(script);
    const auto& data = script.decrypted;
    TextFileMap map;
    map.name = std::move(name);
    map.entries.reserve(scanned.entries.size());
    for (const auto& entry : scanned.entries) {
        TextEntry text_entry;
        text_entry.id = entry.id;
        text_entry.offset = static_cast<std::uint32_t>(entry.begin - header_size);
        const std::string bytes = body_bytes_to_string(data, entry.begin, entry.end);
        text_entry.original = detail::codepage_to_utf8(bytes, source_codepage);
        text_entry.text = text_entry.original;
        map.entries.push_back(std::move(text_entry));
    }
    for (const auto& speaker : scanned.speakers) {
        const std::string bytes = body_bytes_to_string(data, speaker.begin, speaker.end);
        const std::string utf8 = detail::codepage_to_utf8(bytes, source_codepage);
        if (std::find(map.speakers.begin(), map.speakers.end(), utf8) == map.speakers.end()) {
            map.speakers.push_back(utf8);
        }
    }
    return map;
}

std::vector<std::uint8_t> apply_text_map(const DatScript& script, const TextFileMap& file_map,
                                         const std::vector<SpeakerPair>& speakers,
                                         unsigned codepage) {
    if (codepage != 932 && codepage != 936) {
        throw Error(ErrorCode::invalid_content, "仅支持代码页 932（cp932）与 936（gbk）。");
    }
    const auto& data = script.decrypted;
    if (data.size() < header_size + 2) {
        throw Error(ErrorCode::truncated_data, "DAT 数据不完整，无法回填文本。");
    }
    const std::size_t body_begin = header_size;
    const std::size_t body_end = data.size() - 2;

    const auto scanned = scan_script(script);
    if (scanned.entries.size() != file_map.entries.size()) {
        throw Error(ErrorCode::invalid_content,
                    "清单与文件不匹配：" + file_map.name + " 实际含 " +
                        std::to_string(scanned.entries.size()) + " 条文本，清单列出 " +
                        std::to_string(file_map.entries.size()) + " 条。");
    }

    struct Substitution {
        std::size_t begin;
        std::size_t end;
        std::vector<std::uint8_t> bytes;
    };
    std::vector<Substitution> substitutions;

    // —— 条目替换：逐条按偏移三元校验，未修改的条目保留原字节 ——
    for (const auto& entry : file_map.entries) {
        const std::string where = file_map.name + " 偏移 " + std::to_string(entry.offset);
        if (entry.original.empty()) {
            throw Error(ErrorCode::invalid_content, where + " 的原文为空，清单可能被破坏。");
        }
        std::string original_bytes;
        try {
            original_bytes = detail::utf8_to_codepage(entry.original, source_codepage);
        } catch (const Error& error) {
            throw Error(error.code(),
                        where + " 的原文无法按 cp932 还原（可能被误改）：" + error.what());
        }
        const std::size_t begin = body_begin + entry.offset;
        const bool in_range =
            begin >= body_begin + 1 && begin + original_bytes.size() + 1 <= body_end;
        // 注意比较双方必须同为 char（或同为无符号）：signed char 与 uint8_t 对
        // 0x80 以上的字节整型提升结果不同，混用会让非 ASCII 原文的校验必然失败。
        if (!in_range || data[begin - 1] != '"' ||
            !std::equal(original_bytes.begin(), original_bytes.end(),
                        reinterpret_cast<const char*>(data.data()) + begin) ||
            data[begin + original_bytes.size()] != '"') {
            throw Error(ErrorCode::invalid_content,
                        where + " 处的原文与清单不一致（清单可能来自其他版本的文件）。");
        }
        if (entry.text == entry.original) {
            continue;
        }
        check_translation_text(entry.text, where);
        std::string translated;
        try {
            translated = detail::utf8_to_codepage(entry.text, codepage);
        } catch (const Error& error) {
            throw Error(error.code(), where + " 的译文编码失败：" + error.what());
        }
        substitutions.push_back(
            {begin, begin + original_bytes.size(),
             std::vector<std::uint8_t>(translated.begin(), translated.end())});
    }

    // —— 说话者替换：#名字 按字节匹配名字表，仅替换 to 与 from 不同的条目 ——
    std::vector<std::pair<std::string, std::string>> speaker_bytes;
    for (const auto& pair : speakers) {
        if (pair.from == pair.to) {
            continue;
        }
        if (pair.from.empty() || pair.to.empty()) {
            throw Error(ErrorCode::invalid_content, "说话者表的 from/to 不能为空。");
        }
        check_translation_text(pair.to, "说话者“" + pair.from + "”");
        const std::string from_bytes = detail::utf8_to_codepage(pair.from, source_codepage);
        const std::string to_bytes = detail::utf8_to_codepage(pair.to, codepage);
        speaker_bytes.emplace_back(from_bytes, to_bytes);
    }
    if (!speaker_bytes.empty()) {
        for (const auto& span : scanned.speakers) {
            const std::string_view name_bytes(
                reinterpret_cast<const char*>(data.data()) + span.begin, span.end - span.begin);
            for (const auto& [from_bytes, to_bytes] : speaker_bytes) {
                if (name_bytes == from_bytes) {
                    substitutions.push_back(
                        {span.begin, span.end,
                         std::vector<std::uint8_t>(to_bytes.begin(), to_bytes.end())});
                    break;
                }
            }
        }
    }

    std::sort(substitutions.begin(), substitutions.end(),
              [](const Substitution& a, const Substitution& b) { return a.begin < b.begin; });
    for (std::size_t i = 1; i < substitutions.size(); ++i) {
        if (substitutions[i].begin < substitutions[i - 1].end) {
            throw Error(ErrorCode::invalid_content,
                        "替换区间重叠，清单可能被破坏：" + file_map.name);
        }
    }
    for (const auto& label : script.labels) {
        const std::size_t label_begin = body_begin + label.text_offset;
        const std::size_t label_end = label_begin + 1 + label.name.size();
        for (const auto& sub : substitutions) {
            if (sub.begin < label_end && sub.end > label_begin) {
                throw Error(ErrorCode::invalid_content,
                            "替换区间与标签 " + label.name + " 重叠：" + file_map.name);
            }
        }
    }

    // —— 构建新正文与输出文件 ——
    std::vector<std::uint8_t> new_body;
    new_body.reserve(body_end - body_begin);
    std::size_t cursor = body_begin;
    for (const auto& sub : substitutions) {
        new_body.insert(new_body.end(), data.begin() + static_cast<std::ptrdiff_t>(cursor),
                        data.begin() + static_cast<std::ptrdiff_t>(sub.begin));
        new_body.insert(new_body.end(), sub.bytes.begin(), sub.bytes.end());
        cursor = sub.end;
    }
    new_body.insert(new_body.end(), data.begin() + static_cast<std::ptrdiff_t>(cursor),
                    data.begin() + static_cast<std::ptrdiff_t>(body_end));

    std::vector<std::uint8_t> output;
    output.reserve(header_size + new_body.size() + 2);
    output.insert(output.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(header_size));
    output.insert(output.end(), new_body.begin(), new_body.end());
    // 终止 NUL 与原始种子原样保留。
    output.insert(output.end(), data.begin() + static_cast<std::ptrdiff_t>(body_end), data.end());

    // —— 标签偏移按替换增量重建 ——（substitutions 已按 begin 升序）
    for (const auto& label : script.labels) {
        const std::size_t label_begin = body_begin + label.text_offset;
        std::int64_t new_offset = static_cast<std::int64_t>(label.text_offset);
        for (const auto& sub : substitutions) {
            if (sub.begin < label_begin) {
                new_offset += static_cast<std::int64_t>(sub.bytes.size()) -
                              static_cast<std::int64_t>(sub.end - sub.begin);
            } else {
                break;
            }
        }
        if (new_offset < 0 || new_offset > 0xFFFFFFFFll) {
            throw Error(ErrorCode::invalid_content, "内部错误：标签偏移超出范围。");
        }
        const std::size_t record = static_cast<std::size_t>(label.slot) * 36 + 32;
        detail::write_u32_le(output.data() + record, static_cast<std::uint32_t>(new_offset));
        const std::size_t at = static_cast<std::size_t>(new_offset);
        if (at + 1 + label.name.size() > new_body.size() || new_body[at] != '$' ||
            !std::equal(label.name.begin(), label.name.end(),
                        reinterpret_cast<const char*>(new_body.data()) + at + 1)) {
            throw Error(ErrorCode::invalid_content, "内部错误：重建后的标签偏移不正确。");
        }
    }

    // —— 重新加密并自检往返 ——
    detail::xor_dat_body(output, script.seed);
    const auto check = decode_dat(output);
    if (check.decrypted.size() != output.size() ||
        check.decrypted.size() < header_size + 2 ||
        check.decrypted.size() - 2 - header_size != new_body.size() ||
        !std::equal(new_body.begin(), new_body.end(), check.decrypted.begin() + static_cast<std::ptrdiff_t>(header_size))) {
        throw Error(ErrorCode::invalid_content, "内部错误：重建 DAT 的往返校验失败。");
    }
    return output;
}

// —— JSON 清单序列化 ——

std::string serialize_text_map(const TextMap& map) {
    std::string out;
    out += "{\n";
    out += "  \"format\": \"eagls-text-map\",\n";
    out += "  \"version\": 1,\n";
    out += "  \"encoding\": " + detail::escape_json_string(map.encoding) + ",\n";
    out += "  \"speakers\": [\n";
    for (std::size_t i = 0; i < map.speakers.size(); ++i) {
        out += "    {\"from\": " + detail::escape_json_string(map.speakers[i].from) +
               ", \"to\": " + detail::escape_json_string(map.speakers[i].to) + "}";
        out += (i + 1 < map.speakers.size()) ? ",\n" : "\n";
    }
    out += "  ],\n";
    out += "  \"files\": [\n";
    for (std::size_t f = 0; f < map.files.size(); ++f) {
        const auto& file = map.files[f];
        out += "    {\n";
        out += "      \"name\": " + detail::escape_json_string(file.name) + ",\n";
        out += "      \"entries\": [\n";
        for (std::size_t e = 0; e < file.entries.size(); ++e) {
            const auto& entry = file.entries[e];
            out += "        {\"id\": ";
            if (entry.id == no_text_id) {
                out += "null";
            } else {
                out += std::to_string(entry.id);
            }
            out += ", \"off\": " + std::to_string(entry.offset) +
                   ", \"original\": " + detail::escape_json_string(entry.original) +
                   ", \"text\": " + detail::escape_json_string(entry.text) + "}";
            out += (e + 1 < file.entries.size()) ? ",\n" : "\n";
        }
        out += "      ]\n";
        out += "    }";
        out += (f + 1 < map.files.size()) ? ",\n" : "\n";
    }
    out += "  ]\n";
    out += "}\n";
    return out;
}

// —— JSON 清单解析（严格白名单校验）——

TextMap parse_text_map(std::string_view json) {
    const auto root = detail::parse_json(json);
    check_object_keys(root, {"format", "version", "encoding", "speakers", "files"}, "清单根");

    const auto* format = require_member(root, "format", "清单根");
    if (format->type != detail::JsonValue::Type::string || format->string != "eagls-text-map") {
        throw Error(ErrorCode::invalid_content,
                    "不是 EAGLS 文本清单（format 字段应为 \"eagls-text-map\"）。");
    }
    const auto* version = require_member(root, "version", "清单根");
    if (version->type != detail::JsonValue::Type::number || version->number != 1.0) {
        throw Error(ErrorCode::invalid_content, "不支持的清单版本（version 应为 1）。");
    }

    TextMap map;
    const auto* encoding = require_member(root, "encoding", "清单根");
    if (encoding->type != detail::JsonValue::Type::string) {
        throw Error(ErrorCode::invalid_content, "清单根的 encoding 应为字符串。");
    }
    if (encoding->string != "cp932") {
        throw Error(ErrorCode::invalid_content,
                    "不支持的原文编码 \"" + encoding->string + "\"（当前仅支持 cp932）。");
    }
    map.encoding = encoding->string;

    const auto* speakers = require_member(root, "speakers", "清单根");
    if (speakers->type != detail::JsonValue::Type::array) {
        throw Error(ErrorCode::invalid_content, "清单根的 speakers 应为数组。");
    }
    std::set<std::string> speaker_seen;
    for (const auto& item : speakers->array) {
        check_object_keys(item, {"from", "to"}, "speakers 元素");
        const auto* from = require_member(item, "from", "speakers 元素");
        const auto* to = require_member(item, "to", "speakers 元素");
        SpeakerPair pair;
        pair.from = require_text(*from, "from", "speakers 元素");
        pair.to = require_text(*to, "to", "speakers 元素");
        if (!speaker_seen.insert(pair.from).second) {
            throw Error(ErrorCode::invalid_content, "speakers 中出现重复的名字：" + pair.from);
        }
        map.speakers.push_back(std::move(pair));
    }

    const auto* files = require_member(root, "files", "清单根");
    if (files->type != detail::JsonValue::Type::array) {
        throw Error(ErrorCode::invalid_content, "清单根的 files 应为数组。");
    }
    std::set<std::string> file_seen;
    for (const auto& file : files->array) {
        check_object_keys(file, {"name", "entries"}, "files 元素");
        const auto* name = require_member(file, "name", "files 元素");
        TextFileMap file_map;
        file_map.name = require_text(*name, "name", "files 元素");
        if (!file_seen.insert(ascii_lower(file_map.name)).second) {
            throw Error(ErrorCode::invalid_content, "files 中出现重复的文件名：" + file_map.name);
        }
        const auto* entries = require_member(file, "entries", "files 元素");
        if (entries->type != detail::JsonValue::Type::array) {
            throw Error(ErrorCode::invalid_content, "files 元素的 entries 应为数组。");
        }
        for (const auto& item : entries->array) {
            check_object_keys(item, {"id", "off", "original", "text"}, "entries 元素");
            TextEntry entry;
            const auto* id = require_member(item, "id", "entries 元素");
            if (id->type == detail::JsonValue::Type::null_value) {
                entry.id = no_text_id;
            } else {
                entry.id = require_index(*id, "id", "entries 元素", 0xFFFFFFFEu);
            }
            entry.offset = require_index(*require_member(item, "off", "entries 元素"), "off",
                                         "entries 元素", 0xFFFFFFFFu);
            entry.original = require_text(*require_member(item, "original", "entries 元素"),
                                          "original", "entries 元素");
            entry.text = require_text(*require_member(item, "text", "entries 元素"), "text",
                                      "entries 元素");
            file_map.entries.push_back(std::move(entry));
        }
        map.files.push_back(std::move(file_map));
    }
    return map;
}

}  // namespace eagls
