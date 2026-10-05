#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winsqlite/winsqlite3.h>
#include <iostream>
#include <fstream>
#include <cassert>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <atomic>
#include <thread>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <nlohmann/json.hpp>
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
#include "HttpEngine.h"
#include "ProcessRunner.h"
#pragma comment(lib, "ws2_32.lib")

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
    const auto insertedRow = engine.ExecuteQuery("ecommerce", "SELECT id FROM products WHERE sku='TEST-SKU-001';");
    if (insertedRow.rows.size() != 1) throw std::runtime_error("Inserted product missing");
    std::map<std::string, std::string> pk = {{"id", insertedRow.rows[0].at("id")}};
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
    if (!envs.environments.empty()) throw std::runtime_error("Fresh workspace should have no environments");

    auto cols = store.GetCollections();
    if (!cols.empty()) throw std::runtime_error("Fresh workspace should have no collections");

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

static void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static std::string ReadText(const fs::path& path) { std::ifstream file(path, std::ios::binary); return std::string((std::istreambuf_iterator<char>(file)), {}); }

static void TestRequestEdgeCases() {
    ApiRequest request;
    request.url = "https://example.com/:id/:identity#fragment";
    request.pathParams = {{"id", "a/b", true, ""}};
    request.params = {{"q", "a b", true, ""}};
    request.auth.type = AuthType::ApiKey; request.auth.in = "query"; request.auth.key = "api_key"; request.auth.value = "v+v";
    auto prepared = VariableResolver::PrepareRequest(request, {});
    Require(prepared.url == "https://example.com/a%2Fb/:identity?q=a%20b&api_key=v%2Bv#fragment", "URL path boundary/query fragment incorrect");
    request.url = "https://example.com"; request.headers = {{"X-Test", "{{missing}}", true, ""}};
    bool rejected = false; try { VariableResolver::PrepareRequest(request, {}, true); } catch (...) { rejected = true; }
    Require(rejected, "Strict missing header variable should be rejected");
    request.headers[0].enabled = false;
    VariableResolver::PrepareRequest(request, {}, true);
    Require(VariableResolver::UrlDecode("%GG%2Fa+b") == "%GG/a b", "Malformed URL escape altered");
    Require(VariableResolver::RedactSensitive("token", "{{token}} trailing-secret") == "[REDACTED]", "Partial variable reference leaked secret suffix");
    request.url = "https://example.com/files/{id}.json"; request.auth.type = AuthType::None; request.headers.clear(); request.params.clear();
    request.pathParams = {{"id", "a/b", true, ""}};
    Require(VariableResolver::PrepareRequest(request, {}, true).url == "https://example.com/files/a%2Fb.json", "Embedded OpenAPI path template not replaced");
    request.pathParams[0].value = "{{missing}}";
    rejected = false; try { VariableResolver::PrepareRequest(request, {}, true); } catch (...) { rejected = true; }
    Require(rejected, "Path URL encoding concealed unresolved variables");
    request.pathParams.clear(); request.params = {{"q", "{{missing}}", true, ""}};
    rejected = false; try { VariableResolver::PrepareRequest(request, {}, true); } catch (...) { rejected = true; }
    Require(rejected, "Query URL encoding concealed unresolved variables");
}

static void TestDatabaseAtomicityAndErrors() {
    TempWorkspace workspace("batch_");
    DbEngine engine(workspace.GetPath().u8string(), false);
    const auto db = engine.CreateDatabase("Batch test");
    auto result = engine.ExecuteQuery(db.id, "CREATE TABLE entries (id INTEGER PRIMARY KEY, label TEXT UNIQUE); INSERT INTO entries VALUES(1,'first');");
    if (!result.error.empty()) throw std::runtime_error("Create/insert batch failed: " + result.error);
    Require(engine.ExecuteQuery(db.id, "SELECT * FROM entries;").rows.size() == 1, "DML never executed");
    result = engine.ExecuteQuery(db.id, "INSERT INTO entries VALUES(2,'second'); INSERT INTO entries VALUES(3,'first');");
    Require(!result.error.empty() && result.rowsAffected == 0, "Constraint errors not reported");
    Require(engine.ExecuteQuery(db.id, "SELECT * FROM entries;").rows.size() == 1, "Failed batch wasn't rolled back");
    result = engine.ExecuteQuery(db.id, "UPDATE entries SET label='updated' WHERE id=1; SELECT label FROM entries;");
    Require(result.error.empty() && result.rowsAffected == 1 && result.rows[0].at("label") == "updated", "UPDATE or affected count incorrect");
    Require(!engine.ExecuteQuery("../outside", "SELECT 1;").error.empty(), "Unknown database id created a file");
    Require(!fs::exists(workspace.GetPath().parent_path() / "outside.db"), "Unknown id escaped workspace");
    result = engine.ExecuteQuery(db.id, "SELECT ';' AS value; -- comment\n SELECT 42 AS answer;");
    Require(result.error.empty() && result.rows[0].at("answer") == "42", "Statements with comments or literals split incorrectly");
    Require(result.resultSets.size() == 2 && result.resultSets[0].rows[0].at("value") == ";", "Earlier result sets were discarded");
    result = engine.ExecuteQuery(db.id, "SELECT 1 AS id,2 AS id,3 AS 'id (2)',NULL AS empty,X'00FF' AS bytes;");
    Require(result.error.empty() && result.columns[0] != result.columns[1] && result.columns[1] != result.columns[2] && result.rows[0].at(result.columns[0]) == "1" && result.rows[0].at(result.columns[1]) == "2" && result.rows[0].at(result.columns[2]) == "3", "Duplicate SQL labels overwrote values");
    Require(result.typedRows[0].at("empty").type == DbValueType::Null && result.typedRows[0].at("bytes").type == DbValueType::Blob && result.typedRows[0].at("bytes").text == std::string("\0\xff",2), "Query results lost NULL/BLOB types");
    Require(!engine.UpdateTableRow(db.id, "entries", {}, {{"label", "bad"}}), "Update without PK allowed");
    Require(!engine.DeleteTableRow(db.id, "entries", {{"label", "updated"}}), "Delete by non-PK allowed");
    Require(!engine.DeleteTableRow(db.id, "entries", {{"id", "999"}}), "Missing row reported deleted");
    Require(!engine.GetTableData(db.id, "entries", 1, 10, "missing").error.empty(), "Unknown sort column accepted");
    engine.ExecuteQuery(db.id, "INSERT INTO entries VALUES(2,'100% correct'),(3,'under_score'),(4,'100abc correct');");
    Require(engine.GetTableData(db.id, "entries", 1, 10, "", "asc", "%").totalRows == 1, "Search wildcard was not escaped");
    Require(engine.GetTableData(db.id, "entries", 1, 10, "", "asc", "100", "label", "correct").totalRows == 2, "Combined filter/search incorrect");
    std::atomic_bool cancelled{true};
    Require(engine.ExecuteQuery(db.id, "SELECT 1;", 1000, &cancelled).error == "Query cancelled.", "Pre-cancelled SQL was executed");
    cancelled.store(false);
    std::thread cancelThread([&] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); cancelled.store(true); });
    result = engine.ExecuteQuery(db.id, "WITH RECURSIVE sequence(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM sequence WHERE n<100000000) SELECT sum(n) FROM sequence;", 1000, &cancelled);
    cancelThread.join();
    Require(result.error == "Query cancelled.", "Active SQL cancellation failed");
}

static void TestDatabaseFullExportAndUnicode() {
    TempWorkspace workspace("unicode_");
    const auto directory = workspace.GetSubpath(u8"path with spaces/資料");
    DbEngine engine(directory.u8string(), false);
    const auto db = engine.CreateDatabase("Export data");
    auto result = engine.ExecuteQuery(db.id, "CREATE TABLE values_table(id INTEGER PRIMARY KEY, data TEXT, optional TEXT, amount REAL, bytes BLOB); WITH RECURSIVE sequence(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM sequence WHERE n<450) INSERT INTO values_table SELECT n, 'NULL', NULL, 1.5, X'00FF' FROM sequence;");
    if (!result.error.empty()) throw std::runtime_error("Large Unicode-path DB setup failed: " + result.error);
    const auto exported = nlohmann::json::parse(engine.ExportTableData(db.id, "values_table", "json"));
    Require(exported.size() == 450, "Full export silently truncated after 200 rows");
    Require(exported[0]["id"].is_number_integer() && exported[0]["optional"].is_null() && exported[0]["data"] == "NULL" && exported[0]["amount"].is_number_float(), "JSON export lost native types/NULL distinction");
    const auto sql = engine.ExportTableData(db.id, "values_table", "sql");
    Require(sql.find("'NULL', NULL") != std::string::npos && sql.find("X'00ff'") != std::string::npos, "SQL export NULL/text/blob incorrect");
    const auto formatted = engine.FormatQuery("select 'select from where', \"select\" from values_table -- select stays\n/* where stays */ where id=1;");
    Require(formatted == "SELECT 'select from where', \"select\" FROM values_table -- select stays\n/* where stays */ WHERE id=1;", "Formatting changed string/comment content");
    DbEngine restarted(directory.u8string(), false);
    Require(restarted.ExecuteQuery(db.id, "SELECT COUNT(*) AS count FROM values_table;").rows[0].at("count") == "450", "Custom databases not discovered after restart");
    const auto attached = engine.AttachDatabase("User database", db.path);
    // Attach a file outside this manager's directory and prove detach preserves it.
    DbEngine other(workspace.GetSubpath("second manager").u8string(), false);
    const auto userFile = other.AttachDatabase("External file", db.path);
    DbEngine reattached(workspace.GetSubpath("second manager").u8string(), false);
    Require(reattached.ListDatabases().size() == 1 && reattached.ExecuteQuery(userFile.id, "SELECT COUNT(*) AS count FROM values_table;").rows[0].at("count") == "450", "Attached database profile lost after restart");
    Require(other.DeleteDatabase(userFile.id) && fs::exists(fs::u8path(db.path)), "Detach deleted the user's original database");
}

static void TestDatabaseMaintenanceAndCancellation() {
    TempWorkspace workspace("db_cancel_");
    DbEngine engine(workspace.GetPath().u8string(), false);
    const auto database = engine.CreateDatabase("Maintenance");
    auto query = engine.ExecuteQuery(database.id, "CREATE TABLE data(id INTEGER PRIMARY KEY, label TEXT); INSERT INTO data VALUES(1,'kept'); CREATE TABLE auxiliary(value INTEGER);");
    Require(query.error.empty() && query.rowsAffected == 1, "DDL reused a preceding mutation's affected count");
    Require(engine.ExecuteQuery(database.id, "VACUUM;").error.empty(), "VACUUM was trapped in an implicit transaction");
    query = engine.ExecuteQuery(database.id, "PRAGMA journal_mode = WAL;");
    Require(query.error.empty() && !query.rows.empty() && query.rows[0].at("journal_mode") == "wal", "Writable PRAGMA was trapped in an implicit transaction");
    Require(engine.ExecuteQuery(database.id, "PRAGMA journal_mode = DELETE;").error.empty(), "PRAGMA journal mode could not be restored");
    std::atomic_bool cancellation{true};
    auto cancelled = [&](auto operation) {
        try { operation(); return false; } catch (const std::exception& error) { return std::string(error.what()) == "Query cancelled."; }
    };
    Require(cancelled([&] { engine.GetSchema(database.id, &cancellation); }), "Pre-cancelled schema inspection ran");
    Require(cancelled([&] { engine.GetErDiagram(database.id, &cancellation); }), "Pre-cancelled ER inspection ran");
    Require(engine.GetTableData(database.id, "data", 1, 25, "", "asc", "", "", "", &cancellation).error == "Query cancelled.", "Pre-cancelled grid load ran");
    Require(cancelled([&] { engine.ExportTableData(database.id, "data", "json", "", "", "asc", &cancellation); }), "Pre-cancelled export ran");
    cancellation.store(false);
    query = engine.ExecuteQuery(database.id, "CREATE VIEW expensive AS WITH RECURSIVE sequence(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM sequence WHERE n<100000000) SELECT n FROM sequence;");
    Require(query.error.empty(), "Cancellation fixture view creation failed");
    auto runCancelled = [&](auto operation) {
        cancellation.store(false);
        std::thread cancel([&] { Sleep(20); cancellation.store(true); });
        const auto start = std::chrono::steady_clock::now();
        bool stopped = false;
        try { stopped = operation(); } catch (...) { cancel.join(); throw; }
        cancel.join();
        Require(stopped, "Active database operation did not report cancellation");
        Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "Database cancellation exceeded one second");
    };
    runCancelled([&] { return engine.GetTableData(database.id, "expensive", 1, 25, "", "asc", "", "", "", &cancellation).error == "Query cancelled."; });
    runCancelled([&] { return cancelled([&] { engine.ExportTableData(database.id, "expensive", "csv", "", "", "asc", &cancellation); }); });
    // A held exclusive lock exercises cancellation in SQLite's busy handler,
    // including schema preparation before any row-processing callback runs.
    sqlite3* raw = nullptr;
    Require(sqlite3_open(database.path.c_str(), &raw) == SQLITE_OK, "Native lock fixture open failed");
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> locker(raw, &sqlite3_close);
    Require(sqlite3_exec(locker.get(), "BEGIN EXCLUSIVE;", nullptr, nullptr, nullptr) == SQLITE_OK, "Exclusive database lock fixture failed");
    runCancelled([&] { return cancelled([&] { engine.GetSchema(database.id, &cancellation); }); });
    sqlite3_exec(locker.get(), "ROLLBACK;", nullptr, nullptr, nullptr);
    cancellation.store(false);
    Require(engine.GetSchema(database.id, &cancellation).tables.size() == 3 && engine.GetTableData(database.id, "data").rows[0].at("label") == "kept", "Cancelled read changed the source or leaked its connection");
}
static void TestStorePersistenceAndProtection() {
    TempWorkspace workspace("store_protection_");
    const auto directory = workspace.GetSubpath(u8"local settings/設定");
    StoreManager store(directory.u8string());
    Collection collection;
    collection.id = "persisted"; collection.name = "Saved collection";
    collection.auth.type = AuthType::Bearer; collection.auth.token = "fake-collection-credential";
    collection.variables = {{"scope", "collection", true}};
    Folder folder; folder.id = "nested"; folder.name = "Nested folder"; folder.auth.type = AuthType::Basic; folder.auth.password = "fake-folder-password";
    folder.variables = {{"scope", "folder", true}};
    ApiRequest request; request.id = "request"; request.url = "https://example.com"; request.auth.token = "fake-request-credential";
    request.auth.username = "fake-private-user";
    request.tests = {{"body_contains", "fake-assertion-value", "private-field", "X-Private", "Saved test", true}};
    request.options.timeoutSec = 3.25; request.options.followRedirects = false; request.options.verifyTls = false;
    request.variables = {{"scope", "request", true, "test"}};
    request.body.type = BodyType::FormData; FormItem file; file.key = "file"; file.type = "file"; file.contentBase64 = "AAECAw=="; request.body.formItems.push_back(file);
    folder.requests.push_back(request); collection.folders.push_back(folder);
    store.SaveCollections({collection});
    store.SaveGlobals({{"password", "fake-global-password", true}});
    StoreManager restarted(directory.u8string());
    const auto loaded = restarted.GetCollections()[0];
    Require(loaded.auth.token == collection.auth.token && loaded.variables[0].value == "collection", "Collection scope lost on persistence");
    Require(loaded.folders[0].auth.password == folder.auth.password && loaded.folders[0].variables[0].value == "folder", "Folder scope lost on persistence");
    const auto loadedRequest = loaded.folders[0].requests[0];
    Require(loadedRequest.auth.username == request.auth.username && loadedRequest.tests[0].expected == request.tests[0].expected && loadedRequest.tests[0].field == "private-field", "Protected username/assertions did not round-trip");
    Require(loadedRequest.variables.size() == 1 && loadedRequest.options.timeoutSec == 3.25 && !loadedRequest.options.followRedirects && loadedRequest.body.formItems[0].contentBase64 == "AAECAw==", "Request options/variables/file data lost after restart");
    Require(restarted.GetGlobals()[0].value == "fake-global-password", "DPAPI credential did not round-trip");
    const auto disk = ReadText(directory / "collections.json") + ReadText(directory / "globals.json");
    Require(disk.find("fake-") == std::string::npos && disk.find("dpapi:v1:") != std::string::npos, "Saved credential values are plaintext");
    HistoryEntry history; history.id = "safe"; history.requestSnapshot = request;
    history.url = "https://fake-user:fake-pass@example.com/private-path?arbitrary=fake-secret#fake-fragment";
    history.requestSnapshot.url = history.url;
    history.requestSnapshot.body.content = "fake-private-body";
    history.requestSnapshot.body.formItems[0].value = "fake-private-file"; history.requestSnapshot.body.formItems[0].filename = "fake-private-name";
    history.requestSnapshot.params = {{"unrecognized", "fake-query-value", true, ""}};
    history.requestSnapshot.pathParams = {{"item", "fake-path-value", true, ""}};
    history.requestSnapshot.headers = {{"X-Custom", "fake-custom-header", true, ""}};
    history.requestSnapshot.auth.password = "fake-history-password"; history.requestSnapshot.auth.value = "fake-api-key";
    store.AddHistory(history);
    const auto saved = store.GetHistory()[0].requestSnapshot.auth;
    Require(saved.token == "[REDACTED]" && saved.password == "[REDACTED]" && saved.value == "[REDACTED]", "History auth values not redacted");
    const auto safeHistory = store.GetHistory()[0]; const auto& safeSnapshot = safeHistory.requestSnapshot;
    Require(safeSnapshot.id == request.id && safeHistory.url == "https://example.com/[REDACTED]" && safeSnapshot.url == safeHistory.url, "History URL exposed credentials or lost saved identity");
    Require(safeSnapshot.auth.username == "[REDACTED]" && safeSnapshot.params[0].value == "[REDACTED]" && safeSnapshot.pathParams[0].value == "[REDACTED]" && safeSnapshot.headers[0].value == "[REDACTED]", "History URL/header/parameter values exposed");
    Require(safeSnapshot.variables[0].value == "[REDACTED]" && safeSnapshot.body.content == "[REDACTED]" && safeSnapshot.body.formItems[0].contentBase64 == "[REDACTED]" && safeSnapshot.body.formItems[0].filename == "[REDACTED]" && safeSnapshot.tests[0].expected == "[REDACTED]", "History body/files/variables/assertions exposed");
    StoreManager historyRestart(directory.u8string());
    Require(historyRestart.GetHistory()[0].requestSnapshot.url == safeHistory.url && historyRestart.GetCollections()[0].folders[0].requests[0].auth.username == request.auth.username, "History redaction damaged protected saved context");
    {
        std::ofstream legacy(directory / "history.json", std::ios::binary);
        legacy << R"([{"id":"old","url":"https://legacy.example/private?token=fake-legacy-secret","request_snapshot":{"id":"request","url":"https://legacy.example/private","auth":{"username":"fake-legacy-user","token":"fake-legacy-token"},"body":{"content":"fake-legacy-body"}}}])";
    }
    StoreManager legacy(directory.u8string());
    Require(legacy.GetHistory()[0].url == "https://legacy.example/[REDACTED]" && legacy.GetHistory()[0].requestSnapshot.auth.username == "[REDACTED]" && legacy.GetHistory()[0].requestSnapshot.body.content == "[REDACTED]", "Legacy plaintext history was exposed on load");
    // An existing valid empty store must remain empty across startup.
    store.SaveCollections({}); store.SaveEnvironments({});
    StoreManager empty(directory.u8string());
    Require(empty.GetCollections().empty() && empty.GetEnvironments().environments.empty(), "Empty stores unexpectedly reseeded");
    const auto before = ReadText(directory / "collections.json");
    { std::ofstream corrupted(directory / "collections.json"); corrupted << "{broken"; }
    bool rejected = false; try { StoreManager malformed(directory.u8string()); } catch (...) { rejected = true; }
    Require(rejected && ReadText(directory / "collections.json") == "{broken", "Malformed store was silently overwritten");
}

class LocalHttpServer {
public:
    std::string request;
    explicit LocalHttpServer(bool delay = false) {
        WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("Winsock initialization failed");
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (socket_ == INVALID_SOCKET || bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(socket_, 1)) throw std::runtime_error("Loopback server setup failed");
        int size = sizeof(address); getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size); port_ = ntohs(address.sin_port);
        worker_ = std::thread([this, delay] {
            WSAPOLLFD descriptor{socket_, POLLRDNORM, 0};
            if (WSAPoll(&descriptor, 1, 5000) <= 0) return;
            SOCKET client = accept(socket_, nullptr, nullptr); if (client == INVALID_SOCKET) return;
            DWORD timeout = 1000; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            char buffer[4096];
            for (;;) {
                const int size = recv(client, buffer, sizeof(buffer), 0); if (size <= 0) break;
                request.append(buffer, size);
                const auto headerEnd = request.find("\r\n\r\n");
                if (headerEnd == std::string::npos) continue;
                size_t length = 0;
                const auto contentLength = request.find("Content-Length:");
                if (contentLength != std::string::npos) length = std::stoul(request.substr(contentLength + 15));
                if (request.size() >= headerEnd + 4 + length) break;
            }
            if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\nConnection: close\r\n\r\n{\"ok\":true}";
            send(client, response.data(), static_cast<int>(response.size()), 0);
            closesocket(client);
        });
    }
    ~LocalHttpServer() { if (worker_.joinable()) worker_.join(); if (socket_ != INVALID_SOCKET) closesocket(socket_); WSACleanup(); }
    std::string Url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/echo"; }
    void Join() { if (worker_.joinable()) worker_.join(); }
private:
    SOCKET socket_ = INVALID_SOCKET; uint16_t port_ = 0; std::thread worker_;
};

static void TestHttpNativeLifecycle() {
    HttpEngine engine;
    ApiRequest request; request.url = "not a URL";
    Require(!engine.Execute(request).error.empty(), "Invalid HTTP URL accepted");
    request.url = "http://127.0.0.1:1"; request.options.timeoutSec = 0;
    Require(!engine.Execute(request).error.empty(), "Invalid timeout accepted");
    request.options.timeoutSec = 2;
    request.headers = {{"X-Injected", "a\r\nX-Evil: b", true, ""}};
    Require(!engine.Execute(request).error.empty(), "Header injection accepted");
    request.headers.clear(); std::atomic_bool cancelled{true};
    Require(engine.Execute(request, {}, true, &cancelled).error == "Request cancelled.", "Pre-cancelled request attempted I/O");
    LocalHttpServer server;
    request.url = server.Url(); request.method = HttpMethod::POST; request.body.type = BodyType::FormData;
    FormItem file; file.key = "upload"; file.type = "file"; file.filename = "bytes.bin"; file.contentBase64 = "AAECAw=="; request.body.formItems = {file};
    auto response = engine.Execute(request);
    server.Join();
    Require(response.error.empty() && response.statusCode == 200 && response.isJson && response.body == "{\"ok\":true}", "Native loopback HTTP workflow failed");
    Require(server.request.find(std::string("\0\1\2\3", 4)) != std::string::npos, "Multipart file Base64 was not decoded");
    LocalHttpServer delayed(true);
    request.url = delayed.Url(); request.body = {}; cancelled.store(false);
    std::thread canceller([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); cancelled.store(true); });
    response = engine.Execute(request, {}, true, &cancelled);
    canceller.join();
    Require(response.error == "Request cancelled." && response.latencyMs < 1000, "Blocking WinHTTP call did not cancel promptly");
}

static void TestWorkspaceOwnershipAndArchiveBoundaries() {
    TempWorkspace first("unique_"); TempWorkspace second("unique_");
    Require(first.GetPath() != second.GetPath(), "Temporary workspace collision");
    for (const std::string path : {"../escape", "C:/escape", "stream:secret", "NUL", "trailing.", "sub/../../escape"}) {
        bool rejected = false; try { first.GetSubpath(path); } catch (...) { rejected = true; }
        Require(rejected, "Unsafe workspace path accepted");
    }
    bool rejected = false; try { TempWorkspace invalid("../escape"); } catch (...) { rejected = true; }
    Require(rejected, "Untrusted workspace prefix accepted");
    const auto marker = first.GetPath() / ".workspace-owner";
    const auto owner = ReadText(marker);
    { std::ofstream changed(marker); changed << "wrong owner"; }
    rejected = false; try { first.Cleanup(); } catch (...) { rejected = true; }
    Require(rejected && fs::exists(first.GetPath()), "Cleanup ignored ownership marker");
    { std::ofstream restored(marker); restored << owner; }
    const auto archive = first.GetSubpath(u8"資料.zip");
    Require(ArchiveService::CreateZip(archive.u8string(), {{u8"資料.txt", "saved"}}), "Unicode archive creation failed");
    const auto original = ReadText(archive);
    Require(!ArchiveService::CreateZip(archive.u8string(), {{"../escape.txt", "unsafe"}}) && ReadText(archive) == original, "Unsafe ZIP overwrote original destination");
    const auto bytes = ReadText(archive);
    Require(bytes.size() > 30 && static_cast<unsigned char>(bytes[7]) == 8, "ZIP UTF-8 name flag missing");
}
static void TestNativeProcessLifecycle() {
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    const auto application = fs::path(path).u8string();
    const auto result = ProcessRunner::Run(application, {"--echo-args", "", "a b", "embedded\"quote", "trailing\\", u8"資料"});
    std::string normalized = result.stdOut; normalized.erase(std::remove(normalized.begin(), normalized.end(), '\r'), normalized.end());
    if (!result.success || normalized != std::string("0:\n3:a b\n14:embedded\"quote\n9:trailing\\\n6:") + u8"資料\n")
        throw std::runtime_error("Native process argument escaping incorrect: " + result.stdErr + " output=" + result.stdOut);
    const auto timed = ProcessRunner::Run(application, {"--wait"}, "", 50);
    Require(!timed.success && timed.stdErr.find("timed out") != std::string::npos, "Native helper timeout failed");
    Require(!ProcessRunner::Run("missing.exe", {}).success, "Relative/missing helper was launched");
}

static void TestDatabaseChosenDestination() {
    TempWorkspace workspace("chosen_database_");
    const auto manager = workspace.GetSubpath("managed");
    const auto destination = workspace.GetSubpath(u8"user files/資料 chosen.db");
    fs::create_directories(destination.parent_path());
    DbEngine engine(manager.u8string(), false);
    const auto created = engine.CreateDatabase("Chosen sample", "ecommerce", destination.u8string());
    Require(created.path == destination.u8string() && fs::is_regular_file(destination), "Database wizard destination ignored");
    Require(!engine.GetSchema(created.id).tables.empty(), "Chosen sample database not initialized");
    const auto original = ReadText(destination);
    bool rejected = false; try { engine.CreateDatabase("Overwrite", "blank", destination.u8string()); } catch (...) { rejected = true; }
    Require(rejected && ReadText(destination) == original, "Chosen destination overwritten");
    rejected = false; try { engine.CreateDatabase("Missing parent", "blank", workspace.GetSubpath("missing parent/new.db").u8string()); } catch (...) { rejected = true; }
    Require(rejected, "Missing destination parent accepted");
    DbEngine restarted(manager.u8string(), false);
    Require(!restarted.GetSchema(created.id).tables.empty(), "Chosen destination profile lost after restart");
    Require(restarted.DeleteDatabase(created.id) && fs::exists(destination), "Detached chosen database file was deleted");
}

static void TestOpenApiJsonParityAndRejections() {
    const std::string spec = R"JSON({
      "openapi":"3.1.0","info":{"title":"Parity API"},
      "servers":[{"url":"https://{region}.example.com/v1/","variables":{"region":{"default":"api"}}}],
      "security":[{"bearer":[]}],
      "components":{"securitySchemes":{"bearer":{"type":"http","scheme":"bearer"}},
        "parameters":{"Path/Param~one":{"name":"id","in":"path","required":true,"schema":{"type":"string","default":"a/b"}}}},
      "paths":{"/users/{id}":{"parameters":[{"$ref":"#/components/parameters/Path~1Param~0one"},{"name":"limit","in":"query","schema":{"type":"integer","default":2}}],
        "post":{"tags":["Users"],"summary":"Create user","parameters":[{"name":"limit","in":"query","schema":{"type":"integer","default":9}}],
          "requestBody":{"content":{"application/json":{"schema":{"allOf":[{"type":"object","properties":{"name":{"type":"string","enum":["demo","other"]},"serverOnly":{"type":"integer","readOnly":true}}},{"type":"object","properties":{"enabled":{"type":"boolean","default":false}}}]}}}},
          "responses":{"201":{"description":"Created"}}}}}
    })JSON";
    const auto imported = OpenApiParser::Parse(spec);
    Require(imported.folders.size() == 1 && imported.folders[0].requests.size() == 1, "Tagged OpenAPI request not preserved");
    const auto& request = imported.folders[0].requests[0];
    Require(request.params.size() == 1 && request.params[0].value == "9" && request.params[0].enabled, "Shared/overridden OpenAPI defaults lost");
    Require(request.pathParams.size() == 1 && request.pathParams[0].value == "a/b", "Escaped JSON pointer/path parameters failed");
    Require(request.auth.type == AuthType::Bearer && request.auth.token == "{{access_token}}", "OpenAPI security not imported");
    const auto sample = nlohmann::json::parse(request.body.content);
    Require(sample["name"] == "demo" && sample["enabled"] == false && !sample.contains("serverOnly"), "Schema enum/composite/readOnly sample incorrect");
    Require(request.tests[0].type == "status_code" && request.tests[0].expected == "201", "Declared success assertion lost");
    const auto prepared = VariableResolver::PrepareRequest(request, {{"access_token", "synthetic-token"}}, true);
    Require(prepared.url == "https://api.example.com/v1/users/a%2Fb?limit=9", "OpenAPI brace path or server variable not dispatchable");
    auto overrideServer = nlohmann::json::parse(spec);
    overrideServer["paths"]["/users/{id}"]["servers"] = {{{"url", "https://path.example.com"}}};
    overrideServer["paths"]["/users/{id}"]["post"]["servers"] = {{{"url", "https://operation.example.com"}}};
    Require(OpenApiParser::Parse(overrideServer.dump()).folders[0].requests[0].url == "https://operation.example.com/users/{id}", "Operation server override ignored");
    const auto swagger = OpenApiParser::Parse(R"({"swagger":"2.0","info":{"title":"Swagger"},"host":"swagger.example.com","basePath":"/v2","schemes":["https"],"securityDefinitions":{"key":{"type":"apiKey","name":"key","in":"query"}},"security":[{"key":[]}],"paths":{"/items":{"post":{"parameters":[{"name":"body","in":"body","schema":{"type":"object","properties":{"label":{"type":"string","default":"saved"}}}}],"responses":{"200":{"description":"OK"}}}}}})");
    Require(swagger.requests[0].body.type == BodyType::Json && swagger.requests[0].body.content.find("saved") != std::string::npos, "Swagger body schema lost");
    Require(VariableResolver::PrepareRequest(swagger.requests[0], {{"api_key", "a b"}}, true).url == "https://swagger.example.com/v2/items?key=a%20b", "Swagger API-key authentication incorrect");
    const auto form = OpenApiParser::Parse(R"({"swagger":"2.0","paths":{"/form":{"post":{"consumes":["multipart/form-data"],"parameters":[{"name":"title","in":"formData","type":"string","default":"sample"}],"responses":{}}}}})");
    Require(form.requests[0].body.type == BodyType::FormData && form.requests[0].body.formItems[0].value == "sample", "Swagger form body lost");
    for (const std::string invalid : {
        "{}", "[]", R"({"info":{"name":"Postman"},"item":"invalid","event":[]})",
        R"({"openapi":"3.0.0","paths":{"/x":{"$ref":"https://example.com/spec.json"}}})",
        R"({"openapi":"3.0.0","paths":{"/x":{"get":{"parameters":[{"$ref":"#/components/parameters/missing"}]}}}})",
        R"({"openapi":"3.0.0","paths":{"/x":{"get":{"responses":{"200":{"$ref":"#/components/responses/missing"}}}}}})",
        R"({"openapi":"3.0.0","paths":{"/x":{"get":{"preRequestScript":42}}}})"}) {
        bool rejected = false; try { OpenApiParser::Parse(invalid); } catch (...) { rejected = true; }
        Require(rejected, "Invalid API import input silently accepted");
    }
    bool rejected = false; try { OpenApiParser::Parse(std::string(16 * 1024 * 1024 + 1, ' ')); } catch (...) { rejected = true; }
    Require(rejected, "Oversized OpenAPI input accepted");
}

void TestNativeImportFormats();
void TestNativeCurlImportRoundTrips();
void TestNativeTypedProfiles();
void TestNativeDuckDbProvider();
void TestNativeRedisProtocol();
void TestNativeMockRestService();
void TestNativeSnippetBehavior();
void TestNativeScriptRuntime();
void TestNativeScriptPersistence();
int wmain(int argc, wchar_t* argv[]) {
    if (argc > 1 && std::wstring(argv[1]) == L"--wait") { Sleep(1000); return 0; }
    if (argc > 1 && std::wstring(argv[1]) == L"--echo-args") {
        for (int i = 2; i < argc; ++i) {
            const auto argument = fs::path(argv[i]).u8string();
            std::cout << argument.size() << ':' << argument << '\n';
        }
        return 0;
    }
    std::cout << "========================================" << std::endl;
    std::cout << "   DataForge Studio Native Test Suite   " << std::endl;
    std::cout << "========================================" << std::endl;

    RUN_TEST(TestNativeImportFormats);
    RUN_TEST(TestNativeCurlImportRoundTrips);
    RUN_TEST(TestNativeTypedProfiles);
    RUN_TEST(TestNativeDuckDbProvider);
    RUN_TEST(TestNativeRedisProtocol);
    RUN_TEST(TestNativeMockRestService);
    RUN_TEST(TestNativeSnippetBehavior);
    RUN_TEST(TestNativeScriptRuntime);
    RUN_TEST(TestNativeScriptPersistence);
    RUN_TEST(TestVariableSubstitution);
    RUN_TEST(TestAssertions);
    RUN_TEST(TestCurlParser);
    RUN_TEST(TestCodeGen);
    RUN_TEST(TestOpenApiParser);
    RUN_TEST(TestDbEngine);
    RUN_TEST(TestStoreManager);
    RUN_TEST(TestArchiveService);
    RUN_TEST(TestTempWorkspace);
    RUN_TEST(TestRequestEdgeCases);
    RUN_TEST(TestDatabaseAtomicityAndErrors);
    RUN_TEST(TestDatabaseFullExportAndUnicode);
    RUN_TEST(TestDatabaseMaintenanceAndCancellation);
    RUN_TEST(TestStorePersistenceAndProtection);
    RUN_TEST(TestHttpNativeLifecycle);
    RUN_TEST(TestWorkspaceOwnershipAndArchiveBoundaries);
    RUN_TEST(TestNativeProcessLifecycle);
    RUN_TEST(TestDatabaseChosenDestination);
    RUN_TEST(TestOpenApiJsonParityAndRejections);

    std::cout << "========================================" << std::endl;
    std::cout << "Total Passed: " << gPassedTests << std::endl;
    std::cout << "Total Failed: " << gFailedTests << std::endl;
    std::cout << "========================================" << std::endl;

    return gFailedTests == 0 ? 0 : 1;
}
