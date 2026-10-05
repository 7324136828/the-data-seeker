#include "Assertions.h"
#include <nlohmann/json.hpp>
#include <string>
#include <sstream>
#include <algorithm>

namespace native_app {

using json = nlohmann::json;

static bool ExtractJsonPath(const json& root, const std::string& path, json& outVal) {
    if (root.is_null()) return false;
    std::string clean = path;
    if (clean.rfind("$.", 0) == 0) clean = clean.substr(2);
    else if (clean.rfind("$", 0) == 0) clean = clean.substr(1);

    // Normalize [0] to .0
    std::string normalized;
    for (size_t i = 0; i < clean.length(); ++i) {
        if (clean[i] == '[') {
            normalized += '.';
        } else if (clean[i] == ']') {
            // skip
        } else {
            normalized += clean[i];
        }
    }
    if (normalized.rfind(".", 0) == 0) normalized = normalized.substr(1);

    std::stringstream ss(normalized);
    std::string segment;
    json current = root;

    while (std::getline(ss, segment, '.')) {
        if (segment.empty()) continue;
        if (current.is_array()) {
            try {
                size_t idx = std::stoul(segment);
                if (idx < current.size()) {
                    current = current[idx];
                } else {
                    return false;
                }
            } catch (...) {
                return false;
            }
        } else if (current.is_object()) {
            if (current.contains(segment)) {
                current = current[segment];
            } else {
                return false;
            }
        } else {
            return false;
        }
    }
    outVal = current;
    return true;
}

std::vector<TestResult> AssertionsEvaluator::Evaluate(
    const std::vector<TestAssertion>& tests,
    int statusCode,
    double latencyMs,
    const std::vector<KeyValuePair>& headers,
    const std::string& bodyText
) {
    std::vector<TestResult> results;
    json parsedJson = nullptr;
    bool hasParsedJson = false;

    for (const auto& test : tests) {
        if (!test.enabled) continue;
        TestResult tr;
        tr.type = test.type;
        tr.name = test.name.empty() ? test.type : test.name;
        tr.expected = test.expected;

        try {
            if (test.type == "status_code" || test.type == "status_200") {
                int exp = test.type == "status_200" ? 200 : std::stoi(test.expected.empty() ? "200" : test.expected);
                tr.expected = std::to_string(exp);
                tr.actual = std::to_string(statusCode);
                tr.passed = (statusCode == exp);
            } else if (test.type == "status_200_or_201") {
                tr.expected = "200 or 201";
                tr.actual = std::to_string(statusCode);
                tr.passed = (statusCode == 200 || statusCode == 201);
            } else if (test.type == "status_2xx") {
                tr.expected = "2xx";
                tr.actual = std::to_string(statusCode);
                tr.passed = (statusCode >= 200 && statusCode < 300);
            } else if (test.type == "response_time_ms" || test.type == "time_lt_500") {
                double exp = test.type == "time_lt_500" ? 500.0 : std::stod(test.expected.empty() ? "500" : test.expected);
                tr.expected = "< " + std::to_string(static_cast<int>(exp)) + "ms";
                tr.actual = std::to_string(static_cast<int>(latencyMs)) + "ms";
                tr.passed = (latencyMs < exp);
            } else if (test.type == "body_contains") {
                tr.actual = bodyText.substr(0, 100);
                tr.passed = (bodyText.find(test.expected) != std::string::npos);
            } else if (test.type == "header_exists") {
                std::string expHdr = test.header.empty() ? test.expected : test.header;
                tr.expected = expHdr;
                bool found = false;
                for (const auto& h : headers) {
                    if (_stricmp(h.key.c_str(), expHdr.c_str()) == 0) {
                        found = true;
                        tr.actual = h.value;
                        break;
                    }
                }
                tr.passed = found;
            } else if (test.type == "json_field" || test.type == "json_equals") {
                if (!hasParsedJson) {
                    try {
                        parsedJson = json::parse(bodyText);
                        hasParsedJson = true;
                    } catch (...) {
                        hasParsedJson = false;
                    }
                }

                if (!hasParsedJson) {
                    tr.passed = false;
                    tr.message = "Failed to parse response body as JSON";
                } else {
                    std::string field = test.field.empty() ? test.expected : test.field;
                    json val;
                    bool exists = ExtractJsonPath(parsedJson, field, val);
                    if (test.type == "json_field") {
                        tr.expected = field + " exists";
                        tr.passed = exists;
                        tr.actual = exists ? val.dump() : "<not found>";
                    } else { // json_equals
                        tr.expected = test.expected;
                        if (exists) {
                            tr.actual = val.is_string() ? val.get<std::string>() : val.dump();
                            try {
                                json expJson = json::parse(test.expected);
                                tr.passed = (val == expJson);
                            } catch (...) {
                                tr.passed = (tr.actual == test.expected);
                            }
                        } else {
                            tr.passed = false;
                            tr.actual = "<not found>";
                        }
                    }
                }
            } else {
                tr.passed = false;
                tr.message = "Unknown assertion type: " + test.type;
            }

            if (tr.message.empty()) {
                tr.message = tr.passed ? "OK" : ("Expected " + tr.expected + ", got " + tr.actual);
            }
        } catch (const std::exception& ex) {
            tr.passed = false;
            tr.message = std::string("Assertion error: ") + ex.what();
        }

        results.push_back(tr);
    }

    return results;
}

} // namespace native_app
