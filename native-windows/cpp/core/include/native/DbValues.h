#pragma once
#include "Types.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <stdexcept>
#include <algorithm>

namespace native_app {
inline std::string BlobHex(const std::string& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string result; result.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}
inline nlohmann::json DbValueJson(const DbValue& value) {
    using json = nlohmann::json;
    if (value.type == DbValueType::Null) return nullptr;
    if (value.type == DbValueType::Text) return value.text;
    if (value.type == DbValueType::Blob) return json{{"$binary", BlobHex(value.text)}, {"$encoding", "hex"}};
    const auto parsed = json::parse(value.text);
    if (value.type == DbValueType::Integer && !parsed.is_number_integer()) throw std::runtime_error("Enter a valid integer value.");
    if (value.type == DbValueType::Integer && parsed.is_number_unsigned() && parsed.get<uint64_t>() > static_cast<uint64_t>(INT64_MAX)) throw std::runtime_error("Integer value exceeds the signed 64-bit database range.");
    if (value.type == DbValueType::Real && (!parsed.is_number() || !std::isfinite(parsed.get<double>()))) throw std::runtime_error("Enter a finite numeric value.");
    if (value.type == DbValueType::Boolean && !parsed.is_boolean()) throw std::runtime_error("Enter true or false for a boolean value.");
    return parsed;
}
inline DbValue DbValueFromJson(const nlohmann::json& value) {
    if (value.is_object() && value.size() == 2 && value.value("$encoding", std::string()) == "hex" && value.contains("$binary") && value["$binary"].is_string()) {
        const auto encoded = value["$binary"].get<std::string>();
        if (encoded.size() % 2) throw std::runtime_error("Binary hex value must contain complete byte pairs.");
        auto digit = [](char c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; throw std::runtime_error("Binary hex value contains an invalid digit."); };
        std::string bytes; bytes.reserve(encoded.size()/2);
        for (size_t i=0; i<encoded.size(); i+=2) bytes += static_cast<char>((digit(encoded[i])<<4) | digit(encoded[i+1]));
        return {DbValueType::Blob,bytes};
    }
    if (value.is_null()) return {DbValueType::Null, ""};
    if (value.is_string()) return {DbValueType::Text, value.get<std::string>()};
    if (value.is_boolean()) return {DbValueType::Boolean, value.dump()};
    if (value.is_number_integer()) return {DbValueType::Integer, value.dump()};
    if (value.is_number_float()) return {DbValueType::Real, value.dump()};
    return {DbValueType::Json, value.dump()};
}
inline std::string DbValueDisplay(const DbValue& value) {
    if (value.type == DbValueType::Null) return "NULL";
    if (value.type == DbValueType::Blob) return "0x" + BlobHex(value.text);
    return value.text;
}
inline std::string UniqueColumnLabel(const std::string& raw, const std::vector<std::string>& labels) {
    const std::string base = raw.empty() ? "Column" : raw;
    std::string candidate = base;
    for (int number = 2; std::find(labels.begin(), labels.end(), candidate) != labels.end(); ++number) candidate = base + " (" + std::to_string(number) + ")";
    return candidate;
}
inline TypedRow TextRow(const std::map<std::string, std::string>& row) {
    TypedRow result; for (const auto& item : row) result[item.first] = {DbValueType::Text, item.second}; return result;
}
} // namespace native_app
