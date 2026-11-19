#pragma once

#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value;

using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

struct Value {
    using Type = json::Type;
    Type type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;
    Array a;
    Object o;

    Value() = default;
    Value(bool v) : type(Type::Bool), b(v) {}
    Value(int v) : type(Type::Number), n(static_cast<double>(v)) {}
    Value(double v) : type(Type::Number), n(v) {}
    Value(const char* v) : type(Type::String), s(v) {}
    Value(const std::string& v) : type(Type::String), s(v) {}
    Value(const Array& v) : type(Type::Array), a(v) {}
    Value(const Object& v) : type(Type::Object), o(v) {}

    static Value object() { return Value(Object{}); }
    static Value array() { return Value(Array{}); }

    Value& operator[](const std::string& key) {
        if (type != Type::Object) {
            type = Type::Object;
            o.clear();
        }
        return o[key];
    }

    // const lookup that doesn't insert; returns a Null Value if absent.
    const Value& at(const std::string& key) const {
        static const Value null_value;
        if (type != Type::Object)
            return null_value;
        auto it = o.find(key);
        return it == o.end() ? null_value : it->second;
    }
    bool contains(const std::string& key) const {
        return type == Type::Object && o.count(key) != 0;
    }

    void push_back(const Value& v) {
        if (type != Type::Array) {
            type = Type::Array;
            a.clear();
        }
        a.push_back(v);
    }

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }
    const Value* find(const std::string& key) const {
        if (!is_object())
            return nullptr;
        const auto it = o.find(key);
        return it == o.end() ? nullptr : &it->second;
    }
    std::string string_or(const std::string& fallback) const { return is_string() ? s : fallback; }
    bool bool_or(bool fallback) const { return is_bool() ? b : fallback; }
    std::uint32_t u32_or(std::uint32_t fallback) const {
        return is_number() && std::isfinite(n) && n >= 0 &&
                       n <= std::numeric_limits<std::uint32_t>::max()
                   ? static_cast<std::uint32_t>(n)
                   : fallback;
    }

    bool as_bool() const { return b; }
    double as_number() const { return n; }
    std::string as_string() const { return s; }
    const Array& as_array() const { return a; }
    const Object& as_object() const { return o; }
};

inline std::string escape_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        if (c == '"')
            out += "\\\"";
        else if (c == '\\')
            out += "\\\\";
        else if (c == '\b')
            out += "\\b";
        else if (c == '\f')
            out += "\\f";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else if ((unsigned char)c < 0x20) {
            std::array<char, 7> esc{};
            (void)snprintf(esc.data(), esc.size(), "\\u%04x", (unsigned)(unsigned char)c);
            out += esc.data();
        } else
            out += c;
    }
    out += '"';
    return out;
}

inline std::string dump(const Value& v, int indent = -1, int level = 0) {
    switch (v.type) {
    case Type::Null:
        return "null";
    case Type::Bool:
        return v.b ? "true" : "false";
    case Type::Number: {
        if (!std::isfinite(v.n))
            return "null";
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(std::numeric_limits<double>::max_digits10) << v.n;
        return stream.str();
    }
    case Type::String:
        return escape_string(v.s);
    case Type::Array: {
        if (v.a.empty())
            return "[]";
        std::string out = "[";
        if (indent >= 0)
            out += '\n';
        for (size_t i = 0; i < v.a.size(); ++i) {
            if (indent >= 0)
                out.append((size_t)((level + 1) * indent), ' ');
            out += dump(v.a[i], indent, level + 1);
            if (i + 1 < v.a.size())
                out += (indent >= 0) ? ",\n" : ", ";
        }
        if (indent >= 0) {
            out += '\n';
            out.append((size_t)(level * indent), ' ');
        }
        out += ']';
        return out;
    }
    case Type::Object: {
        if (v.o.empty())
            return "{}";
        std::string out = "{";
        if (indent >= 0)
            out += '\n';
        size_t i = 0;
        for (const auto& kv : v.o) {
            if (indent >= 0)
                out.append((size_t)((level + 1) * indent), ' ');
            out += escape_string(kv.first);
            out += ':';
            if (indent >= 0)
                out += ' ';
            out += dump(kv.second, indent, level + 1);
            if (++i < v.o.size())
                out += (indent >= 0) ? ",\n" : ", ";
        }
        if (indent >= 0) {
            out += '\n';
            out.append((size_t)(level * indent), ' ');
        }
        out += '}';
        return out;
    }
    }
    return "";
}

class Parser {
public:
    // Optional source slices for top-level object members refer to input.
    using MemberSources = std::map<std::string, std::string_view>;
    explicit Parser(const std::string& input, MemberSources* members = nullptr)
        : input_(input), members_(members) {}

    bool parse(Value& value, std::string& error) {
        if (members_)
            members_->clear();
        skip_ws();
        if (!parse_value(value, error)) {
            return false;
        }
        skip_ws();
        if (pos_ != input_.size()) {
            error = "unexpected trailing data at byte " + std::to_string(pos_);
            return false;
        }
        return true;
    }

    Value parse() {
        Value value;
        std::string error;
        return parse(value, error) ? value : Value{};
    }

private:
    std::size_t depth_ = 0;
    const std::string& input_;
    MemberSources* members_;
    std::size_t pos_ = 0;

    void skip_ws() {
        while (pos_ < input_.size()) {
            const char c = input_[pos_];
            if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
                break;
            }
            ++pos_;
        }
    }

    bool consume(char expected) {
        skip_ws();
        if (pos_ < input_.size() && input_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool consume_literal(const char* literal) {
        const std::size_t start = pos_;
        for (const char* p = literal; *p; ++p) {
            if (pos_ >= input_.size() || input_[pos_] != *p) {
                pos_ = start;
                return false;
            }
            ++pos_;
        }
        return true;
    }

    bool parse_value(Value& value, std::string& error) {
        if (depth_ >= 128) {
            error = "JSON nesting limit exceeded";
            return false;
        }
        ++depth_;
        struct DepthGuard {
            std::size_t& depth;
            ~DepthGuard() { --depth; }
        } guard{depth_};
        skip_ws();
        if (pos_ >= input_.size()) {
            error = "unexpected end of JSON";
            return false;
        }
        const char c = input_[pos_];
        if (c == '{') {
            return parse_object(value, error);
        }
        if (c == '[') {
            return parse_array(value, error);
        }
        if (c == '"') {
            value.type = Value::Type::String;
            return parse_string(value.s, error);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parse_number(value, error);
        }
        if (consume_literal("true")) {
            value.type = Value::Type::Bool;
            value.b = true;
            return true;
        }
        if (consume_literal("false")) {
            value.type = Value::Type::Bool;
            value.b = false;
            return true;
        }
        if (consume_literal("null")) {
            value.type = Value::Type::Null;
            return true;
        }
        error = "unexpected token at byte " + std::to_string(pos_);
        return false;
    }

    bool parse_object(Value& value, std::string& error) {
        if (!consume('{')) {
            error = "expected object";
            return false;
        }
        value = Value{};
        value.type = Value::Type::Object;
        skip_ws();
        if (consume('}')) {
            return true;
        }
        for (;;) {
            skip_ws();
            const size_t begin = pos_;
            std::string key;
            if (!parse_string(key, error)) {
                return false;
            }
            if (!consume(':')) {
                error = "expected ':' after object key";
                return false;
            }
            Value child;
            if (!parse_value(child, error)) {
                return false;
            }
            value.o[key] = child;
            if (members_ && depth_ == 1)
                (*members_)[key] = std::string_view(input_).substr(begin, pos_ - begin);
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                error = "expected ',' or '}' in object";
                return false;
            }
        }
    }

    bool parse_array(Value& value, std::string& error) {
        if (!consume('[')) {
            error = "expected array";
            return false;
        }
        value = Value{};
        value.type = Value::Type::Array;
        skip_ws();
        if (consume(']')) {
            return true;
        }
        for (;;) {
            Value child;
            if (!parse_value(child, error)) {
                return false;
            }
            value.a.push_back(child);
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                error = "expected ',' or ']' in array";
                return false;
            }
        }
    }

    bool parse_string(std::string& value, std::string& error) {
        if (!consume('"')) {
            error = "expected string at byte " + std::to_string(pos_);
            return false;
        }
        value.clear();
        while (pos_ < input_.size()) {
            const char c = input_[pos_++];
            if (static_cast<unsigned char>(c) < 0x20) {
                error = "unescaped control character";
                return false;
            }
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                value.push_back(c);
                continue;
            }
            if (pos_ >= input_.size()) {
                error = "unterminated string escape";
                return false;
            }
            const char e = input_[pos_++];
            switch (e) {
            case '"':
            case '\\':
            case '/':
                value.push_back(e);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'u':
                if (!parse_unicode_escape(value, error)) {
                    return false;
                }
                break;
            default:
                error = "invalid string escape";
                return false;
            }
        }
        error = "unterminated string";
        return false;
    }

    bool hex4(unsigned& code, std::string& error) {
        if (pos_ + 4 > input_.size()) {
            error = "short Unicode escape";
            return false;
        }
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[pos_++];
            const int digit = c >= '0' && c <= '9'   ? c - '0'
                              : c >= 'a' && c <= 'f' ? c - 'a' + 10
                              : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                     : -1;
            if (digit < 0) {
                error = "invalid Unicode escape";
                return false;
            }
            code = (code << 4) | static_cast<unsigned>(digit);
        }
        return true;
    }

    bool parse_unicode_escape(std::string& value, std::string& error) {
        unsigned code;
        if (!hex4(code, error))
            return false;
        if (code >= 0xd800 && code <= 0xdbff) {
            if (pos_ + 2 > input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u') {
                error = "missing low surrogate";
                return false;
            }
            pos_ += 2;
            unsigned low;
            if (!hex4(low, error))
                return false;
            if (low < 0xdc00 || low > 0xdfff) {
                error = "invalid low surrogate";
                return false;
            }
            code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
        } else if (code >= 0xdc00 && code <= 0xdfff) {
            error = "unpaired low surrogate";
            return false;
        }
        if (code <= 0x7f)
            value += static_cast<char>(code);
        else {
            if (code <= 0x7ff)
                value += static_cast<char>(0xc0 | (code >> 6));
            else {
                if (code <= 0xffff)
                    value += static_cast<char>(0xe0 | (code >> 12));
                else {
                    value += static_cast<char>(0xf0 | (code >> 18));
                    value += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
                }
                value += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            }
            value += static_cast<char>(0x80 | (code & 0x3f));
        }
        return true;
    }

    bool parse_number(Value& value, std::string& error) {
        const auto begin = pos_;
        if (input_[pos_] == '-')
            ++pos_;
        const auto digits = [&] {
            const auto start = pos_;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9')
                ++pos_;
            return start != pos_;
        };
        if (pos_ < input_.size() && input_[pos_] == '0')
            ++pos_;
        else if (!digits()) {
            error = "invalid JSON number";
            return false;
        }
        if (pos_ < input_.size() && input_[pos_] == '.') {
            ++pos_;
            if (!digits()) {
                error = "missing fractional digits";
                return false;
            }
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-'))
                ++pos_;
            if (!digits()) {
                error = "missing exponent";
                return false;
            }
        }
        const auto token = input_.substr(begin, pos_ - begin);
        std::istringstream stream(token);
        stream.imbue(std::locale::classic());
        double number;
        if (!(stream >> number) || !std::isfinite(number)) {
            error = "JSON number out of range";
            return false;
        }
        value = Value(number);
        return true;
    }
};

// Reuse unchanged members from the input so opaque numbers retain their precision.
inline std::string dump_object_members(const Value& value, const std::string& source,
                                       const Value* changes = nullptr,
                                       const Value* omit_defaults = nullptr) {
    if (!value.is_object() || source.empty())
        return dump(value);
    Parser::MemberSources members;
    const auto old = Parser(source, &members).parse();
    if (!old.is_object())
        return dump(value);
    std::string out = "{";
    for (const auto& [key, member] : value.as_object()) {
        const auto encoded = dump(member);
        if (omit_defaults && omit_defaults->contains(key) &&
            encoded == dump(omit_defaults->at(key)))
            continue;
        if (out.size() > 1)
            out += omit_defaults ? "," : ", ";
        const auto it = members.find(key);
        if (it != members.end() && (!changes || !changes->contains(key)) &&
            encoded == dump(old.at(key)))
            out.append(it->second);
        else
            out += escape_string(key) + ":" + encoded;
    }
    out += '}';
    return out;
}

inline bool parse_checked(const std::string& input, Value& out, std::string& error) {
    error.clear();
    Value parsed;
    if (!Parser(input).parse(parsed, error)) {
        out = Value{};
        return false;
    }
    out = std::move(parsed);
    return true;
}
inline Value parse(const std::string& input) {
    return Parser(input).parse();
}
}  // namespace json
