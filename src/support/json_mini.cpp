#include "support/json_mini.hpp"

#include <eagls/error.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace eagls::detail {
namespace {

// 递归下降解析器。pos_ 始终停在下一个待处理字符；出错信息换算为行列。
class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    JsonValue parse_document() {
        skip_bom();
        skip_whitespace();
        JsonValue value = parse_value();
        skip_whitespace();
        if (pos_ != text_.size()) {
            fail("文档在第一个 JSON 值之后还有其他内容");
        }
        return value;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        std::size_t line = 1;
        std::size_t column = 1;
        const std::size_t stop = pos_ < text_.size() ? pos_ : text_.size();
        for (std::size_t i = 0; i < stop; ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        throw Error(ErrorCode::invalid_content, "JSON 解析失败（第 " + std::to_string(line) +
                                                    " 行第 " + std::to_string(column) +
                                                    " 列）：" + message);
    }

    void skip_bom() {
        if (text_.size() >= 2) {
            const auto b0 = static_cast<unsigned char>(text_[0]);
            const auto b1 = static_cast<unsigned char>(text_[1]);
            if ((b0 == 0xFF && b1 == 0xFE) || (b0 == 0xFE && b1 == 0xFF)) {
                fail("输入看起来是 UTF-16，请以 UTF-8 保存清单文件");
            }
        }
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB &&
            static_cast<unsigned char>(text_[2]) == 0xBF) {
            pos_ = 3;
        }
    }

    void skip_whitespace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    void expect(char expected) {
        if (pos_ >= text_.size() || text_[pos_] != expected) {
            fail(std::string("应为 '") + expected + "'");
        }
        ++pos_;
    }

    void expect_literal(std::string_view literal) {
        if (text_.substr(pos_, literal.size()) != literal) {
            fail("无法识别的字面量");
        }
        pos_ += literal.size();
    }

    JsonValue parse_value() {
        if (pos_ >= text_.size()) {
            fail("文档意外结束");
        }
        switch (text_[pos_]) {
            case '{':
                return parse_object();
            case '[':
                return parse_array();
            case '"':
                return parse_string();
            case 't': {
                expect_literal("true");
                JsonValue value;
                value.type = JsonValue::Type::boolean;
                value.boolean = true;
                return value;
            }
            case 'f': {
                expect_literal("false");
                JsonValue value;
                value.type = JsonValue::Type::boolean;
                value.boolean = false;
                return value;
            }
            case 'n': {
                expect_literal("null");
                return JsonValue{};
            }
            default:
                return parse_number();
        }
    }

    JsonValue parse_object() {
        expect('{');
        JsonValue result;
        result.type = JsonValue::Type::object;
        skip_whitespace();
        if (peek() == '}') {
            ++pos_;
            return result;
        }
        while (true) {
            skip_whitespace();
            if (peek() != '"') {
                fail("对象的键必须是字符串");
            }
            JsonValue key = parse_string();
            skip_whitespace();
            expect(':');
            skip_whitespace();
            result.object.emplace_back(std::move(key.string), parse_value());
            skip_whitespace();
            const char c = peek();
            if (c == ',') {
                ++pos_;
                continue;
            }
            if (c == '}') {
                ++pos_;
                break;
            }
            fail("对象成员之间应为 ',' 或 '}'");
        }
        return result;
    }

    JsonValue parse_array() {
        expect('[');
        JsonValue result;
        result.type = JsonValue::Type::array;
        skip_whitespace();
        if (peek() == ']') {
            ++pos_;
            return result;
        }
        while (true) {
            skip_whitespace();
            result.array.push_back(parse_value());
            skip_whitespace();
            const char c = peek();
            if (c == ',') {
                ++pos_;
                continue;
            }
            if (c == ']') {
                ++pos_;
                break;
            }
            fail("数组元素之间应为 ',' 或 ']'");
        }
        return result;
    }

    JsonValue parse_string() {
        expect('"');
        std::string out;
        while (true) {
            if (pos_ >= text_.size()) {
                fail("字符串缺少结束引号");
            }
            const auto c = static_cast<unsigned char>(text_[pos_]);
            if (c == '"') {
                ++pos_;
                break;
            }
            if (c < 0x20) {
                fail("字符串中出现未转义的控制字符");
            }
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                ++pos_;
                continue;
            }
            ++pos_;
            if (pos_ >= text_.size()) {
                fail("转义序列不完整");
            }
            const char escape = text_[pos_++];
            switch (escape) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    unsigned code = parse_hex4();
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (pos_ + 1 < text_.size() && text_[pos_] == '\\' &&
                            text_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            const unsigned low = parse_hex4();
                            if (low < 0xDC00 || low > 0xDFFF) {
                                fail("UTF-16 代理对不完整");
                            }
                            code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
                        } else {
                            fail("UTF-16 代理对不完整");
                        }
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        fail("出现了孤立的 UTF-16 低位代理");
                    }
                    append_utf8(out, code);
                    break;
                }
                default:
                    fail("不支持的转义序列");
            }
        }
        JsonValue result;
        result.type = JsonValue::Type::string;
        result.string = std::move(out);
        return result;
    }

    unsigned parse_hex4() {
        if (pos_ + 4 > text_.size()) {
            fail("\\u 转义不完整");
        }
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                fail("\\u 转义中出现非十六进制字符");
            }
        }
        return value;
    }

    static void append_utf8(std::string& out, unsigned code) {
        if (code <= 0x7F) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    JsonValue parse_number() {
        const std::size_t start = pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' ||
                c == 'E') {
                ++pos_;
            } else {
                break;
            }
        }
        if (pos_ == start) {
            fail("此处应为 JSON 值");
        }
        const std::string token(text_.substr(start, pos_ - start));
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0') {
            fail("数字格式不正确");
        }
        JsonValue result;
        result.type = JsonValue::Type::number;
        result.number = value;
        return result;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

}  // namespace

const JsonValue* JsonValue::find(std::string_view key) const {
    if (type != Type::object) {
        return nullptr;
    }
    for (const auto& [name, value] : object) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

JsonValue parse_json(std::string_view text) {
    Parser parser(text);
    return parser.parse_document();
}

std::string escape_json_string(std::string_view text) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0xF]);
                    out.push_back(hex[c & 0xF]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace eagls::detail
