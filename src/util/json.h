/*
 * TestHub - 轻量 JSON 库
 * 零依赖的 JSON 值类型、解析器与序列化器（支持 RFC 8259 子集：
 * null/bool/number/string/array/object，字符串转义含 \uXXXX 与代理对）。
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <initializer_list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace testhub {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() : type_(Type::Null) {}
    Json(std::nullptr_t) : type_(Type::Null) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int n) : type_(Type::Number), number_(n) {}
    Json(long n) : type_(Type::Number), number_(static_cast<double>(n)) {}
    Json(long long n) : type_(Type::Number), number_(static_cast<double>(n)) {}
    Json(unsigned n) : type_(Type::Number), number_(n) {}
    Json(unsigned long n) : type_(Type::Number), number_(static_cast<double>(n)) {}
    Json(unsigned long long n) : type_(Type::Number), number_(static_cast<double>(n)) {}
    Json(double d) : type_(Type::Number), number_(d) {}
    Json(const char* s) : type_(Type::String), string_(s ? s : "") {}
    Json(const std::string& s) : type_(Type::String), string_(s) {}
    Json(std::string&& s) : type_(Type::String), string_(std::move(s)) {}
    Json(const Array& a) : type_(Type::Array), array_(std::make_shared<Array>(a)) {}
    Json(Array&& a) : type_(Type::Array), array_(std::make_shared<Array>(std::move(a))) {}
    Json(const Object& o) : type_(Type::Object), object_(std::make_shared<Object>(o)) {}
    Json(Object&& o) : type_(Type::Object), object_(std::make_shared<Object>(std::move(o))) {}

    template <typename T>
    Json(const std::vector<T>& values) : type_(Type::Array), array_(std::make_shared<Array>()) {
        for (const auto& v : values) array_->emplace_back(v);
    }

    template <typename T>
    Json(const std::map<std::string, T>& values) : type_(Type::Object), object_(std::make_shared<Object>()) {
        for (const auto& kv : values) (*object_)[kv.first] = Json(kv.second);
    }

    static Json array() { return Json(Array{}); }
    static Json object() { return Json(Object{}); }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool(bool def = false) const { return isBool() ? bool_ : def; }
    double asNumber(double def = 0.0) const { return isNumber() ? number_ : def; }
    int asInt(int def = 0) const { return isNumber() ? static_cast<int>(number_) : def; }
    long long asInt64(long long def = 0) const { return isNumber() ? static_cast<long long>(number_) : def; }
    const std::string& asString() const {
        static const std::string empty;
        return isString() ? string_ : empty;
    }
    std::string asString(const std::string& def) const { return isString() ? string_ : def; }

    const Array& asArray() const {
        static const Array empty;
        return isArray() ? *array_ : empty;
    }
    const Object& asObject() const {
        static const Object empty;
        return isObject() ? *object_ : empty;
    }

    size_t size() const {
        if (isArray()) return array_->size();
        if (isObject()) return object_->size();
        return 0;
    }
    bool empty() const { return size() == 0; }

    bool contains(const std::string& key) const {
        return isObject() && object_->find(key) != object_->end();
    }

    // 只读访问：不存在返回 null 值
    const Json& operator[](const std::string& key) const {
        static const Json nullValue;
        if (!isObject()) return nullValue;
        auto it = object_->find(key);
        return it == object_->end() ? nullValue : it->second;
    }
    const Json& operator[](size_t index) const {
        static const Json nullValue;
        if (!isArray() || index >= array_->size()) return nullValue;
        return (*array_)[index];
    }
    // 整型字面量索引：避免 j[0] 被解析为 operator[](const char*) 而构造空指针字符串
    const Json& operator[](int index) const {
        static const Json nullValue;
        if (index < 0) return nullValue;
        return (*this)[static_cast<size_t>(index)];
    }
    Json& operator[](size_t index) {
        if (!isArray()) {
            type_ = Type::Array;
            array_ = std::make_shared<Array>();
        }
        detachArray();
        if (index >= array_->size()) array_->resize(index + 1);
        return (*array_)[index];
    }
    Json& operator[](int index) {
        if (index < 0) throw std::out_of_range("Negative JSON array index");
        return (*this)[static_cast<size_t>(index)];
    }

    const Json& get(const std::string& key) const { return (*this)[key]; }

    // 可写访问：自动转为对象
    Json& operator[](const std::string& key) {
        if (!isObject()) {
            type_ = Type::Object;
            object_ = std::make_shared<Object>();
        }
        detachObject();
        return (*object_)[key];
    }
    Json& operator[](const char* key) { return (*this)[std::string(key)]; }

    Json& set(const std::string& key, const Json& value) {
        (*this)[key] = value;
        return *this;
    }

    Json& push(const Json& value) {
        if (!isArray()) {
            type_ = Type::Array;
            array_ = std::make_shared<Array>();
        }
        detachArray();
        array_->push_back(value);
        return *this;
    }

    void erase(const std::string& key) {
        if (isObject()) {
            detachObject();
            object_->erase(key);
        }
    }

    bool operator==(const Json& other) const {
        if (type_ != other.type_) return false;
        switch (type_) {
            case Type::Null: return true;
            case Type::Bool: return bool_ == other.bool_;
            case Type::Number: return number_ == other.number_;
            case Type::String: return string_ == other.string_;
            case Type::Array: return *array_ == *other.array_;
            case Type::Object: return *object_ == *other.object_;
        }
        return false;
    }
    bool operator!=(const Json& other) const { return !(*this == other); }

    // ------------------------------------------------------------
    // 序列化
    // ------------------------------------------------------------
    std::string dump(int indent = -1) const {
        std::string out;
        dumpTo(out, indent, 0);
        return out;
    }

    static std::string escape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('"');
        for (unsigned char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out.push_back(static_cast<char>(c));
                    }
            }
        }
        out.push_back('"');
        return out;
    }

    // ------------------------------------------------------------
    // 解析
    // ------------------------------------------------------------
    struct ParseError : std::runtime_error {
        size_t position;
        ParseError(const std::string& msg, size_t pos)
            : std::runtime_error(msg + " at position " + std::to_string(pos)), position(pos) {}
    };

    static Json parse(const std::string& text) {
        Parser p(text);
        p.skipWs();
        Json value = p.parseValue();
        p.skipWs();
        if (!p.eof()) p.fail("Trailing characters");
        return value;
    }

    // 不抛异常版本：失败时 error 非空，返回 null
    static Json tryParse(const std::string& text, std::string* error = nullptr) {
        try {
            return parse(text);
        } catch (const std::exception& e) {
            if (error) *error = e.what();
            return Json();
        }
    }

private:
    Type type_;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::shared_ptr<Array> array_;
    std::shared_ptr<Object> object_;

    // 写时复制：容器由 shared_ptr 持有以便廉价拷贝，写入前分离
    void detachArray() {
        if (array_.use_count() > 1) array_ = std::make_shared<Array>(*array_);
    }
    void detachObject() {
        if (object_.use_count() > 1) object_ = std::make_shared<Object>(*object_);
    }

    static void indentTo(std::string& out, int indent, int depth) {
        if (indent < 0) return;
        out.push_back('\n');
        out.append(static_cast<size_t>(indent * depth), ' ');
    }

    void dumpTo(std::string& out, int indent, int depth) const {
        switch (type_) {
            case Type::Null: out += "null"; break;
            case Type::Bool: out += bool_ ? "true" : "false"; break;
            case Type::Number: {
                if (std::isnan(number_) || std::isinf(number_)) {
                    out += "null";
                } else if (std::floor(number_) == number_ && std::fabs(number_) < 1e15) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(number_));
                    out += buf;
                } else {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.17g", number_);
                    out += buf;
                }
                break;
            }
            case Type::String: out += escape(string_); break;
            case Type::Array: {
                out.push_back('[');
                bool first = true;
                for (const auto& v : *array_) {
                    if (!first) out.push_back(',');
                    first = false;
                    indentTo(out, indent, depth + 1);
                    v.dumpTo(out, indent, depth + 1);
                }
                if (!array_->empty()) indentTo(out, indent, depth);
                out.push_back(']');
                break;
            }
            case Type::Object: {
                out.push_back('{');
                bool first = true;
                for (const auto& kv : *object_) {
                    if (!first) out.push_back(',');
                    first = false;
                    indentTo(out, indent, depth + 1);
                    out += escape(kv.first);
                    out.push_back(':');
                    if (indent >= 0) out.push_back(' ');
                    kv.second.dumpTo(out, indent, depth + 1);
                }
                if (!object_->empty()) indentTo(out, indent, depth);
                out.push_back('}');
                break;
            }
        }
    }

    class Parser {
    public:
        explicit Parser(const std::string& text) : text_(text) {}

        bool eof() const { return pos_ >= text_.size(); }

        void skipWs() {
            while (!eof()) {
                char c = text_[pos_];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
                else break;
            }
        }

        [[noreturn]] void fail(const std::string& msg) const { throw ParseError(msg, pos_); }

        Json parseValue() {
            if (eof()) fail("Unexpected end of input");
            char c = text_[pos_];
            switch (c) {
                case '{': return parseObject();
                case '[': return parseArray();
                case '"': return Json(parseString());
                case 't': expectLiteral("true"); return Json(true);
                case 'f': expectLiteral("false"); return Json(false);
                case 'n': expectLiteral("null"); return Json();
                default:
                    if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
                    fail(std::string("Unexpected character '") + c + "'");
            }
        }

    private:
        const std::string& text_;
        size_t pos_ = 0;
        int depth_ = 0;

        void expectLiteral(const char* lit) {
            size_t len = std::char_traits<char>::length(lit);
            if (text_.compare(pos_, len, lit) != 0) fail(std::string("Invalid literal, expected ") + lit);
            pos_ += len;
        }

        Json parseNumber() {
            size_t start = pos_;
            if (text_[pos_] == '-') ++pos_;
            if (eof()) fail("Invalid number");
            if (text_[pos_] == '0') {
                ++pos_;
            } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
                while (!eof() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
            } else {
                fail("Invalid number");
            }
            if (!eof() && text_[pos_] == '.') {
                ++pos_;
                if (eof() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) fail("Invalid fraction");
                while (!eof() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
            }
            if (!eof() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
                ++pos_;
                if (!eof() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
                if (eof() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) fail("Invalid exponent");
                while (!eof() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
            }
            return Json(std::strtod(text_.c_str() + start, nullptr));
        }

        static void appendUtf8(std::string& out, uint32_t cp) {
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        uint32_t parseHex4() {
            if (pos_ + 4 > text_.size()) fail("Invalid unicode escape");
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i) {
                char c = text_[pos_++];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
                else fail("Invalid hex digit");
            }
            return v;
        }

        std::string parseString() {
            ++pos_;  // opening quote
            std::string out;
            while (true) {
                if (eof()) fail("Unterminated string");
                char c = text_[pos_++];
                if (c == '"') break;
                if (c == '\\') {
                    if (eof()) fail("Unterminated escape");
                    char e = text_[pos_++];
                    switch (e) {
                        case '"': out.push_back('"'); break;
                        case '\\': out.push_back('\\'); break;
                        case '/': out.push_back('/'); break;
                        case 'b': out.push_back('\b'); break;
                        case 'f': out.push_back('\f'); break;
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        case 'u': {
                            uint32_t cp = parseHex4();
                            if (cp >= 0xD800 && cp <= 0xDBFF) {
                                if (pos_ + 1 < text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                                    pos_ += 2;
                                    uint32_t low = parseHex4();
                                    if (low >= 0xDC00 && low <= 0xDFFF) {
                                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                    } else {
                                        fail("Invalid surrogate pair");
                                    }
                                } else {
                                    fail("Unpaired surrogate");
                                }
                            }
                            appendUtf8(out, cp);
                            break;
                        }
                        default: fail("Invalid escape sequence");
                    }
                } else if (static_cast<unsigned char>(c) < 0x20) {
                    fail("Control character in string");
                } else {
                    out.push_back(c);
                }
            }
            return out;
        }

        Json parseArray() {
            if (++depth_ > 512) fail("Nesting too deep");
            ++pos_;  // [
            Array arr;
            skipWs();
            if (!eof() && text_[pos_] == ']') {
                ++pos_;
                --depth_;
                return Json(std::move(arr));
            }
            while (true) {
                skipWs();
                arr.push_back(parseValue());
                skipWs();
                if (eof()) fail("Unterminated array");
                char c = text_[pos_++];
                if (c == ']') break;
                if (c != ',') fail("Expected ',' or ']'");
            }
            --depth_;
            return Json(std::move(arr));
        }

        Json parseObject() {
            if (++depth_ > 512) fail("Nesting too deep");
            ++pos_;  // {
            Object obj;
            skipWs();
            if (!eof() && text_[pos_] == '}') {
                ++pos_;
                --depth_;
                return Json(std::move(obj));
            }
            while (true) {
                skipWs();
                if (eof() || text_[pos_] != '"') fail("Expected string key");
                std::string key = parseString();
                skipWs();
                if (eof() || text_[pos_] != ':') fail("Expected ':'");
                ++pos_;
                skipWs();
                obj[key] = parseValue();
                skipWs();
                if (eof()) fail("Unterminated object");
                char c = text_[pos_++];
                if (c == '}') break;
                if (c != ',') fail("Expected ',' or '}'");
            }
            --depth_;
            return Json(std::move(obj));
        }
    };
};

} // namespace testhub
