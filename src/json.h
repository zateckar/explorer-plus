#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A small JSON reader and the string quoting a writer needs (the Azure DevOps REST store).
// Pure; numbers keep their text, so 64-bit sizes and ids read back exactly.
namespace json {

struct Value {
    enum Type : uint8_t { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    std::string text;                                       // String: the decoded UTF-8; Number: as written
    std::vector<Value> items;                               // Array
    std::vector<std::pair<std::string, Value>> members;     // Object, in document order

    const Value& operator[](std::string_view key) const;    // a Null value when absent or not an object
    const Value& operator[](size_t i) const;                // a Null value when out of range
    size_t size() const { return type == Array ? items.size() : type == Object ? members.size() : 0; }
    bool is_null() const { return type == Null; }
    const std::string& str() const;                         // "" unless a String
    uint64_t u64() const;                                   // 0 unless a non-negative integer Number
};

bool parse(std::string_view text, Value& out, std::string* error = nullptr);
std::string quote(std::string_view utf8);                   // "..." with the escapes JSON needs

} // namespace json
