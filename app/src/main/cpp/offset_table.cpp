#include "offset_table.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace esp {

namespace {

struct Parser {
    const std::string& text;
    size_t pos = 0;
    std::string error;

    explicit Parser(const std::string& input) : text(input) {}

    void skip_ws() {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
            pos++;
        }
    }

    bool fail(const std::string& message) {
        if (error.empty()) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), " at offset %zu", pos);
            error = message + buf;
        }
        return false;
    }

    bool peek(char c) {
        skip_ws();
        return pos < text.size() && text[pos] == c;
    }

    bool eat(char c) {
        skip_ws();
        if (pos < text.size() && text[pos] == c) {
            pos++;
            return true;
        }
        return fail(std::string("expected '") + c + "'");
    }

    bool parse_value(JsonValue& out) {
        skip_ws();
        if (pos >= text.size()) return fail("unexpected end of input");
        char c = text[pos];
        switch (c) {
            case '{':
                return parse_object(out);
            case '[':
                return parse_array(out);
            case '"': {
                out = JsonValue(JsonValue::Type::String);
                return parse_string_body(out.mutable_string());
            }
            case 't':
                return parse_literal("true", out, true);
            case 'f':
                return parse_literal("false", out, false);
            case 'n':
                return parse_null(out);
            default:
                return parse_number(out);
        }
    }

    bool parse_literal(const char* literal, JsonValue& out, bool value) {
        size_t len = 0;
        while (literal[len] != '\0') len++;
        if (text.compare(pos, len, literal) != 0) {
            return fail(std::string("invalid literal, expected ") + literal);
        }
        pos += len;
        out = JsonValue(JsonValue::Type::Bool);
        out.mutable_bool() = value;
        return true;
    }

    bool parse_null(JsonValue& out) {
        if (text.compare(pos, 4, "null") != 0) return fail("invalid literal, expected null");
        pos += 4;
        out = JsonValue();
        return true;
    }

    bool parse_number(JsonValue& out) {
        size_t start = pos;
        if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) pos++;
        bool has_digits = false;
        while (pos < text.size() &&
               (std::isdigit(static_cast<unsigned char>(text[pos])) || text[pos] == '.' ||
                text[pos] == 'e' || text[pos] == 'E' || text[pos] == '-' || text[pos] == '+')) {
            if (std::isdigit(static_cast<unsigned char>(text[pos]))) has_digits = true;
            pos++;
        }
        if (!has_digits) return fail("invalid number");
        out = JsonValue(JsonValue::Type::Number);
        out.mutable_number() = std::strtod(text.c_str() + start, nullptr);
        return true;
    }

    bool parse_string_body(std::string& out) {
        if (pos >= text.size() || text[pos] != '"') return fail("expected string");
        pos++;
        out.clear();
        while (pos < text.size()) {
            char c = text[pos];
            if (c == '"') {
                pos++;
                return true;
            }
            if (c == '\\') {
                pos++;
                if (pos >= text.size()) return fail("unterminated escape");
                char esc = text[pos];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'u': {
                        if (pos + 4 >= text.size()) return fail("bad \\u escape");
                        unsigned code = 0;
                        for (int i = 1; i <= 4; i++) {
                            char h = text[pos + i];
                            code <<= 4;
                            if (h >= '0' && h <= '9') {
                                code |= static_cast<unsigned>(h - '0');
                            } else if (h >= 'a' && h <= 'f') {
                                code |= static_cast<unsigned>(h - 'a' + 10);
                            } else if (h >= 'A' && h <= 'F') {
                                code |= static_cast<unsigned>(h - 'A' + 10);
                            } else {
                                return fail("bad \\u escape digit");
                            }
                        }
                        pos += 4;
                        if (code < 0x80) {
                            out.push_back(static_cast<char>(code));
                        } else if (code < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        break;
                    }
                    default:
                        return fail("unknown escape");
                }
                pos++;
                continue;
            }
            out.push_back(c);
            pos++;
        }
        return fail("unterminated string");
    }

    bool parse_object(JsonValue& out) {
        out = JsonValue(JsonValue::Type::Object);
        if (!eat('{')) return false;
        skip_ws();
        if (peek('}')) {
            pos++;
            return true;
        }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string_body(key)) return false;
            if (!eat(':')) return false;
            JsonValue value;
            if (!parse_value(value)) return false;
            out.mutable_members().emplace_back(std::move(key), std::move(value));
            skip_ws();
            if (pos < text.size() && text[pos] == ',') {
                pos++;
                continue;
            }
            return eat('}');
        }
    }

    bool parse_array(JsonValue& out) {
        out = JsonValue(JsonValue::Type::Array);
        if (!eat('[')) return false;
        skip_ws();
        if (peek(']')) {
            pos++;
            return true;
        }
        while (true) {
            JsonValue value;
            if (!parse_value(value)) return false;
            out.mutable_array().push_back(std::move(value));
            skip_ws();
            if (pos < text.size() && text[pos] == ',') {
                pos++;
                continue;
            }
            return eat(']');
        }
    }
};

uint32_t as_u32(const JsonValue* value) {
    if (value == nullptr || !value->is_number()) return 0;
    double number = value->as_number();
    if (number < 0.0 || number > 4294967295.0) return 0;
    return static_cast<uint32_t>(number);
}

uint64_t as_u64(const JsonValue* value) {
    if (value == nullptr || !value->is_number()) return 0;
    double number = value->as_number();
    if (number < 0.0) return 0;
    return static_cast<uint64_t>(number);
}

} // namespace

bool JsonValue::parse(const std::string& text, JsonValue& out, std::string& error) {
    Parser parser(text);
    if (!parser.parse_value(out)) {
        error = parser.error;
        return false;
    }
    parser.skip_ws();
    if (parser.pos != text.size()) {
        error = "trailing content after JSON value";
        return false;
    }
    return true;
}

const ClassOffsets* OffsetTable::find_class(const std::string& name) const {
    for (const auto& klass : classes_) {
        if (klass.name == name) return &klass;
    }
    return nullptr;
}

const FieldOffset* OffsetTable::find_field(const std::string& class_name,
                                           const std::string& field) const {
    const ClassOffsets* klass = find_class(class_name);
    if (klass == nullptr) return nullptr;
    for (const auto& entry : klass->fields) {
        if (entry.first == field) return &entry.second;
    }
    return nullptr;
}

const GlobalOffset* OffsetTable::find_global(const std::string& name) const {
    for (const auto& global : globals_) {
        if (global.first == name) return &global.second;
    }
    return nullptr;
}

const MethodAddress* OffsetTable::find_method(const std::string& name) const {
    for (const auto& method : methods_) {
        if (method.first == name) return &method.second;
    }
    return nullptr;
}

bool OffsetTable::load(const std::string& path, OffsetTable& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path;
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    JsonValue root;
    if (!JsonValue::parse(text, root, error)) {
        error = "invalid JSON in " + path + ": " + error;
        return false;
    }
    if (!root.is_object()) {
        error = "offset table root must be an object";
        return false;
    }

    const JsonValue* format = root.find("format");
    if (format == nullptr || !format->is_string() || format->as_string() != "esp-offsets") {
        error = "format must be \"esp-offsets\"";
        return false;
    }
    const JsonValue* version = root.find("version");
    if (version == nullptr || !version->is_number() || version->as_number() != 1.0) {
        error = "unsupported offset table version";
        return false;
    }

    out = OffsetTable();
    out.version_ = 1;
    if (const JsonValue* binary = root.find("binary"); binary != nullptr && binary->is_string()) {
        out.binary_name_ = binary->as_string();
    }
    if (const JsonValue* hash = root.find("hash_blake3"); hash != nullptr && hash->is_string()) {
        out.hash_blake3_ = hash->as_string();
    }

    if (const JsonValue* classes = root.find("classes"); classes != nullptr && classes->is_object()) {
        for (const auto& [class_name, class_value] : classes->object_members()) {
            if (!class_value.is_object()) continue;
            ClassOffsets klass;
            klass.name = class_name;
            if (const JsonValue* static_fields = class_value.find("static_fields_offset");
                static_fields != nullptr && static_fields->is_number()) {
                klass.has_static_fields = true;
                klass.static_fields_offset = as_u32(static_fields);
            }
            if (const JsonValue* fields = class_value.find("fields"); fields != nullptr && fields->is_object()) {
                for (const auto& [field_name, field_value] : fields->object_members()) {
                    if (!field_value.is_object()) continue;
                    FieldOffset field;
                    field.offset = as_u32(field_value.find("offset"));
                    if (const JsonValue* type = field_value.find("type"); type != nullptr && type->is_string()) {
                        field.type_name = type->as_string();
                    }
                    klass.fields.emplace_back(field_name, field);
                }
            }
            out.classes_.push_back(std::move(klass));
        }
    }

    if (const JsonValue* globals = root.find("globals"); globals != nullptr && globals->is_object()) {
        for (const auto& [name, value] : globals->object_members()) {
            if (!value.is_object()) continue;
            out.globals_.emplace_back(name, GlobalOffset{as_u64(value.find("address"))});
        }
    }

    if (const JsonValue* methods = root.find("methods"); methods != nullptr && methods->is_object()) {
        for (const auto& [name, value] : methods->object_members()) {
            if (!value.is_object()) continue;
            out.methods_.emplace_back(name, MethodAddress{as_u64(value.find("address"))});
        }
    }

    return true;
}

} // namespace esp
