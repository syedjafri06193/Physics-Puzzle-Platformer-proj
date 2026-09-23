// A minimal JSON reader.
//
// Levels are JSON because they are hand-edited and diffed, and a diffable
// level format is what makes a level's history reviewable. There is no JSON
// in the standard library and the solver has no dependencies, so this is
// ~250 lines rather than a vendored library — the level loader is the only
// consumer and it needs objects, arrays, numbers, strings and bools.
//
// Numbers are parsed with std::strtod and stored as double. That matters:
// a level file is read once at load, before any physics runs, so the parse
// is not part of the simulation and does not affect determinism. What DOES
// affect determinism is that the same file must produce the same floats on
// every load, which strtod guarantees and a hand-rolled parser would not.

#pragma once

#include <map>
#include <string>
#include <vector>

namespace ppp {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;

    Type GetType() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsObject() const { return type_ == Type::Object; }
    bool IsArray() const { return type_ == Type::Array; }

    /// Parse. On failure returns false and fills `error` with a message that
    /// names the byte offset — a level file that fails to load should say
    /// where, not just "invalid".
    static bool Parse(const std::string& text, Json* out, std::string* error);

    // --- accessors ----------------------------------------------------
    // All of these are total: a missing key or a type mismatch yields the
    // supplied default. A level file with a missing field should load with
    // sensible defaults and be caught by the validator (doc 9.3), not
    // throw halfway through construction and leave a half-built level.

    bool Has(const std::string& key) const;
    const Json& operator[](const std::string& key) const;
    const Json& operator[](size_t index) const;
    size_t Size() const;

    double AsNumber(double fallback = 0.0) const;
    float AsFloat(float fallback = 0.0f) const { return static_cast<float>(AsNumber(fallback)); }
    int AsInt(int fallback = 0) const;
    bool AsBool(bool fallback = false) const;
    const std::string& AsString() const;
    std::string AsString(const std::string& fallback) const;

    /// Object keys in sorted order. std::map, not unordered_map: iterating
    /// an unordered container would make load order depend on the hash
    /// function, and load order decides particle indices, which decides the
    /// constraint solve order (doc 6.2).
    const std::map<std::string, Json>& Members() const { return object_; }
    const std::vector<Json>& Elements() const { return array_; }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Json> array_;
    std::map<std::string, Json> object_;

    friend struct JsonParser;
};

/// Escapes a string for JSON output. Used by the level writer in the tools.
std::string JsonEscape(const std::string& s);

/// Formats a float with enough digits to round-trip exactly. A level written
/// out and read back in must produce bit-identical floats or every recorded
/// replay for it becomes invalid.
std::string JsonNumber(double v);

}  // namespace ppp
