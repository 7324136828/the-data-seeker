#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include "Types.h"
#include "VariableResolver.h"
#include "Assertions.h"
#include "CurlParser.h"
#include "CodeGen.h"
#include "OpenApiParser.h"
#include "DbEngine.h"
#include "StoreManager.h"
#include "ArchiveService.h"
#include "TempWorkspace.h"

using namespace native_app;
namespace fs = std::filesystem;

static int gPassedTests = 0;
static int gFailedTests = 0;

#define RUN_TEST(fn) \
    do { \
        std::cout << "[ RUN      ] " << #fn << std::endl; \
        try { \
            fn(); \
            std::cout << "[       OK ] " << #fn << std::endl; \
            gPassedTests++; \
        } catch (const std::exception& ex) { \
            std::cerr << "[  FAILED  ] " << #fn << ": " << ex.what() << std::endl; \
            gFailedTests++; \
        } catch (...) { \
            std::cerr << "[  FAILED  ] " << #fn << ": unknown exception" << std::endl; \
            gFailedTests++; \
        } \
    } while (0)

static void TestVariableSubstitution() {
    std::map<std::string, std::string> vars = {
        {"base_url", "https://api.example.com"},
        {"version", "v1"},
        {"endpoint", "{{base_url}}/{{version}}/users"},
        {"user_id", "42"}
    };

    std::string substituted = VariableResolver::Substitute("{{endpoint}}/:id", vars);
    if (substituted != "https://api.example.com/v1/users/:id") {
        throw std::runtime_error("Unexpected substitution: " + substituted);
    }

    ApiRequest req;
    req.url = "{{endpoint}}/:id";
    req.pathParams.push_back({"id", "99", true, ""});
    req.params.push_back({"format", "json", true, ""});
    req.headers.push_back({"X-Version", "{{version}}", true, ""});

    ApiRequest prepared = VariableResolver::PrepareRequest(req, vars);
    if (prepared.url != "https://api.example.com/v1/users/99?format=json") {
        throw std::runtime_error("Prepared URL mismatch: " + prepared.url);
    }
    if (prepared.headers.empty() || prepared.headers[0].value != "v1") {
        throw std::runtime_error("Prepared header mismatch");
    }
}

static void TestAssertions() {
    std::vector<TestAssertion> tests = {
        {"status_200", "", "", "", "Status is 200", true},
        {"status_2xx", "", "", "", "Status is 2xx", true},
        {"time_lt_500", "", "", "", "Latency < 500ms", true},
        {"body_contains", "success", "", "", "Body contains success", true},
        {"header_exists", "Content-Type", "", "", "Has content-type", true},
        {"json_field", "data.user.name", "data.user.name", "", "Has user name", true},
        {"json_equals", "DataForge", "data.user.name", "", "User name is DataForge", true}
    };

    std::vector<KeyValuePair> headers = {
        {"Content-Type", "application/json", true, ""}
    };
    std::string body = R"({"data": {"user": {"name": "DataForge", "id": 1}}, "success": true})";

    auto results = AssertionsEvaluator::Evaluate(tests, 200, 120.0, headers, body);
    if (results.size() != tests.size()) {
        throw std::runtime_error("Expected " + std::to_string(tests.size()) + " test results, got " + std::to_string(results.size()));
    }
    for (const auto& r : results) {
        if (!r.passed) {
            throw std::runtime_error("Assertion failed: " + r.name + " (" + r.message + ")");
        }
    }
}

static void TestCurlParser() {
    std::string cmd = "curl -X POST 'https://httpbin.org/post?tag=demo' \\\n"
                      "  -H 'Content-Type: application/json' \\\n"
                      "  -H 'Authorization: Bearer my-secret-token' \\\n"
                      "  -d '{\"title\": \"Test Note\"}' \\\n"
                      "  -L -k";

    ApiRequest req = CurlParser::Parse(cmd);
    if (req.method != HttpMethod::POST) throw std::runtime_error("Expected POST method");
    if (req.url.find("https://httpbin.org/post") == std::string::npos) throw std::runtime_error("URL mismatch");
    if (req.auth.type != AuthType::Bearer || req.auth.token != "my-secret-token") {
        throw std::runtime_error("Bearer auth was not parsed correctly");
    }
    if (req.body.type != BodyType::Json) throw std::runtime_error("Body type should be Json");
    if (req.body.content.find("Test Note") == std::string::npos) throw std::runtime_error("Body content missing");
    if (!req.options.followRedirects || req.options.verifyTls) throw std::runtime_error("Options flags mismatch");
}

static void TestCodeGen() {
    ApiRequest req;
    req.method = HttpMethod::POST;
    req.url = "https://api.example.com/items";
    req.headers.push_back({"Content-Type", "application/json", true, ""});
    req.body.type = BodyType::Json;
    req.body.content = "{\"name\": \"widget\", \"price\": 19.99}";

    std::string curlCode = CodeGen::Generate(req, "curl");
    if (curlCode.find("curl -X POST") == std::string::npos) throw std::runtime_error("cURL codegen failed");

    std::string pyCode = CodeGen::Generate(req, "python");
    if (pyCode.find("import requests") == std::string::npos) throw std::runtime_error("Python codegen failed");

    std::string jsCode = CodeGen::Generate(req, "javascript");
    if (jsCode.find("fetch(") == std::string::npos) throw std::runtime_error("JS fetch codegen failed");

    std::string axiosCode = CodeGen::Generate(req, "axios");
    if (axiosCode.find("axios") == std::string::npos) throw std::runtime_error("Axios codegen failed");
}

static void TestOpenApiParser() {
    std::string spec = R"JSON({
        "openapi": "3.0.0",
        "info": {"title": "Sample API", "version": "1.0.0"},
        "servers": [{"url": "https://api.example.com/v1"}],
        "paths": {
            "/users": {
                "get": {
                    "summary": "List Users",
                    "tags": ["Users"],
                    "parameters": [
                        {"name": "limit", "in": "query", "schema": {"type": "integer"}}
                    ]
                }
            }
        }
    })JSON";

    Collection col = OpenApiParser::Parse(spec);
    if (col.name != "Sample API") throw std::runtime_error("OpenAPI collection name mismatch");
    if (col.folders.empty() || col.folders[0].name != "Users") throw std::runtime_error("Expected Users folder");
    if (col.folders[0].requests.empty() || col.folders[0].requests[0].name != "List Users") {
        throw std::runtime_error("Expected List Users request");
    }
}

static void TestDbEngine() {
    TempWorkspace ws("test_db_");
    DbEngine engine(ws.GetPath().string(), true);

    auto dbs = engine.ListDatabases();
    if (dbs.size() < 2) throw std::runtime_error("Expected at least 2 seeded databases");

    // Test Schema on ecommerce
    DatabaseSchema schema = engine.GetSchema("ecommerce");
    if (schema.tables.empty()) throw std::runtime_error("ecommerce schema has no tables");

    bool hasProducts = false;
    for (const auto& t : schema.tables) {
        if (t.name == "products") {
            hasProducts = true;
            if (t.primaryKeys.empty() || t.primaryKeys[0] != "id") throw std::runtime_error("products primary key mismatch");
            if (t.rowCount <= 0) throw std::runtime_error("products table should have rows");
        }
    }
    if (!hasProducts) throw std::runtime_error("products table missing in schema");

    // Test Query
    QueryResult qr = engine.ExecuteQuery("ecommerce", "SELECT id, name, price FROM products LIMIT 3;");
    if (qr.rows.size() != 3) throw std::runtime_error("Expected 3 products from query");

    // Test Table Data & Pagination
    TableDataResult td = engine.GetTableData("ecommerce", "products", 1, 5);
    if (td.rows.size() != 5) throw std::runtime_error("Expected 5 rows in page 1");
    if (td.totalRows <= 5) throw std::runtime_error("Expected totalRows > 5");

    // Test CRUD
    std::map<std::string, std::string> newRow = {
        {"category_id", "1"},
        {"name", "Unit Test Product"},
        {"sku", "TEST-SKU-001"},
        {"price", "99.99"},
        {"stock", "10"},
        {"status", "active"}
    };
    bool inserted = engine.InsertTableRow("ecommerce", "products", newRow);
    if (!inserted) throw std::runtime_error("Insert table row failed");

    // Update
    std::map<std::string, std::string> pk = {{"sku", "TEST-SKU-001"}};
    std::map<std::string, std::string> updatedData = {{"price", "149.99"}};
    bool updated = engine.UpdateTableRow("ecommerce", "products", pk, updatedData);
    if (!updated) throw std::runtime_error("Update table row failed");

    // Delete
    bool deleted = engine.DeleteTableRow("ecommerce", "products", pk);
    if (!deleted) throw std::runtime_error("Delete table row failed");

    // Export Table Data
    std::string csv = engine.ExportTableData("ecommerce", "categories", "csv");
    if (csv.find("Electronics") == std::string::npos) throw std::runtime_error("CSV export missing data");

    std::string sqlExport = engine.ExportTableData("ecommerce", "categories", "sql");
    if (sqlExport.find("INSERT INTO \"categories\"") == std::string::npos) throw std::runtime_error("SQL export missing statements");

    std::string jsonExport = engine.ExportTableData("ecommerce", "categories", "json");
    if (jsonExport.find("Electronics") == std::string::npos) throw std::runtime_error("JSON export missing data");
}

static void TestStoreManager() {
    TempWorkspace ws("test_store_");
    StoreManager store(ws.GetPath().string());

    auto envs = store.GetEnvironments();
    if (envs.environments.empty()) throw std::runtime_error("Expected default environments");

    auto cols = store.GetCollections();
    if (cols.empty()) throw std::runtime_error("Expected default collections");

    store.SaveGlobals({{"global_key", "global_val", true}});
    auto vars = store.GetMergedVariables("env_local");
    if (vars.find("global_key") == vars.end() || vars["global_key"] != "global_val") {
        throw std::runtime_error("Merged variables missing global variable");
    }

    HistoryEntry h;
    h.id = "hist_1";
    h.method = "GET";
    h.url = "https://httpbin.org/get";
    h.statusCode = 200;
    h.statusText = "OK";
    h.latencyMs = 45.0;
    store.AddHistory(h);

    auto hist = store.GetHistory();
    if (hist.empty() || hist[0].id != "hist_1") throw std::runtime_error("History entry missing");

    store.ClearHistory();
    if (!store.GetHistory().empty()) throw std::runtime_error("History not cleared");
}

static void TestArchiveService() {
    TempWorkspace ws("test_archive_");
    std::string zipPath = ws.GetSubpath("test.zip").string();

    std::map<std::string, std::string> files = {
        {"hello.txt", "Hello World from DataForge Studio Native ZIP packaging!"},
        {"data/numbers.csv", "id,name,value\n1,one,100\n2,two,200\n"},
        {"config.json", "{\"app\": \"DataForge Studio\", \"version\": \"1.0.0\"}\n"}
    };

    bool created = ArchiveService::CreateZip(zipPath, files);
    if (!created) throw std::runtime_error("Failed to create ZIP archive");
    if (!fs::exists(zipPath) || fs::file_size(zipPath) == 0) {
        throw std::runtime_error("ZIP archive file is missing or empty");
    }
}

static void TestTempWorkspace() {
    fs::path createdPath;
    {
        TempWorkspace ws("ws_scope_");
        createdPath = ws.GetPath();
        if (!fs::exists(createdPath)) throw std::runtime_error("Workspace path does not exist");

        auto sub = ws.GetSubpath("sub/file.txt");
        fs::create_directories(sub.parent_path());
        std::ofstream f(sub);
        f << "data";
        f.close();
        if (!fs::exists(sub)) throw std::runtime_error("Subpath file missing");

        bool caught = false;
        try {
            ws.GetSubpath("../../escaped.txt");
        } catch (...) {
            caught = true;
        }
        if (!caught) throw std::runtime_error("Path traversal was not prevented");
    }
    // Workspace should be cleaned up on scope exit
    if (fs::exists(createdPath)) {
        throw std::runtime_error("Workspace was not automatically cleaned up");
    }
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "   DataForge Studio Native Test Suite   " << std::endl;
    std::cout << "========================================" << std::endl;

    RUN_TEST(TestVariableSubstitution);
    RUN_TEST(TestAssertions);
    RUN_TEST(TestCurlParser);
    RUN_TEST(TestCodeGen);
    RUN_TEST(TestOpenApiParser);
    RUN_TEST(TestDbEngine);
    RUN_TEST(TestStoreManager);
    RUN_TEST(TestArchiveService);
    RUN_TEST(TestTempWorkspace);

    std::cout << "========================================" << std::endl;
    std::cout << "Total Passed: " << gPassedTests << std::endl;
    std::cout << "Total Failed: " << gFailedTests << std::endl;
    std::cout << "========================================" << std::endl;

    return gFailedTests == 0 ? 0 : 1;
}
