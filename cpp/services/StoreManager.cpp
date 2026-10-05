#include "StoreManager.h"
#include "VariableResolver.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace native_app {

namespace fs = std::filesystem;
using json = nlohmann::json;

StoreManager::StoreManager(const std::string& dataDir) : dataDir_(dataDir) {
    fs::create_directories(dataDir_);
    collectionsFile_ = (fs::path(dataDir_) / "collections.json").string();
    environmentsFile_ = (fs::path(dataDir_) / "environments.json").string();
    globalsFile_ = (fs::path(dataDir_) / "globals.json").string();
    historyFile_ = (fs::path(dataDir_) / "history.json").string();

    LoadAll();
}

static json SerializeVariable(const Variable& v) {
    return json{{"key", v.key}, {"value", v.value}, {"enabled", v.enabled}};
}

static Variable DeserializeVariable(const json& j) {
    Variable v;
    v.key = j.value("key", "");
    v.value = j.value("value", "");
    v.enabled = j.value("enabled", true);
    return v;
}

static json SerializeKeyValuePair(const KeyValuePair& kv) {
    return json{{"key", kv.key}, {"value", kv.value}, {"enabled", kv.enabled}, {"description", kv.description}};
}

static KeyValuePair DeserializeKeyValuePair(const json& j) {
    KeyValuePair kv;
    kv.key = j.value("key", "");
    kv.value = j.value("value", "");
    kv.enabled = j.value("enabled", true);
    kv.description = j.value("description", "");
    return kv;
}

static json SerializeRequest(const ApiRequest& r) {
    json j;
    j["id"] = r.id;
    j["name"] = r.name;
    j["method"] = HttpMethodToString(r.method);
    j["url"] = r.url;

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
        {"token", r.auth.token},
        {"username", r.auth.username},
        {"password", r.auth.password},
        {"key", r.auth.key},
        {"value", r.auth.value},
        {"in", r.auth.in}
    };

    json fItems = json::array();
    for (const auto& item : r.body.formItems) {
        fItems.push_back(json{
            {"key", item.key}, {"value", item.value}, {"enabled", item.enabled},
            {"type", item.type}, {"filename", item.filename}, {"contentType", item.contentType}
        });
    }
    j["body"] = json{
        {"type", BodyTypeToString(r.body.type)},
        {"content", r.body.content},
        {"formItems", fItems}
    };

    json tests = json::array();
    for (const auto& t : r.tests) {
        tests.push_back(json{
            {"type", t.type}, {"expected", t.expected}, {"field", t.field},
            {"header", t.header}, {"name", t.name}, {"enabled", t.enabled}
        });
    }
    j["tests"] = tests;

    return j;
}

static ApiRequest DeserializeRequest(const json& j) {
    ApiRequest r;
    r.id = j.value("id", "");
    r.name = j.value("name", "Untitled Request");
    r.method = HttpMethodFromString(j.value("method", "GET"));
    r.url = j.value("url", "");

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
        r.auth.token = a.value("token", "");
        r.auth.username = a.value("username", "");
        r.auth.password = a.value("password", "");
        r.auth.key = a.value("key", "");
        r.auth.value = a.value("value", "");
        r.auth.in = a.value("in", "header");
    }
    if (j.contains("body") && j["body"].is_object()) {
        const json& b = j["body"];
        r.body.type = BodyTypeFromString(b.value("type", "none"));
        r.body.content = b.value("content", "");
        if (b.contains("formItems") && b["formItems"].is_array()) {
            for (const auto& f : b["formItems"]) {
                FormItem item;
                item.key = f.value("key", "");
                item.value = f.value("value", "");
                item.enabled = f.value("enabled", true);
                item.type = f.value("type", "text");
                item.filename = f.value("filename", "");
                item.contentType = f.value("contentType", "application/octet-stream");
                r.body.formItems.push_back(item);
            }
        }
    }
    if (j.contains("tests") && j["tests"].is_array()) {
        for (const auto& t : j["tests"]) {
            TestAssertion a;
            a.type = t.value("type", "");
            a.expected = t.value("expected", "");
            a.field = t.value("field", "");
            a.header = t.value("header", "");
            a.name = t.value("name", "");
            a.enabled = t.value("enabled", true);
            r.tests.push_back(a);
        }
    }

    return r;
}

static json SerializeFolder(const Folder& f) {
    json j;
    j["id"] = f.id;
    j["name"] = f.name;
    json reqs = json::array();
    for (const auto& r : f.requests) reqs.push_back(SerializeRequest(r));
    j["requests"] = reqs;

    json flds = json::array();
    for (const auto& sub : f.folders) flds.push_back(SerializeFolder(sub));
    j["folders"] = flds;
    return j;
}

static Folder DeserializeFolder(const json& j) {
    Folder f;
    f.id = j.value("id", "");
    f.name = j.value("name", "");
    if (j.contains("requests") && j["requests"].is_array()) {
        for (const auto& r : j["requests"]) f.requests.push_back(DeserializeRequest(r));
    }
    if (j.contains("folders") && j["folders"].is_array()) {
        for (const auto& sub : j["folders"]) f.folders.push_back(DeserializeFolder(sub));
    }
    return f;
}

static json SerializeCollection(const Collection& c) {
    json j;
    j["id"] = c.id;
    j["name"] = c.name;
    j["description"] = c.description;

    json reqs = json::array();
    for (const auto& r : c.requests) reqs.push_back(SerializeRequest(r));
    j["requests"] = reqs;

    json flds = json::array();
    for (const auto& f : c.folders) flds.push_back(SerializeFolder(f));
    j["folders"] = flds;
    return j;
}

static Collection DeserializeCollection(const json& j) {
    Collection c;
    c.id = j.value("id", "");
    c.name = j.value("name", "");
    c.description = j.value("description", "");
    if (j.contains("requests") && j["requests"].is_array()) {
        for (const auto& r : j["requests"]) c.requests.push_back(DeserializeRequest(r));
    }
    if (j.contains("folders") && j["folders"].is_array()) {
        for (const auto& f : j["folders"]) c.folders.push_back(DeserializeFolder(f));
    }
    return c;
}

void StoreManager::InitDefaults() {
    environments_.activeId = "env_local";
    Environment envLocal;
    envLocal.id = "env_local";
    envLocal.name = "Localhost Studio";
    envLocal.variables = {
        {"base_url", "http://127.0.0.1:8001", true},
        {"api_prefix", "/api/mock/data", true},
        {"auth_token", "df_token_sample_abc123", true}
    };
    environments_.environments.push_back(envLocal);

    Environment envPublic;
    envPublic.id = "env_public";
    envPublic.name = "Public Demo APIs";
    envPublic.variables = {
        {"httpbin_url", "https://httpbin.org", true},
        {"cat_fact_url", "https://catfact.ninja/fact", true},
        {"ipify_url", "https://api.ipify.org?format=json", true}
    };
    environments_.environments.push_back(envPublic);

    Collection mockCol;
    mockCol.id = "col_db_mock";
    mockCol.name = "DataForge Mock DB REST API";
    mockCol.description = "Live REST API dynamically backed by active SQLite database tables";

    Folder fld;
    fld.id = "fld_ecommerce";
    fld.name = "E-Commerce Resources";

    ApiRequest req1;
    req1.id = "req_get_products";
    req1.name = "Get All Products";
    req1.method = HttpMethod::GET;
    req1.url = "{{base_url}}{{api_prefix}}/products?limit=5";
    req1.headers.push_back({"Accept", "application/json", true, ""});
    req1.params.push_back({"limit", "5", true, ""});
    req1.tests.push_back({"status_200", "200", "", "", "Status code is 200", true});
    req1.tests.push_back({"time_lt_500", "500", "", "", "Response time < 500ms", true});
    req1.tests.push_back({"json_field", "rows", "rows", "", "Has 'rows' array", true});
    fld.requests.push_back(req1);

    ApiRequest req2;
    req2.id = "req_get_product_by_id";
    req2.name = "Get Single Product";
    req2.method = HttpMethod::GET;
    req2.url = "{{base_url}}{{api_prefix}}/products/1";
    req2.headers.push_back({"Accept", "application/json", true, ""});
    req2.tests.push_back({"status_200", "200", "", "", "Status code is 200", true});
    fld.requests.push_back(req2);

    ApiRequest req3;
    req3.id = "req_create_product";
    req3.name = "Create New Product";
    req3.method = HttpMethod::POST;
    req3.url = "{{base_url}}{{api_prefix}}/products";
    req3.headers.push_back({"Content-Type", "application/json", true, ""});
    req3.auth.type = AuthType::Bearer;
    req3.auth.token = "{{auth_token}}";
    req3.body.type = BodyType::Json;
    req3.body.content = "{\n  \"category_id\": 1,\n  \"name\": \"Wireless Charging Pad 15W\",\n  \"sku\": \"ELEC-WCHG-15W\",\n  \"price\": 39.99,\n  \"stock\": 80,\n  \"status\": \"active\"\n}";
    req3.tests.push_back({"status_200_or_201", "", "", "", "Status code is 200 or 201", true});
    req3.tests.push_back({"json_field", "success", "success", "", "Operation succeeded", true});
    fld.requests.push_back(req3);

    mockCol.folders.push_back(fld);
    collections_.push_back(mockCol);

    SaveCollectionsInternal();
    SaveEnvironmentsInternal();
}

void StoreManager::LoadAll() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (fs::exists(environmentsFile_)) {
        try {
            std::ifstream f(environmentsFile_);
            json j = json::parse(f);
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
        } catch (...) {}
    }

    if (fs::exists(collectionsFile_)) {
        try {
            std::ifstream f(collectionsFile_);
            json j = json::parse(f);
            if (j.is_array()) {
                for (const auto& c : j) collections_.push_back(DeserializeCollection(c));
            }
        } catch (...) {}
    }

    if (fs::exists(globalsFile_)) {
        try {
            std::ifstream f(globalsFile_);
            json j = json::parse(f);
            if (j.is_array()) {
                for (const auto& v : j) globals_.push_back(DeserializeVariable(v));
            }
        } catch (...) {}
    }

    if (fs::exists(historyFile_)) {
        try {
            std::ifstream f(historyFile_);
            json j = json::parse(f);
            if (j.is_array()) {
                for (const auto& h : j) {
                    HistoryEntry he;
                    he.id = h.value("id", "");
                    he.method = h.value("method", "GET");
                    he.url = h.value("url", "");
                    he.statusCode = h.value("status_code", 0);
                    he.statusText = h.value("status_text", "");
                    he.latencyMs = h.value("latency_ms", 0.0);
                    he.sizeBytes = h.value("size_bytes", static_cast<size_t>(0));
                    he.executedAt = h.value("executed_at", "");
                    if (h.contains("request_snapshot")) {
                        he.requestSnapshot = DeserializeRequest(h["request_snapshot"]);
                    }
                    history_.push_back(he);
                }
            }
        } catch (...) {}
    }

    if (collections_.empty() && environments_.environments.empty()) {
        InitDefaults();
    }
}

void StoreManager::SaveCollectionsInternal() {
    json arr = json::array();
    for (const auto& c : collections_) arr.push_back(SerializeCollection(c));
    std::ofstream f(collectionsFile_);
    f << arr.dump(2);
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
    std::ofstream f(environmentsFile_);
    f << root.dump(2);
}

void StoreManager::SaveGlobalsInternal() {
    json arr = json::array();
    for (const auto& v : globals_) arr.push_back(SerializeVariable(v));
    std::ofstream f(globalsFile_);
    f << arr.dump(2);
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
        item["url"] = h.url;
        item["status_code"] = h.statusCode;
        item["status_text"] = h.statusText;
        item["latency_ms"] = h.latencyMs;
        item["size_bytes"] = h.sizeBytes;
        item["executed_at"] = h.executedAt;
        item["request_snapshot"] = SerializeRequest(h.requestSnapshot);
        arr.push_back(item);
    }
    std::ofstream f(historyFile_);
    f << arr.dump(2);
}

std::vector<Collection> StoreManager::GetCollections() {
    std::lock_guard<std::mutex> lock(mutex_);
    return collections_;
}

void StoreManager::SaveCollections(const std::vector<Collection>& collections) {
    std::lock_guard<std::mutex> lock(mutex_);
    collections_ = collections;
    SaveCollectionsInternal();
}

EnvironmentStore StoreManager::GetEnvironments() {
    std::lock_guard<std::mutex> lock(mutex_);
    return environments_;
}

void StoreManager::SaveEnvironments(const EnvironmentStore& envStore) {
    std::lock_guard<std::mutex> lock(mutex_);
    environments_ = envStore;
    SaveEnvironmentsInternal();
}

void StoreManager::SetActiveEnvironment(const std::string& envId) {
    std::lock_guard<std::mutex> lock(mutex_);
    environments_.activeId = envId;
    SaveEnvironmentsInternal();
}

std::vector<Variable> StoreManager::GetGlobals() {
    std::lock_guard<std::mutex> lock(mutex_);
    return globals_;
}

void StoreManager::SaveGlobals(const std::vector<Variable>& globals) {
    std::lock_guard<std::mutex> lock(mutex_);
    globals_ = globals;
    SaveGlobalsInternal();
}

std::vector<HistoryEntry> StoreManager::GetHistory() {
    std::lock_guard<std::mutex> lock(mutex_);
    return history_;
}

void StoreManager::AddHistory(const HistoryEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    HistoryEntry sanitized = entry;
    // Redact password and sensitive tokens in snapshot
    if (VariableResolver::IsSensitiveKey(sanitized.requestSnapshot.auth.token)) {
        sanitized.requestSnapshot.auth.token = "[REDACTED]";
    }
    if (VariableResolver::IsSensitiveKey(sanitized.requestSnapshot.auth.password)) {
        sanitized.requestSnapshot.auth.password = "[REDACTED]";
    }
    for (auto& h : sanitized.requestSnapshot.headers) {
        if (VariableResolver::IsSensitiveKey(h.key)) {
            h.value = "[REDACTED]";
        }
    }
    history_.insert(history_.begin(), sanitized);
    SaveHistoryInternal();
}

void StoreManager::ClearHistory() {
    std::lock_guard<std::mutex> lock(mutex_);
    history_.clear();
    SaveHistoryInternal();
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
