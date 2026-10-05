#pragma once
#include "Types.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <vector>

namespace native_app {
class HttpEngine;
struct ScriptEntry { std::string name; std::string code; };
struct ScriptContext {
    // Inherited entries, in collection-to-folder order. Execute adds the request.
    std::vector<ScriptEntry> preRequestScripts;
    std::vector<ScriptEntry> postResponseScripts;
    nlohmann::json globals = nlohmann::json::object();
    nlohmann::json collection = nlohmann::json::object();
    nlohmann::json environment = nlohmann::json::object();
    nlohmann::json data = nlohmann::json::object();
    nlohmann::json local = nlohmann::json::object();
    bool environmentEnabled = false;
    bool collectionEnabled = false;
    int iteration = 0;
};
struct ScriptExecutionResult {
    ApiRequest request;
    ApiResponse response;
    nlohmann::json changes = nlohmann::json::object();
    nlohmann::json local = nlohmann::json::object();
    bool variablesChanged = false;
};
class ScriptRuntime {
public:
    // The original runScriptFrame payload. Pre errors throw; post errors are tests.
    static nlohmann::json RunScripts(const nlohmann::json& payload,
        const std::atomic_bool* cancelled = nullptr);
    static ScriptExecutionResult Execute(HttpEngine& http, const ApiRequest& request,
        ScriptContext context, const std::atomic_bool* cancelled = nullptr);
};
}
