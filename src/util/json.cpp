#include "util/json.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace apkinfo::json {
namespace {

class Parser {
public:
    Parser(std::string_view text, std::string* error) : text_(text), error_(error) {}

    std::optional<Value> document() {
        Value value;
        skip_space();
        if (!parse_value(&value, 0)) return std::nullopt;
        skip_space();
        if (position_ != text_.size()) {
            fail("trailing characters");
            return std::nullopt;
        }
        return value;
    }

private:
    bool fail(const char* what) {
        if (error_) {
            *error_ = std::string(what) + " at offset " + std::to_string(position_);
        }
        return false;
    }

    void skip_space() {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++position_;
            } else {
                break;
            }
        }
    }

    bool consume(char expected) {
        skip_space();
        if (position_ >= text_.size() || text_[position_] != expected) {
            return fail("expected a character");
        }
        ++position_;
        return true;
    }

    bool parse_value(Value* out, int depth) {
        if (depth > 64) return fail("nesting too deep");
        skip_space();
        if (position_ >= text_.size()) return fail("unexpected end of input");
        switch (text_[position_]) {
            case '{':
                return parse_object(out, depth);
            case '[':
                return parse_array(out, depth);
            case '"':
                out->type = Value::Type::String;
                return parse_string(&out->text);
            case 't':
                return parse_literal("true", &out->boolean, true, out);
            case 'f':
                return parse_literal("false", &out->boolean, false, out);
            case 'n':
                return parse_literal("null", nullptr, false, out);
            default:
                return parse_number(out);
        }
    }

    bool parse_literal(const char* literal, bool* boolean, bool value, Value* out) {
        const size_t length = std::strlen(literal);
        if (text_.compare(position_, length, literal) != 0) return fail("unknown literal");
        position_ += length;
        if (boolean) {
            out->type = Value::Type::Bool;
            *boolean = value;
        } else {
            out->type = Value::Type::Null;
        }
        return true;
    }

    bool parse_number(Value* out) {
        const char* begin = text_.data() + position_;
        char* end = nullptr;
        const double number = std::strtod(begin, &end);
        if (end == begin) return fail("expected a number");
        position_ += static_cast<size_t>(end - begin);
        out->type = Value::Type::Number;
        out->number = number;
        return true;
    }

    static void append_utf8(std::string* out, unsigned codepoint) {
        if (codepoint < 0x80) {
            out->push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    bool parse_hex4(unsigned* out) {
        if (position_ + 4 > text_.size()) return fail("truncated escape");
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[position_++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                return fail("bad hex digit");
            }
        }
        *out = value;
        return true;
    }

    bool parse_string(std::string* out) {
        if (!consume('"')) return false;
        out->clear();
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"') return true;
            if (c != '\\') {
                out->push_back(c);
                continue;
            }
            if (position_ >= text_.size()) return fail("unterminated escape");
            const char escape = text_[position_++];
            switch (escape) {
                case '"':
                case '\\':
                case '/':
                    out->push_back(escape);
                    break;
                case 'b':
                    out->push_back('\b');
                    break;
                case 'f':
                    out->push_back('\f');
                    break;
                case 'n':
                    out->push_back('\n');
                    break;
                case 'r':
                    out->push_back('\r');
                    break;
                case 't':
                    out->push_back('\t');
                    break;
                case 'u': {
                    unsigned codepoint = 0;
                    if (!parse_hex4(&codepoint)) return false;
                    // surrogate pair
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF && position_ + 6 <= text_.size() &&
                        text_[position_] == '\\' && text_[position_ + 1] == 'u') {
                        position_ += 2;
                        unsigned low = 0;
                        if (!parse_hex4(&low)) return false;
                        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                    }
                    append_utf8(out, codepoint);
                    break;
                }
                default:
                    return fail("unknown escape");
            }
        }
        return fail("unterminated string");
    }

    bool parse_array(Value* out, int depth) {
        if (!consume('[')) return false;
        out->type = Value::Type::Array;
        skip_space();
        if (position_ < text_.size() && text_[position_] == ']') {
            ++position_;
            return true;
        }
        while (true) {
            Value item;
            if (!parse_value(&item, depth + 1)) return false;
            out->items.push_back(std::move(item));
            skip_space();
            if (position_ >= text_.size()) return fail("unterminated array");
            if (text_[position_] == ',') {
                ++position_;
                continue;
            }
            if (text_[position_] == ']') {
                ++position_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool parse_object(Value* out, int depth) {
        if (!consume('{')) return false;
        out->type = Value::Type::Object;
        skip_space();
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            return true;
        }
        while (true) {
            skip_space();
            std::string key;
            if (!parse_string(&key)) return false;
            if (!consume(':')) return false;
            Value value;
            if (!parse_value(&value, depth + 1)) return false;
            out->members.emplace_back(std::move(key), std::move(value));
            skip_space();
            if (position_ >= text_.size()) return fail("unterminated object");
            if (text_[position_] == ',') {
                ++position_;
                continue;
            }
            if (text_[position_] == '}') {
                ++position_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    std::string_view text_;
    size_t position_ = 0;
    std::string* error_;
};

}  // namespace

const Value* Value::find(std::string_view key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& member : members) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

std::string Value::string_or(std::string_view fallback) const {
    return type == Type::String ? text : std::string(fallback);
}

double Value::number_or(double fallback) const {
    return type == Type::Number ? number : fallback;
}

bool Value::bool_or(bool fallback) const {
    return type == Type::Bool ? boolean : fallback;
}

std::optional<Value> parse(std::string_view text, std::string* error) {
    Parser parser(text, error);
    return parser.document();
}

std::string quote(std::string_view text) {
    std::string out = "\"";
    out.reserve(text.size() + 2);
    for (unsigned char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
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
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                    out += buffer;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += "\"";
    return out;
}

}  // namespace apkinfo::json
