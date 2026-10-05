#pragma once

#include "Types.h"
#include <vector>

namespace native_app {

class AssertionsEvaluator {
public:
    static std::vector<TestResult> Evaluate(
        const std::vector<TestAssertion>& tests,
        int statusCode,
        double latencyMs,
        const std::vector<KeyValuePair>& headers,
        const std::string& bodyText
    );
};

} // namespace native_app
