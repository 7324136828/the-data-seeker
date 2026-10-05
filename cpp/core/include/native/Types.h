#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>
#include <chrono>

namespace native_app {

enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE_,
    PATCH,
    HEAD,
    OPTIONS
};

inline std::string HttpMethodToString(HttpMethod m) {
    switch (m) {
    case HttpMethod::GET: return "GET";
    case HttpMethod::POST: return "POST";
    case HttpMethod::PUT: return "PUT";
    case HttpMethod::DELETE_: return "DELETE";
    case HttpMethod::PATCH: return "PATCH";
    case HttpMethod::HEAD: return "HEAD";
    case HttpMethod::OPTIONS: return "OPTIONS";
    }
    return "GET";
}

inline HttpMethod HttpMethodFromString(const std::string& str) {
    std::string upper;
    for (char c : str) upper += static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if (upper == "POST") return HttpMethod::POST;
    if (upper == "PUT") return HttpMethod::PUT;
    if (upper == "DELETE") return HttpMethod::DELETE_;
    if (upper == "PATCH") return HttpMethod::PATCH;
    if (upper == "HEAD") return HttpMethod::HEAD;
    if (upper == "OPTIONS") return HttpMethod::OPTIONS;
    return HttpMethod::GET;
}

enum class AuthType {
    None,
    Bearer,
    Basic,
    ApiKey,
    Inherit
};

inline std::string AuthTypeToString(AuthType a) {
    switch (a) {
    case AuthType::Bearer: return "bearer";
    case AuthType::Basic: return "basic";
    case AuthType::ApiKey: return "apikey";
    case AuthType::Inherit: return "inherit";
    default: return "none";
    }
}

inline AuthType AuthTypeFromString(const std::string& str) {
    std::string lower;
    for (char c : str) lower += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (lower == "bearer") return AuthType::Bearer;
    if (lower == "basic") return AuthType::Basic;
    if (lower == "apikey") return AuthType::ApiKey;
    if (lower == "inherit") return AuthType::Inherit;
    return AuthType::None;
}

struct AuthConfig {
    AuthType type = AuthType::None;
    std::string token;
    std::string username;
    std::string password;
    std::string key;
    std::string value;
    std::string in = "header"; // "header" or "query"
};

enum class BodyType {
    None,
    Json,
    Raw,
    UrlEncoded,
    FormData
};

inline std::string BodyTypeToString(BodyType b) {
    switch (b) {
    case BodyType::Json: return "json";
    case BodyType::Raw: return "raw";
    case BodyType::UrlEncoded: return "x_www_form_urlencoded";
    case BodyType::FormData: return "form_data";
    default: return "none";
    }
}

inline BodyType BodyTypeFromString(const std::string& str) {
    std::string lower;
    for (char c : str) lower += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (lower == "json") return BodyType::Json;
    if (lower == "raw") return BodyType::Raw;
    if (lower == "x_www_form_urlencoded" || lower == "urlencoded") return BodyType::UrlEncoded;
    if (lower == "form_data" || lower == "formdata") return BodyType::FormData;
    return BodyType::None;
}

struct FormItem {
    std::string key;
    std::string value;
    bool enabled = true;
    std::string type = "text"; // "text" or "file"
    std::string filename;
    std::string contentType = "application/octet-stream";
    std::string contentBase64;
};

struct RequestBody {
    BodyType type = BodyType::None;
    std::string content;
    std::vector<FormItem> formItems;
};

struct KeyValuePair {
    std::string key;
    std::string value;
    bool enabled = true;
    std::string description;
};

struct TestAssertion {
    std::string type; // status_code, status_200, status_200_or_201, status_2xx, response_time_ms, time_lt_500, body_contains, header_exists, json_field, json_equals
    std::string expected;
    std::string field;
    std::string header;
    std::string name;
    bool enabled = true;
};

struct TestResult {
    std::string type;
    std::string name;
    bool passed = false;
    std::string expected;
    std::string actual;
    std::string message;
};

struct RequestOptions {
    double timeoutSec = 20.0;
    bool followRedirects = true;
    bool verifyTls = true;
};

struct ApiRequest {
    std::string id;
    std::string name = "Untitled Request";
    HttpMethod method = HttpMethod::GET;
    std::string url;
    std::vector<KeyValuePair> headers;
    std::vector<KeyValuePair> params;
    std::vector<KeyValuePair> pathParams;
    AuthConfig auth;
    RequestBody body;
    std::vector<TestAssertion> tests;
    std::vector<KeyValuePair> variables;
    RequestOptions options;
};

struct ApiResponse {
    int statusCode = 0;
    std::string statusText;
    double latencyMs = 0.0;
    size_t sizeBytes = 0;
    std::vector<KeyValuePair> headers;
    std::string contentType;
    bool isJson = false;
    std::string body;
    std::vector<TestResult> testResults;
    std::string executedAt;
    std::string url;
    std::string method;
    std::string error;
};

struct HistoryEntry {
    std::string id;
    std::string method;
    std::string url;
    int statusCode = 0;
    std::string statusText;
    double latencyMs = 0.0;
    size_t sizeBytes = 0;
    std::string executedAt;
    ApiRequest requestSnapshot;
};

struct Variable {
    std::string key;
    std::string value;
    bool enabled = true;
};

struct Environment {
    std::string id;
    std::string name;
    std::vector<Variable> variables;
};

struct EnvironmentStore {
    std::string activeId;
    std::vector<Environment> environments;
};

struct Folder;

struct Folder {
    std::string id;
    std::string name;
    std::vector<ApiRequest> requests;
    std::vector<Folder> folders;
    std::vector<Variable> variables;
    AuthConfig auth;
};

struct Collection {
    std::string id;
    std::string name;
    std::string description;
    std::vector<ApiRequest> requests;
    std::vector<Folder> folders;
    std::vector<Variable> variables;
    AuthConfig auth;
};

// Database studio types
struct DatabaseInfo {
    std::string id;
    std::string name;
    std::string path;
    bool isSample = false;
    uint64_t sizeBytes = 0;
    int tableCount = 0;
    std::string type = "sqlite";
    std::string dialect = "sqlite";
    bool connected = true;
};

struct ColumnMeta {
    int cid = 0;
    std::string name;
    std::string type = "TEXT";
    bool notnull = false;
    std::string dfltValue;
    bool pk = false;
};

struct ForeignKeyMeta {
    int id = 0;
    int seq = 0;
    std::string targetTable;
    std::string fromColumn;
    std::string toColumn;
    std::string onUpdate;
    std::string onDelete;
};

struct IndexMeta {
    std::string name;
    bool unique = false;
    std::string origin;
    std::vector<std::string> columns;
};

struct TableMeta {
    std::string type = "table"; // "table" or "view"
    std::string name;
    int64_t rowCount = 0;
    std::vector<std::string> primaryKeys;
    std::vector<ColumnMeta> columns;
    std::vector<ForeignKeyMeta> foreignKeys;
    std::vector<IndexMeta> indexes;
    std::string ddl;
    bool editable = true;
};

struct DatabaseSchema {
    std::string databaseId;
    std::string databaseName;
    std::vector<TableMeta> tables;
};

struct ERNode {
    std::string id;
    std::string name;
    std::vector<ColumnMeta> columns;
    int64_t rowCount = 0;
};

struct ERLink {
    std::string id;
    std::string source;
    std::string sourceCol;
    std::string target;
    std::string targetCol;
    std::string onUpdate;
    std::string onDelete;
};

struct ERDiagram {
    std::string databaseId;
    std::vector<ERNode> nodes;
    std::vector<ERLink> links;
};

struct QueryResult {
    std::string query;
    std::vector<std::string> columns;
    std::vector<std::map<std::string, std::string>> rows;
    int64_t rowCount = 0;
    int64_t rowsAffected = 0;
    double executionTimeMs = 0.0;
    std::string error;
    bool truncated = false;
};

struct TableDataResult {
    std::string table;
    std::vector<std::string> columns;
    std::vector<std::string> primaryKeys;
    std::vector<ColumnMeta> columnMetadata;
    int64_t totalRows = 0;
    int page = 1;
    int pageSize = 25;
    int totalPages = 1;
    std::vector<std::map<std::string, std::string>> rows;
    std::string error;
};

} // namespace native_app
