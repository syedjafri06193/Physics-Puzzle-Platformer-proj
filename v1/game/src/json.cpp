#include "ppp/json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ppp {
namespace {

const Json& NullValue() {
    static const Json null_value;
    return null_value;
}

}  // namespace

bool Json::Has(const std::string& key) const {
    return type_ == Type::Object && object_.find(key) != object_.end();
}

const Json& Json::operator[](const std::string& key) const {
    if (type_ != Type::Object) return NullValue();
    auto it = object_.find(key);
    return it == object_.end() ? NullValue() : it->second;
}

const Json& Json::operator[](size_t index) const {
    if (type_ != Type::Array || index >= array_.size()) return NullValue();
    return array_[index];
}

size_t Json::Size() const {
    if (type_ == Type::Array) return array_.size();
    if (type_ == Type::Object) return object_.size();
    return 0;
}

double Json::AsNumber(double fallback) const {
    if (type_ == Type::Number) return number_;
    if (type_ == Type::Bool) return bool_ ? 1.0 : 0.0;
    return fallback;
}

int Json::AsInt(int fallback) const {
    if (type_ != Type::Number && type_ != Type::Bool) return fallback;
    return static_cast<int>(AsNumber(static_cast<double>(fallback)));
}

bool Json::AsBool(bool fallback) const {
    if (type_ == Type::Bool) return bool_;
    if (type_ == Type::Number) return number_ != 0.0;
    return fallback;
}

const std::string& Json::AsString() const {
    static const std::string empty;
    return type_ == Type::String ? string_ : empty;
}

std::string Json::AsString(const std::string& fallback) const {
    return type_ == Type::String ? string_ : fallback;
}

// ---------------------------------------------------------------- parser

struct JsonParser {
    const std::string& text;
    size_t pos = 0;
    std::string error;

    explicit JsonParser(const std::string& t) : text(t) {}

    bool Fail(const char* what) {
        if (error.empty()) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "at byte %zu: %s", pos, what);
            error = buf;
        }
        return false;
    }

    void SkipWhitespace() {
        while (pos < text.size()) {
            const char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos;
            } else if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '/') {
                // Line comments are not JSON. They are allowed here because
                // levels are hand-authored and a level with no room for a
                // note about why a box is where it is gets one anyway, in a
                // commit message nobody reads.
                while (pos < text.size() && text[pos] != '\n') ++pos;
            } else {
                return;
            }
        }
    }

    bool Literal(const char* word, Json::Type type, bool value) {
        const size_t len = std::strlen(word);
        if (text.compare(pos, len, word) != 0) return Fail("unrecognised literal");
        pos += len;
        out_.type_ = type;
        out_.bool_ = value;
        return true;
    }

    bool ParseValue(Json* out);
    bool ParseString(std::string* out);
    bool ParseNumber(Json* out);
    bool ParseArray(Json* out);
    bool ParseObject(Json* out);

    Json out_;
};

bool JsonParser::ParseString(std::string* out) {
    if (pos >= text.size() || text[pos] != '"') return Fail("expected a string");
    ++pos;
    out->clear();
    while (pos < text.size()) {
        const char c = text[pos++];
        if (c == '"') return true;
        if (c != '\\') {
            out->push_back(c);
            continue;
        }
        if (pos >= text.size()) return Fail("string ends inside an escape");
        const char e = text[pos++];
        switch (e) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                if (pos + 4 > text.size()) return Fail("truncated \\u escape");
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    const char h = text[pos + i];
                    code <<= 4;
                    if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                    else return Fail("bad hex digit in \\u escape");
                }
                pos += 4;
                // UTF-8 encode. Surrogate pairs are not joined: level files
                // hold identifiers and English display strings, and a lone
                // surrogate here would be a typo, not a plane-1 character.
                if (code < 0x80) {
                    out->push_back(static_cast<char>(code));
                } else if (code < 0x800) {
                    out->push_back(static_cast<char>(0xC0 | (code >> 6)));
                    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
                } else {
                    out->push_back(static_cast<char>(0xE0 | (code >> 12)));
                    out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
                }
                break;
            }
            default: return Fail("unknown escape character");
        }
    }
    return Fail("unterminated string");
}

bool JsonParser::ParseNumber(Json* out) {
    const char* start = text.c_str() + pos;
    char* end = nullptr;
    const double v = std::strtod(start, &end);
    if (end == start) return Fail("expected a number");
    pos += static_cast<size_t>(end - start);
    out->type_ = Json::Type::Number;
    out->number_ = v;
    return true;
}

bool JsonParser::ParseArray(Json* out) {
    ++pos;  // '['
    out->type_ = Json::Type::Array;
    SkipWhitespace();
    if (pos < text.size() && text[pos] == ']') {
        ++pos;
        return true;
    }
    while (true) {
        Json element;
        if (!ParseValue(&element)) return false;
        out->array_.push_back(std::move(element));
        SkipWhitespace();
        if (pos >= text.size()) return Fail("array is not closed");
        if (text[pos] == ',') {
            ++pos;
            SkipWhitespace();
            // A trailing comma before ']' is accepted. Hand-edited arrays of
            // boxes grow and shrink constantly and a trailing comma is the
            // single most common hand-edit mistake.
            if (pos < text.size() && text[pos] == ']') {
                ++pos;
                return true;
            }
            continue;
        }
        if (text[pos] == ']') {
            ++pos;
            return true;
        }
        return Fail("expected ',' or ']' in array");
    }
}

bool JsonParser::ParseObject(Json* out) {
    ++pos;  // '{'
    out->type_ = Json::Type::Object;
    SkipWhitespace();
    if (pos < text.size() && text[pos] == '}') {
        ++pos;
        return true;
    }
    while (true) {
        SkipWhitespace();
        std::string key;
        if (!ParseString(&key)) return false;
        SkipWhitespace();
        if (pos >= text.size() || text[pos] != ':') return Fail("expected ':' after a key");
        ++pos;
        Json value;
        if (!ParseValue(&value)) return false;
        out->object_[key] = std::move(value);
        SkipWhitespace();
        if (pos >= text.size()) return Fail("object is not closed");
        if (text[pos] == ',') {
            ++pos;
            SkipWhitespace();
            if (pos < text.size() && text[pos] == '}') {
                ++pos;
                return true;
            }
            continue;
        }
        if (text[pos] == '}') {
            ++pos;
            return true;
        }
        return Fail("expected ',' or '}' in object");
    }
}

bool JsonParser::ParseValue(Json* out) {
    SkipWhitespace();
    if (pos >= text.size()) return Fail("unexpected end of input");
    const char c = text[pos];
    switch (c) {
        case '{': return ParseObject(out);
        case '[': return ParseArray(out);
        case '"': {
            std::string s;
            if (!ParseString(&s)) return false;
            out->type_ = Json::Type::String;
            out->string_ = std::move(s);
            return true;
        }
        case 't':
            if (text.compare(pos, 4, "true") != 0) return Fail("expected 'true'");
            pos += 4;
            out->type_ = Json::Type::Bool;
            out->bool_ = true;
            return true;
        case 'f':
            if (text.compare(pos, 5, "false") != 0) return Fail("expected 'false'");
            pos += 5;
            out->type_ = Json::Type::Bool;
            out->bool_ = false;
            return true;
        case 'n':
            if (text.compare(pos, 4, "null") != 0) return Fail("expected 'null'");
            pos += 4;
            out->type_ = Json::Type::Null;
            return true;
        default: return ParseNumber(out);
    }
}

bool Json::Parse(const std::string& text, Json* out, std::string* error) {
    JsonParser parser(text);
    Json value;
    if (!parser.ParseValue(&value)) {
        if (error) *error = parser.error;
        return false;
    }
    parser.SkipWhitespace();
    if (parser.pos != text.size()) {
        if (error) *error = "trailing characters after the top-level value";
        return false;
    }
    *out = std::move(value);
    return true;
}

// ---------------------------------------------------------------- writing

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
    return out;
}

std::string JsonNumber(double v) {
    // %.17g round-trips every double exactly. %.9g would round-trip a float,
    // but a level's numbers are read into floats *after* parsing as double,
    // and shortening here would quietly change the float that comes out.
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

}  // namespace ppp
