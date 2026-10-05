#include "ScriptRuntime.h"
#include "HttpEngine.h"
#include "JsonSerialization.h"
#include "ImportDocument.h"
#include "ScriptBootstrap.h"
#include <quickjs/quickjs.h>
#include <windows.h>
#include <objbase.h>
#include <bcrypt.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <limits>
#include <stdexcept>
#include <cstring>
#include <set>

#pragma comment(lib, "bcrypt.lib")
namespace native_app {
namespace {
using json = nlohmann::json;
constexpr size_t kInputLimit = 4 * 1024 * 1024;
constexpr size_t kMemoryLimit = 64 * 1024 * 1024;
// Native std::thread workers use Windows' default stack reservation. Leave
// room for C++ frames and exception handling before QuickJS rejects recursion.
constexpr size_t kStackLimit = 512 * 1024;
struct RuntimeDelete { void operator()(JSRuntime* value) const { JS_FreeRuntime(value); } };
struct ContextDelete { void operator()(JSContext* value) const { JS_FreeContext(value); } };
struct Value {
    JSContext* context;
    JSValue value;
    Value(JSContext* c, JSValue v) : context(c), value(v) {}
    ~Value() { JS_FreeValue(context, value); }
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
};
struct Budget {
    std::chrono::steady_clock::time_point deadline;
    const std::atomic_bool* cancelled;
    bool timedOut = false;
};
int Interrupt(JSRuntime*, void* opaque) {
    auto& budget = *static_cast<Budget*>(opaque);
    if (budget.cancelled && budget.cancelled->load()) return 1;
    if (std::chrono::steady_clock::now() >= budget.deadline) { budget.timedOut = true; return 1; }
    return 0;
}
std::string ExceptionText(JSContext* context, const Budget& budget) {
    if (budget.cancelled && budget.cancelled->load()) return "Request cancelled.";
    if (budget.timedOut) return "Script exceeded the one-second execution limit.";
    Value exception(context, JS_GetException(context));
    size_t length = 0; const char* text = JS_ToCStringLen(context, &length, exception.value);
    if (!text) return "Script failed or exceeded the 64 MiB memory limit.";
    std::string result(text, (std::min)(length, size_t{1500})); JS_FreeCString(context, text);
    return result;
}
json EmptyFrame(const json& payload) {
    return {{"request", payload.value("request", json::object())}, {"changes", json::object()},
        {"tests", json::array()}, {"console", json::array()},
        {"local", payload.value("scopes", json::object()).value("local", json::object())}};
}
json FailedFrame(const json& payload, const std::string& detail) {
    const std::string message = "post-response script failed: " + detail;
    auto result = EmptyFrame(payload);
    result["tests"].push_back({{"type", "script"}, {"name", "Post-response script"}, {"passed", false}, {"message", message}});
    result["console"].push_back({{"level", "error"}, {"phase", "post-response"}, {"source", "Sandbox"}, {"message", message}});
    return result;
}
json DynamicVariables() {
    GUID guid{}; if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Cannot create a dynamic script identifier.");
    wchar_t text[40]{}; StringFromGUID2(guid, text, 40); std::string id;
    for (const auto c : text) if (c && c != L'{' && c != L'}') id += static_cast<char>(c);
    std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    ULONG random = 0;
    do {
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&random), sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("Cannot initialize dynamic script values.");
    } while (random >= (std::numeric_limits<ULONG>::max)() - ((std::numeric_limits<ULONG>::max)() % 1001));
    return {{"$guid", id}, {"$timestamp", std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()}, {"$randomInt", random % 1001}};
}
json Payload(const std::vector<ScriptEntry>& inherited, const ApiRequest& request,
    const ScriptContext& context, bool post, const ApiResponse* response = nullptr) {
    json entries = json::array();
    for (const auto& entry : inherited) entries.push_back({{"name", entry.name}, {"code", entry.code}});
    const auto& own = post ? request.postResponseScript : request.preRequestScript;
    if (!own.empty()) entries.push_back({{"name", request.name}, {"code", own}});
    json result = {{"scripts", entries}, {"request", RequestJson::ToJson(request)},
        {"scopes", {{"globals", context.globals}, {"collection", context.collection},
            {"environment", context.environment}, {"data", context.data}, {"local", context.local}}},
        {"phase", post ? "post-response" : "pre-request"},
        {"environmentEnabled", context.environmentEnabled}, {"collectionEnabled", context.collectionEnabled},
        {"iteration", context.iteration}, {"dynamic", DynamicVariables()}};
    if (response) {
        json headers = json::array(); for (const auto& h : response->headers)
            headers.push_back({{"key", h.key}, {"value", h.value}, {"enabled", h.enabled}});
        result["response"] = {{"status_code", response->statusCode}, {"status_text", response->statusText},
            {"latency_ms", response->latencyMs}, {"size_bytes", response->sizeBytes},
            {"headers", headers}, {"body", response->body}, {"error", response->error}};
    }
    return result;
}
void ApplyDelta(json& values, const json& delta) {
    if (!values.is_object()) throw std::runtime_error("Script scope must be an object.");
    if (!delta.is_object()) throw std::runtime_error("Script variable delta must be an object.");
    const auto removed = delta.value("unset", json::array()); const auto added = delta.value("set", json::object());
    if (!removed.is_array() || !added.is_object() || removed.size() + added.size() > 10000)
        throw std::runtime_error("Script variable delta exceeds its schema or limit.");
    for (const auto& item : removed) values.erase(item.get<std::string>());
    for (auto item = added.begin(); item != added.end(); ++item) values[item.key()] = item.value();
}
void ApplyToContext(ScriptContext& context, const json& frame) {
    const auto changes = frame.value("changes", json::object());
    for (const auto* name : {"globals", "collection", "environment", "local"}) {
        auto* target = std::strcmp(name, "globals") == 0 ? &context.globals :
            std::strcmp(name, "collection") == 0 ? &context.collection :
            std::strcmp(name, "environment") == 0 ? &context.environment : &context.local;
        if (changes.contains(name)) ApplyDelta(*target, changes[name]);
    }
    context.local = frame.value("local", context.local);
}
json MergeChanges(const json& first, const json& second) {
    json result = json::object();
    for (const auto* scope : {"globals", "collection", "environment", "local"}) {
        json sets = json::object(); std::vector<std::string> removed;
        for (const auto* frame : {&first, &second}) {
            const auto changes = frame->value("changes", json::object()); if (!changes.contains(scope)) continue;
            const auto& delta = changes[scope];
            for (const auto& key : delta.value("unset", json::array())) {
                const auto name = key.get<std::string>(); sets.erase(name);
                if (std::find(removed.begin(), removed.end(), name) == removed.end()) removed.push_back(name);
            }
            const auto added = delta.value("set", json::object());
            for (auto item = added.begin(); item != added.end(); ++item) {
                sets[item.key()] = item.value(); removed.erase(std::remove(removed.begin(), removed.end(), item.key()), removed.end());
            }
        }
        result[scope] = {{"set", sets}, {"unset", removed}};
    }
    return result;
}
void AppendReports(ApiResponse& response, const json& frame) {
    const auto tests = frame.value("tests", json::array()); const auto logs = frame.value("console", json::array());
    if (!tests.is_array() || !logs.is_array() || tests.size() > 10000 || logs.size() > 100)
        throw std::runtime_error("Script reports exceed their schema or limit.");
    for (const auto& t : tests) { TestResult result; result.type = "script"; result.name = t.value("name", "Script");
        result.passed = t.value("passed", false); result.message = t.value("message", ""); response.testResults.push_back(std::move(result)); }
    for (const auto& entry : logs) response.scriptConsole.push_back({entry.value("level", "log"),
        entry.value("phase", ""), entry.value("source", ""), entry.value("message", "")});
}
}
json ScriptRuntime::RunScripts(const json& input, const std::atomic_bool* cancelled) {
    if (!input.is_object()) throw std::runtime_error("Script payload must be an object.");
    ValidateDocumentStructure(input);
    auto payload = input; const auto phase = payload.value("phase", "pre-request");
    if (phase != "pre-request" && phase != "post-response") throw std::runtime_error("Invalid script phase.");
    try {
        auto entries = payload.value("scripts", json::array());
        if (!entries.is_array()) throw std::runtime_error("Scripts must be an array.");
        json filtered = json::array();
        for (const auto& entry : entries) {
            if (!entry.is_object() || !entry.contains("code") || !entry["code"].is_string()) throw std::runtime_error("Script content must be JavaScript text.");
            const auto& code = entry["code"].get_ref<const std::string&>();
            if (code.size() > 65536) throw std::runtime_error("Scripts are limited to 64 KiB per inherited entry.");
            if (std::any_of(code.begin(), code.end(), [](unsigned char c) { return !std::isspace(c); })) filtered.push_back(entry);
        }
        if (filtered.size() > 25) throw std::runtime_error("Scripts are limited to 25 inherited entries.");
        payload["scripts"] = filtered;
        if (cancelled && cancelled->load()) throw std::runtime_error("Request cancelled.");
        if (filtered.empty()) return EmptyFrame(payload);
        if (!payload.contains("dynamic")) payload["dynamic"] = DynamicVariables();
        const auto serialized = payload.dump(-1, ' ', true);
        if (serialized.size() > kInputLimit) throw std::runtime_error("Script input exceeds the 4 MiB sandbox limit.");
        if (cancelled && cancelled->load()) throw std::runtime_error("Request cancelled.");
        std::unique_ptr<JSRuntime, RuntimeDelete> runtime(JS_NewRuntime());
        if (!runtime) throw std::runtime_error("Cannot allocate native script runtime.");
        JS_SetMemoryLimit(runtime.get(), kMemoryLimit); JS_SetMaxStackSize(runtime.get(), kStackLimit);
        Budget budget{std::chrono::steady_clock::now() + std::chrono::seconds(1), cancelled};
        JS_SetInterruptHandler(runtime.get(), Interrupt, &budget);
        std::unique_ptr<JSContext, ContextDelete> context(JS_NewContext(runtime.get()));
        if (!context) throw std::runtime_error("Cannot allocate native script context.");
        Value global(context.get(), JS_GetGlobalObject(context.get()));
        // No filesystem/network/process/module bridge; disallow blocking Atomics.
        for (const auto* name : {"Atomics", "SharedArrayBuffer", "WebAssembly"})
            if (JS_SetPropertyStr(context.get(), global.value, name, JS_UNDEFINED) < 0)
                throw std::runtime_error(ExceptionText(context.get(), budget));
        Value bootstrap(context.get(), JS_Eval(context.get(), kScriptBootstrap, std::strlen(kScriptBootstrap),
            "native-script-bootstrap.js", JS_EVAL_TYPE_GLOBAL));
        if (JS_IsException(bootstrap.value)) throw std::runtime_error(ExceptionText(context.get(), budget));
        budget.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        Value function(context.get(), JS_GetPropertyStr(context.get(), global.value, "runScriptFrame"));
        Value argument(context.get(), JS_ParseJSON(context.get(), serialized.data(), serialized.size(), "script-input.json"));
        if (JS_IsException(argument.value)) throw std::runtime_error(ExceptionText(context.get(), budget));
        Value output(context.get(), JS_Call(context.get(), function.value, JS_UNDEFINED, 1, &argument.value));
        if (JS_IsException(output.value)) throw std::runtime_error(ExceptionText(context.get(), budget));
        size_t length = 0; const char* bytes = JS_ToCStringLen(context.get(), &length, output.value);
        if (!bytes) throw std::runtime_error(ExceptionText(context.get(), budget));
        std::string text; if (length <= kInputLimit) text.assign(bytes, length); JS_FreeCString(context.get(), bytes);
        if (length > kInputLimit) throw std::runtime_error("Script output exceeds the 4 MiB sandbox limit.");
        const auto result = json::parse(text);
        if (!result.is_object() || !result.contains("request") || !result.contains("local")) throw std::runtime_error("Script produced an invalid result.");
        return result;
    } catch (const std::exception& error) {
        if (phase == "post-response" && !(cancelled && cancelled->load())) return FailedFrame(payload, error.what());
        throw std::runtime_error(phase + " script failed: " + std::string(error.what()).substr(0, 1500));
    }
}
ScriptExecutionResult ScriptRuntime::Execute(HttpEngine& http, const ApiRequest& request,
    ScriptContext context, const std::atomic_bool* cancelled) {
    context.data.update(RequestIterationData(request));
    if(request.hasIteration)context.iteration=request.iteration;
    std::set<std::string> localNames;
    for(const auto& variable:request.variables)if(variable.enabled&&!variable.key.empty()&&!localNames.insert(variable.key).second)throw std::runtime_error("Enabled request variable names must be unique.");
    for (const auto& v : request.variables) if (v.enabled && !v.key.empty() && !context.local.contains(v.key))
        context.local[v.key] = v.scriptJsonValue.empty() ? json(v.value) : json::parse(v.scriptJsonValue);
    const auto before = RunScripts(Payload(context.preRequestScripts, request, context, false), cancelled);
    ApplyToContext(context, before);
    ScriptExecutionResult result; result.request = RequestJson::FromJson(before["request"]);
    std::map<std::string, std::string> variables;
    for (const auto* scope : {&context.globals, &context.collection, &context.environment, &context.data, &context.local}) {
        if (!scope->is_object()) throw std::runtime_error("Script variable scopes must be objects.");
        for (auto item = scope->begin(); item != scope->end(); ++item) variables[item.key()] = item.value().is_string() ? item.value().get<std::string>() : item.value().dump();
    }
    auto wireRequest=result.request; wireRequest.variables.clear();wireRequest.iterationDataJson.clear(); // Flattened scopes already include iteration data and subsequent local mutations.
    result.response = http.Execute(wireRequest, variables, true, cancelled);
    const auto after = RunScripts(Payload(context.postResponseScripts, result.request, context, true, &result.response), cancelled);
    ApplyToContext(context, after); result.changes = MergeChanges(before, after); result.local = context.local;
    for (const auto* scope : {"globals", "collection", "environment"})
        if (!result.changes[scope]["set"].empty() || !result.changes[scope]["unset"].empty()) result.variablesChanged = true;
    AppendReports(result.response, before); AppendReports(result.response, after);
    return result;
}
}
