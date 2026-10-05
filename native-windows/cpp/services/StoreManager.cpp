#include "StoreManager.h"
#include "VariableResolver.h"
#include "ImportDocument.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <windows.h>
#include <wincrypt.h>
#include <objbase.h>
#include <algorithm>
#include <stdexcept>
#include <set>
#include <functional>

#pragma comment(lib, "crypt32.lib")

namespace native_app {

namespace fs = std::filesystem;
using json = nlohmann::json;

// Secret fields are protected for the current Windows user; never stored as
// plaintext. This is user-account protection, not protection from that user.
static std::string ProtectValue(const std::string& value) {
    if (value.empty()) return value;
    DATA_BLOB input{static_cast<DWORD>(value.size()), reinterpret_cast<BYTE*>(const_cast<char*>(value.data()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"DataForge Studio", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("Windows could not protect saved credentials.");
    DWORD length = 0;
    CryptBinaryToStringA(output.pbData, output.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length);
    std::string result(length, '\0');
    BOOL ok = CryptBinaryToStringA(output.pbData, output.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &length);
    LocalFree(output.pbData);
    if (!ok) throw std::runtime_error("Windows could not encode saved credentials.");
    result.resize(length);
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return "dpapi:v1:" + result;
}

static std::string UnprotectValue(const std::string& value) {
    if (value.rfind("dpapi:v1:", 0) != 0) return value; // Legacy local JSON import.
    const std::string encoded = value.substr(9);
    DWORD length = 0;
    if (!CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64, nullptr, &length, nullptr, nullptr))
        throw std::runtime_error("Saved credential data is damaged.");
    std::vector<BYTE> bytes(length);
    if (!CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64, bytes.data(), &length, nullptr, nullptr))
        throw std::runtime_error("Saved credential data is damaged.");
    DATA_BLOB input{length, bytes.data()}, output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("Saved credentials belong to another Windows account or are damaged.");
    std::string result(reinterpret_cast<char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return result;
}

static void WriteStore(const std::string& filename, const json& value) {
    const auto target = fs::u8path(filename);
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Cannot initialize a local save operation.");
    wchar_t identifier[40]{}; StringFromGUID2(guid, identifier, 40);
    const auto temporary = target.wstring() + identifier + L".pending";
    const std::string bytes = value.dump(2);
    if (bytes.size() > 128 * 1024 * 1024) throw std::runtime_error("Local data exceeds the 128 MiB save limit.");
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot save local data. Check folder access and free disk space.");
    DWORD written = 0;
    BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    ok = ok && written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("Cannot finish saving local data. Previous data was preserved.");
    }
}

StoreManager::StoreManager(const std::string& dataDir) : dataDir_(dataDir) {
    fs::create_directories(fs::u8path(dataDir_));
    collectionsFile_ = (fs::u8path(dataDir_) / "collections.json").u8string();
    environmentsFile_ = (fs::u8path(dataDir_) / "environments.json").u8string();
    globalsFile_ = (fs::u8path(dataDir_) / "globals.json").u8string();
    historyFile_ = (fs::u8path(dataDir_) / "history.json").u8string();

    LoadAll();
}

static json SerializeVariable(const Variable& v) {
    return json{{"key", v.key}, {"value", ProtectValue(v.value)}, {"enabled", v.enabled},
        {"scriptJsonValue", ProtectValue(v.scriptJsonValue)}};
}

static Variable DeserializeVariable(const json& j) {
    Variable v;
    v.key = j.value("key", "");
    const auto raw = j.value("value", json(""));
    v.value = raw.is_string() ? UnprotectValue(raw.get<std::string>()) : raw.dump();
    v.scriptJsonValue = j.contains("scriptJsonValue") ? UnprotectValue(j.value("scriptJsonValue", "")) :
        (raw.is_string() ? std::string{} : raw.dump());
    if (!v.scriptJsonValue.empty()) { const auto validated = json::parse(v.scriptJsonValue); (void)validated; }
    v.enabled = j.value("enabled", true);
    return v;
}

static json SerializeKeyValuePair(const KeyValuePair& kv) {
    return json{{"key", kv.key}, {"value", ProtectValue(kv.value)}, {"enabled", kv.enabled}, {"description", ProtectValue(kv.description)},
        {"scriptJsonValue", ProtectValue(kv.scriptJsonValue)}};
}

static KeyValuePair DeserializeKeyValuePair(const json& j) {
    KeyValuePair kv;
    kv.key = j.value("key", "");
    const auto raw = j.value("value", json(""));
    kv.value = raw.is_string() ? UnprotectValue(raw.get<std::string>()) : raw.dump();
    kv.scriptJsonValue = j.contains("scriptJsonValue") ? UnprotectValue(j.value("scriptJsonValue", "")) :
        (raw.is_string() ? std::string{} : raw.dump());
    if (!kv.scriptJsonValue.empty()) { const auto validated = json::parse(kv.scriptJsonValue); (void)validated; }
    kv.enabled = j.value("enabled", true);
    kv.description = UnprotectValue(j.value("description", ""));
    return kv;
}

static json SerializeRequest(const ApiRequest& r) {
    json j;
    j["preRequestScript"] = ProtectValue(r.preRequestScript);
    j["postResponseScript"] = ProtectValue(r.postResponseScript);
    j["id"] = r.id;
    j["name"] = r.name;
    j["description"] = ProtectValue(r.description);
    j["iterationDataJson"] = ProtectValue(r.iterationDataJson);
    if(r.hasIteration)j["iteration"]=r.iteration;
    j["method"] = HttpMethodToString(r.method);
    j["url"] = ProtectValue(r.url);

    json hdrs = json::array();
    for (const auto& h : r.headers) hdrs.push_back(SerializeKeyValuePair(h));
    j["headers"] = hdrs;

    json prms = json::array();
    for (const auto& p : r.params) prms.push_back(SerializeKeyValuePair(p));
    j["params"] = prms;

    json pathPrms = json::array();
    for (const auto& p : r.pathParams) pathPrms.push_back(SerializeKeyValuePair(p));
    j["pathParams"] = pathPrms;

    j["auth"] = json{
        {"type", AuthTypeToString(r.auth.type)},
        {"token", ProtectValue(r.auth.token)},
        {"username", ProtectValue(r.auth.username)},
        {"password", ProtectValue(r.auth.password)},
        {"key", r.auth.key},
        {"value", ProtectValue(r.auth.value)},
        {"in", r.auth.in}
    };

    json fItems = json::array();
    for (const auto& item : r.body.formItems) {
        fItems.push_back(json{
            {"key", item.key}, {"value", ProtectValue(item.value)}, {"enabled", item.enabled},
            {"type", item.type}, {"filename", item.filename}, {"contentType", item.contentType},
            {"contentBase64", ProtectValue(item.contentBase64)}
        });
    }
    j["body"] = json{
        {"type", BodyTypeToString(r.body.type)},
        {"content", ProtectValue(r.body.content)},
        {"formItems", fItems}
    };

    json tests = json::array();
    for (const auto& t : r.tests) {
        tests.push_back(json{
            {"type", t.type}, {"expected", ProtectValue(t.expected)}, {"field", ProtectValue(t.field)},
            {"header", ProtectValue(t.header)}, {"name", t.name}, {"enabled", t.enabled}
        });
    }
    j["tests"] = tests;
    j["variables"] = json::array();
    for (const auto& v : r.variables) j["variables"].push_back(SerializeKeyValuePair(v));
    j["options"] = {{"timeoutSec", r.options.timeoutSec}, {"followRedirects", r.options.followRedirects}, {"verifyTls", r.options.verifyTls}};

    return j;
}

static ApiRequest DeserializeRequest(const json& j) {
    ApiRequest r;
    r.preRequestScript = UnprotectValue(j.value("preRequestScript", ""));
    r.postResponseScript = UnprotectValue(j.value("postResponseScript", ""));
    r.id = j.value("id", "");
    r.name = j.value("name", "Untitled Request");
    r.description = UnprotectValue(j.value("description", ""));
    r.iterationDataJson=UnprotectValue(j.value("iterationDataJson",std::string{}));
    if(r.iterationDataJson.empty()&&j.contains("iterationData"))r.iterationDataJson=j["iterationData"].dump();
    (void)RequestIterationData(r);
    if(j.contains("iteration")){if(!j["iteration"].is_number_integer())throw std::runtime_error("Saved request iteration must be an integer.");r.iteration=j["iteration"].get<int>();r.hasIteration=true;}
    r.method = HttpMethodFromString(j.value("method", "GET"));
    r.url = UnprotectValue(j.value("url", ""));

    if (j.contains("headers") && j["headers"].is_array()) {
        for (const auto& h : j["headers"]) r.headers.push_back(DeserializeKeyValuePair(h));
    }
    if (j.contains("params") && j["params"].is_array()) {
        for (const auto& p : j["params"]) r.params.push_back(DeserializeKeyValuePair(p));
    }
    if (j.contains("pathParams") && j["pathParams"].is_array()) {
        for (const auto& p : j["pathParams"]) r.pathParams.push_back(DeserializeKeyValuePair(p));
    }
    if (j.contains("auth") && j["auth"].is_object()) {
        const json& a = j["auth"];
        r.auth.type = AuthTypeFromString(a.value("type", "none"));
        r.auth.token = UnprotectValue(a.value("token", ""));
        r.auth.username = UnprotectValue(a.value("username", ""));
        r.auth.password = UnprotectValue(a.value("password", ""));
        r.auth.key = a.value("key", "");
        r.auth.value = UnprotectValue(a.value("value", ""));
        r.auth.in = a.value("in", "header");
    }
    if (j.contains("body") && j["body"].is_object()) {
        const json& b = j["body"];
        r.body.type = BodyTypeFromString(b.value("type", "none"));
        r.body.content = UnprotectValue(b.value("content", ""));
        if (b.contains("formItems") && b["formItems"].is_array()) {
            for (const auto& f : b["formItems"]) {
                FormItem item;
                item.key = f.value("key", "");
                item.value = UnprotectValue(f.value("value", ""));
                item.enabled = f.value("enabled", true);
                item.type = f.value("type", "text");
                item.filename = f.value("filename", "");
                item.contentType = f.value("contentType", "application/octet-stream");
                item.contentBase64 = UnprotectValue(f.value("contentBase64", ""));
                r.body.formItems.push_back(item);
            }
        }
    }
    if (j.contains("tests") && j["tests"].is_array()) {
        for (const auto& t : j["tests"]) {
            TestAssertion a;
            a.type = t.value("type", "");
            a.expected = UnprotectValue(t.value("expected", ""));
            a.field = UnprotectValue(t.value("field", ""));
            a.header = UnprotectValue(t.value("header", ""));
            a.name = t.value("name", "");
            a.enabled = t.value("enabled", true);
            r.tests.push_back(a);
        }
    }

    if (j.contains("variables") && j["variables"].is_array())
        for (const auto& v : j["variables"]) r.variables.push_back(DeserializeKeyValuePair(v));
    if (j.contains("options") && j["options"].is_object()) {
        r.options.timeoutSec = j["options"].value("timeoutSec", 20.0);
        r.options.followRedirects = j["options"].value("followRedirects", true);
        r.options.verifyTls = j["options"].value("verifyTls", true);
    }
    return r;
}

static json SerializeFolder(const Folder& f) {
    json j;
    j["preRequestScript"] = ProtectValue(f.preRequestScript);
    j["postResponseScript"] = ProtectValue(f.postResponseScript);
    j["id"] = f.id;
    j["name"] = f.name;
    j["description"] = ProtectValue(f.description);
    json reqs = json::array();
    for (const auto& r : f.requests) reqs.push_back(SerializeRequest(r));
    j["requests"] = reqs;

    json flds = json::array();
    for (const auto& sub : f.folders) flds.push_back(SerializeFolder(sub));
    j["folders"] = flds;
    ApiRequest authRequest;
    authRequest.auth = f.auth;
    j["auth"] = SerializeRequest(authRequest)["auth"];
    j["variables"] = json::array();
    for (const auto& v : f.variables) j["variables"].push_back(SerializeVariable(v));
    return j;
}

static Folder DeserializeFolder(const json& j) {
    Folder f;
    f.preRequestScript = UnprotectValue(j.value("preRequestScript", ""));
    f.postResponseScript = UnprotectValue(j.value("postResponseScript", ""));
    f.id = j.value("id", "");
    f.name = j.value("name", "");
    f.description = UnprotectValue(j.value("description", ""));
    if (j.contains("requests") && j["requests"].is_array()) {
        for (const auto& r : j["requests"]) f.requests.push_back(DeserializeRequest(r));
    }
    if (j.contains("folders") && j["folders"].is_array()) {
        for (const auto& sub : j["folders"]) f.folders.push_back(DeserializeFolder(sub));
    }
    if (j.contains("auth")) f.auth = DeserializeRequest(json{{"auth", j["auth"]}}).auth;
    if (j.contains("variables") && j["variables"].is_array())
        for (const auto& v : j["variables"]) f.variables.push_back(DeserializeVariable(v));
    return f;
}

static json SerializeCollection(const Collection& c) {
    json j;
    j["preRequestScript"] = ProtectValue(c.preRequestScript);
    j["postResponseScript"] = ProtectValue(c.postResponseScript);
    j["id"] = c.id;
    j["name"] = c.name;
    j["description"] = ProtectValue(c.description);

    json reqs = json::array();
    for (const auto& r : c.requests) reqs.push_back(SerializeRequest(r));
    j["requests"] = reqs;

    json flds = json::array();
    for (const auto& f : c.folders) flds.push_back(SerializeFolder(f));
    j["folders"] = flds;
    ApiRequest authRequest;
    authRequest.auth = c.auth;
    j["auth"] = SerializeRequest(authRequest)["auth"];
    j["variables"] = json::array();
    for (const auto& v : c.variables) j["variables"].push_back(SerializeVariable(v));
    return j;
}

static Collection DeserializeCollection(const json& j) {
    Collection c;
    c.preRequestScript = UnprotectValue(j.value("preRequestScript", ""));
    c.postResponseScript = UnprotectValue(j.value("postResponseScript", ""));
    c.id = j.value("id", "");
    c.name = j.value("name", "");
    c.description = UnprotectValue(j.value("description", ""));
    if (j.contains("requests") && j["requests"].is_array()) {
        for (const auto& r : j["requests"]) c.requests.push_back(DeserializeRequest(r));
    }
    if (j.contains("folders") && j["folders"].is_array()) {
        for (const auto& f : j["folders"]) c.folders.push_back(DeserializeFolder(f));
    }
    if (j.contains("auth")) c.auth = DeserializeRequest(json{{"auth", j["auth"]}}).auth;
    if (j.contains("variables") && j["variables"].is_array())
        for (const auto& v : j["variables"]) c.variables.push_back(DeserializeVariable(v));
    return c;
}

static HistoryEntry SanitizeHistory(const HistoryEntry& entry);
static void ValidateWorkspaceIdentities(const std::vector<Collection>& collections) {
    std::set<std::string> identifiers;size_t count=0;
    auto add=[&](const std::string& id){if(++count>100000)throw std::runtime_error("Workspace exceeds its item limit.");if(id.empty())throw std::runtime_error("Every collection, folder and request needs an identifier. Reimport it to generate identifiers.");if(!identifiers.insert(id).second)throw std::runtime_error("A collection, folder or request identifier already exists in this workspace. Choose a unique identifier or reimport the collection.");};
    std::function<void(const Folder&,unsigned)> folder=[&](const Folder& value,unsigned depth){if(depth>32)throw std::runtime_error("Workspace folder nesting exceeds 32 levels.");add(value.id);for(const auto& request:value.requests)add(request.id);for(const auto& child:value.folders)folder(child,depth+1);};
    for(const auto& collection:collections){add(collection.id);for(const auto& request:collection.requests)add(request.id);for(const auto& child:collection.folders)folder(child,1);}
}

void StoreManager::LoadAll() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (fs::exists(fs::u8path(environmentsFile_))) {
        try {
            std::ifstream f(fs::u8path(environmentsFile_));
            json j = json::parse(f);
            if (!j.is_object()) throw std::runtime_error("Invalid environments schema");
            environments_.activeId = j.value("active", "");
            if (j.contains("environments") && j["environments"].is_array()) {
                for (const auto& item : j["environments"]) {
                    Environment env;
                    env.id = item.value("id", "");
                    env.name = item.value("name", "");
                    if (item.contains("variables") && item["variables"].is_array()) {
                        for (const auto& v : item["variables"]) env.variables.push_back(DeserializeVariable(v));
                    }
                    environments_.environments.push_back(env);
                }
            }
        } catch (...) { throw std::runtime_error("Cannot read environments.json. Restore a valid backup; the original file was preserved."); }
    }

    if (fs::exists(fs::u8path(collectionsFile_))) {
        try {
            std::ifstream f(fs::u8path(collectionsFile_));
            json j = json::parse(f);
            if (!j.is_array()) throw std::runtime_error("Invalid collections schema");
            if (j.is_array()) {
                for (const auto& c : j) collections_.push_back(DeserializeCollection(c));
            }
        } catch (...) { throw std::runtime_error("Cannot read collections.json. Restore a valid backup; the original file was preserved."); }
    }

    if (fs::exists(fs::u8path(globalsFile_))) {
        try {
            std::ifstream f(fs::u8path(globalsFile_));
            json j = json::parse(f);
            if (!j.is_array()) throw std::runtime_error("Invalid globals schema");
            if (j.is_array()) {
                for (const auto& v : j) globals_.push_back(DeserializeVariable(v));
            }
        } catch (...) { throw std::runtime_error("Cannot read globals.json. Restore a valid backup; the original file was preserved."); }
    }

    if (fs::exists(fs::u8path(historyFile_))) {
        try {
            std::ifstream f(fs::u8path(historyFile_));
            json j = json::parse(f);
            if (!j.is_array()) throw std::runtime_error("Invalid history schema");
            if (j.is_array()) {
                for (const auto& h : j) {
                    HistoryEntry he;
                    he.id = h.value("id", "");
                    he.method = h.value("method", "GET");
                    he.url = UnprotectValue(h.value("url", ""));
                    he.statusCode = h.value("status_code", 0);
                    he.statusText = h.value("status_text", "");
                    he.latencyMs = h.value("latency_ms", 0.0);
                    he.sizeBytes = h.value("size_bytes", static_cast<size_t>(0));
                    he.executedAt = h.value("executed_at", "");
                    if (h.contains("request_snapshot")) {
                        he.requestSnapshot = DeserializeRequest(h["request_snapshot"]);
                    }
                    if (history_.size() < 100) history_.push_back(SanitizeHistory(he));
                }
            }
        } catch (...) { throw std::runtime_error("Cannot read history.json. Restore a valid backup; the original file was preserved."); }
    }

    ValidateWorkspaceIdentities(collections_);
    // A new workspace starts empty, matching the source application. Samples
    // are created explicitly by the user rather than injecting mock endpoints.
}

void StoreManager::SaveCollectionsInternal() {
    json arr = json::array();
    for (const auto& c : collections_) arr.push_back(SerializeCollection(c));
    WriteStore(collectionsFile_, arr);
}

void StoreManager::SaveEnvironmentsInternal() {
    json arr = json::array();
    for (const auto& env : environments_.environments) {
        json envJ;
        envJ["id"] = env.id;
        envJ["name"] = env.name;
        json vars = json::array();
        for (const auto& v : env.variables) vars.push_back(SerializeVariable(v));
        envJ["variables"] = vars;
        arr.push_back(envJ);
    }
    json root;
    root["active"] = environments_.activeId;
    root["environments"] = arr;
    WriteStore(environmentsFile_, root);
}

void StoreManager::SaveGlobalsInternal() {
    json arr = json::array();
    for (const auto& v : globals_) arr.push_back(SerializeVariable(v));
    WriteStore(globalsFile_, arr);
}

void StoreManager::SaveHistoryInternal() {
    json arr = json::array();
    // Keep up to 100 entries
    size_t limit = std::min(history_.size(), static_cast<size_t>(100));
    for (size_t i = 0; i < limit; ++i) {
        const auto& h = history_[i];
        json item;
        item["id"] = h.id;
        item["method"] = h.method;
        item["url"] = ProtectValue(h.url);
        item["status_code"] = h.statusCode;
        item["status_text"] = h.statusText;
        item["latency_ms"] = h.latencyMs;
        item["size_bytes"] = h.sizeBytes;
        item["executed_at"] = h.executedAt;
        item["request_snapshot"] = SerializeRequest(h.requestSnapshot);
        arr.push_back(item);
    }
    WriteStore(historyFile_, arr);
}

std::vector<Collection> StoreManager::GetCollections() {
    std::lock_guard<std::mutex> lock(mutex_);
    return collections_;
}

void StoreManager::SaveCollections(const std::vector<Collection>& collections) {
    std::lock_guard<std::mutex> lock(mutex_);
    ValidateWorkspaceIdentities(collections);
    auto previous = collections_; collections_ = collections;
    try { SaveCollectionsInternal(); } catch (...) { collections_ = std::move(previous); throw; }
}

EnvironmentStore StoreManager::GetEnvironments() {
    std::lock_guard<std::mutex> lock(mutex_);
    return environments_;
}

void StoreManager::SaveEnvironments(const EnvironmentStore& envStore) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto previous = environments_; environments_ = envStore;
    try { SaveEnvironmentsInternal(); } catch (...) { environments_ = std::move(previous); throw; }
}

void StoreManager::SetActiveEnvironment(const std::string& envId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!envId.empty() && std::none_of(environments_.environments.begin(), environments_.environments.end(), [&](const Environment& environment) { return environment.id == envId; }))
        throw std::runtime_error("The selected environment no longer exists.");
    const auto previous = environments_.activeId; environments_.activeId = envId;
    try { SaveEnvironmentsInternal(); } catch (...) { environments_.activeId = previous; throw; }
}

std::vector<Variable> StoreManager::GetGlobals() {
    std::lock_guard<std::mutex> lock(mutex_);
    return globals_;
}

void StoreManager::SaveGlobals(const std::vector<Variable>& globals) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto previous = globals_; globals_ = globals;
    try { SaveGlobalsInternal(); } catch (...) { globals_ = std::move(previous); throw; }
}

std::vector<HistoryEntry> StoreManager::GetHistory() {
    std::lock_guard<std::mutex> lock(mutex_);
    return history_;
}

static std::string HistoryOrigin(const std::string& value) {
    // URL paths can themselves be credentials. Preserve only a safe origin,
    // removing userinfo as well as every path, query and fragment component.
    const auto scheme = value.find("://");
    if (scheme == std::string::npos || (value.substr(0, scheme) != "https" && value.substr(0, scheme) != "http")) return "[REDACTED]";
    const auto begin = scheme + 3;
    const auto end = value.find_first_of("/?#", begin);
    std::string authority = value.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    const auto userinfo = authority.rfind('@');
    if (userinfo != std::string::npos) authority.erase(0, userinfo + 1);
    if (authority.empty() || authority.find_first_of("\r\n\t ") != std::string::npos) return "[REDACTED]";
    return value.substr(0, scheme + 3) + authority + "/[REDACTED]";
}

static HistoryEntry SanitizeHistory(const HistoryEntry& entry) {
    HistoryEntry sanitized = entry;
    auto hide = [](std::string& value) { if (!value.empty()) value = "[REDACTED]"; };
    auto& snapshot = sanitized.requestSnapshot;
    snapshot.preRequestScript.clear();
    snapshot.postResponseScript.clear();
    snapshot.description.clear();
    snapshot.iterationDataJson.clear();
    sanitized.url = HistoryOrigin(sanitized.url);
    snapshot.url = HistoryOrigin(snapshot.url);
    // History is an activity record. The protected saved request identified by
    // snapshot.id supplies replay context; an unsaved snapshot requires re-entry.
    hide(snapshot.auth.token); hide(snapshot.auth.username); hide(snapshot.auth.password); hide(snapshot.auth.value);
    for (auto& header : snapshot.headers) hide(header.value);
    for (auto& parameter : snapshot.params) hide(parameter.value);
    for (auto& parameter : snapshot.pathParams) hide(parameter.value);
    for (auto& variable : snapshot.variables) { hide(variable.value); variable.scriptJsonValue.clear(); variable.description.clear(); }
    for (auto& header : snapshot.headers) { header.scriptJsonValue.clear(); header.description.clear(); }
    for (auto& parameter : snapshot.params) { parameter.scriptJsonValue.clear(); parameter.description.clear(); }
    for (auto& parameter : snapshot.pathParams) { parameter.scriptJsonValue.clear(); parameter.description.clear(); }
    hide(snapshot.body.content);
    for (auto& item : snapshot.body.formItems) { hide(item.value); hide(item.contentBase64); hide(item.filename); }
    for (auto& assertion : snapshot.tests) { hide(assertion.expected); hide(assertion.field); hide(assertion.header); }
    return sanitized;
}

void StoreManager::AddHistory(const HistoryEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto sanitized = SanitizeHistory(entry);
    auto previous = history_;
    history_.insert(history_.begin(), sanitized);
    if (history_.size() > 100) history_.resize(100);
    try { SaveHistoryInternal(); } catch (...) { history_ = std::move(previous); throw; }
}

void StoreManager::ClearHistory() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto previous = history_; history_.clear();
    try { SaveHistoryInternal(); } catch (...) { history_ = std::move(previous); throw; }
}

std::map<std::string, std::string> StoreManager::GetMergedVariables(
    const std::string& environmentId,
    const std::vector<Variable>& requestVars
) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, std::string> vars;

    // 1. Globals
    for (const auto& v : globals_) {
        if (v.enabled && !v.key.empty()) vars[v.key] = v.value;
    }

    // 2. Active Environment
    std::string envId = environmentId.empty() ? environments_.activeId : environmentId;
    for (const auto& env : environments_.environments) {
        if (env.id == envId) {
            for (const auto& v : env.variables) {
                if (v.enabled && !v.key.empty()) vars[v.key] = v.value;
            }
            break;
        }
    }

    // 3. Request variables
    for (const auto& v : requestVars) {
        if (v.enabled && !v.key.empty()) vars[v.key] = v.value;
    }

    return vars;
}

} // namespace native_app
