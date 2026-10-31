// Minimal JSON value for the automation protocol and the MCP connector.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace enp::model {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int n) : type_(Type::Number), number_(n) {}
    Json(double n) : type_(Type::Number), number_(n) {}
    Json(const char* s) : type_(Type::String), string_(s) {}
    Json(std::string s) : type_(Type::String), string_(std::move(s)) {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray() const { return type_ == Type::Array; }
    bool isString() const { return type_ == Type::String; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isBool() const { return type_ == Type::Bool; }

    bool asBool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    double asNumber(double fallback = 0.0) const { return type_ == Type::Number ? number_ : fallback; }
    const std::string& asString() const;

    // Arrays.
    const std::vector<Json>& items() const { return items_; }
    Json& push(Json value);
    // Objects (insertion order is kept).
    const std::vector<std::pair<std::string, Json>>& members() const { return members_; }
    Json& set(const std::string& key, Json value); // returns *this for chaining
    const Json* find(std::string_view key) const;
    // Convenience lookups with defaults.
    std::string getString(std::string_view key, const std::string& fallback = {}) const;
    double getNumber(std::string_view key, double fallback = 0.0) const;
    bool getBool(std::string_view key, bool fallback = false) const;

    std::string dump() const; // compact, single line

    // Parses one JSON value; returns nullopt and sets `error` on malformed input.
    static std::optional<Json> parse(std::string_view text, std::string* error = nullptr);

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> members_;
};

} // namespace enp::model
