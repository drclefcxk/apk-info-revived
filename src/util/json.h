// Minimal JSON reader/writer.
//
// The configuration file is JSON, so the program needs to read it without pulling in
// a dependency. Only what a settings file needs is implemented: objects, arrays,
// strings, numbers, booleans and null, with \u escapes and the standard escapes.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apkinfo::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;

    bool boolean = false;
    double number = 0.0;
    std::string text;                                  // Type::String
    std::vector<Value> items;                          // Type::Array
    std::vector<std::pair<std::string, Value>> members;  // Type::Object

    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }
    bool is_string() const { return type == Type::String; }
    bool is_number() const { return type == Type::Number; }
    bool is_bool() const { return type == Type::Bool; }

    // Object member lookup; nullptr when absent or when this is not an object.
    const Value* find(std::string_view key) const;
    // Convenience accessors with a fallback for a missing/wrong-typed value.
    std::string string_or(std::string_view fallback = {}) const;
    double number_or(double fallback = 0.0) const;
    bool bool_or(bool fallback = false) const;
};

// Parses `text`. Returns std::nullopt when the document is malformed; `error` then
// carries a short reason (position included).
std::optional<Value> parse(std::string_view text, std::string* error = nullptr);

// Quotes and escapes `text` for embedding in a JSON document.
std::string quote(std::string_view text);

}  // namespace apkinfo::json
