#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace esp {

// Minimal in-place JSON value used by OffsetTable parsing. Supports objects,
// arrays, strings, numbers (int64/double), booleans and null. Strings are
// unescaped for \\, \", \/, \n, \r, \t and \uXXXX (BMP only).
class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() : type_(Type::Null) {}
    explicit JsonValue(Type type) : type_(type) {}

    Type type() const { return type_; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_string() const { return type_ == Type::String; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_bool() const { return type_ == Type::Bool; }

    bool as_bool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    double as_number(double fallback = 0.0) const {
        return type_ == Type::Number ? number_ : fallback;
    }
    const std::string& as_string() const { return string_; }

    // Mutable accessors used by the parser (kept private surface small).
    std::string& mutable_string() { return string_; }
    std::vector<JsonValue>& mutable_array() { return array_; }
    std::vector<std::pair<std::string, JsonValue>>& mutable_members() { return members_; }
    bool& mutable_bool() { return bool_; }
    double& mutable_number() { return number_; }

    const JsonValue* find(const std::string& key) const {
        if (type_ != Type::Object) return nullptr;
        for (const auto& [name, value] : members_) {
            if (name == key) return &value;
        }
        return nullptr;
    }

    const std::vector<std::pair<std::string, JsonValue>>& object_members() const {
        return members_;
    }

    static bool parse(const std::string& text, JsonValue& out, std::string& error);

private:
    Type type_;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<JsonValue> array_;
    std::vector<std::pair<std::string, JsonValue>> members_;
};

struct FieldOffset {
    uint32_t offset = 0;
    std::string type_name;
};

struct ClassOffsets {
    std::string name;
    bool has_static_fields = false;
    uint32_t static_fields_offset = 0;
    std::vector<std::pair<std::string, FieldOffset>> fields;
};

struct GlobalOffset {
    uint64_t address = 0;
};

struct MethodAddress {
    uint64_t address = 0;
};

// Versioned offset table produced by `revx export-offsets`.
class OffsetTable {
public:
    // Loads and validates the table. Returns false and fills `error` on any
    // structural problem (bad JSON, wrong format/version, missing sections).
    static bool load(const std::string& path, OffsetTable& out, std::string& error);

    const ClassOffsets* find_class(const std::string& name) const;
    const FieldOffset* find_field(const std::string& class_name, const std::string& field) const;
    const GlobalOffset* find_global(const std::string& name) const;
    const MethodAddress* find_method(const std::string& name) const;

    const std::string& binary_name() const { return binary_name_; }
    const std::string& hash_blake3() const { return hash_blake3_; }
    int table_version() const { return version_; }
    size_t class_count() const { return classes_.size(); }
    size_t global_count() const { return globals_.size(); }
    size_t method_count() const { return methods_.size(); }

private:
    int version_ = 0;
    std::string binary_name_;
    std::string hash_blake3_;
    std::vector<ClassOffsets> classes_;
    std::vector<std::pair<std::string, GlobalOffset>> globals_;
    std::vector<std::pair<std::string, MethodAddress>> methods_;
};

} // namespace esp
