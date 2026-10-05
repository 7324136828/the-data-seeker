#include "OpenApiParser.h"
#include <windows.h>
#include <sstream>
#include <nlohmann/json.hpp>
#include <map>
#include <vector>

namespace native_app {

using json = nlohmann::json;

static json ResolveRef(const json& root, const std::string& ref) {
    if (ref.rfind("#/", 0) != 0) return json();
    std::string path = ref.substr(2);
    std::stringstream ss(path);
    std::string seg;
    json curr = root;
    while (std::getline(ss, seg, '/')) {
        if (curr.contains(seg)) {
            curr = curr[seg];
        } else {
            return json();
        }
    }
    return curr;
}

static json GenerateSample(const json& root, const json& schema, int depth = 0) {
    if (depth > 6 || schema.is_null()) return json();
    if (schema.contains("$ref")) {
        std::string ref = schema["$ref"].get<std::string>();
        return GenerateSample(root, ResolveRef(root, ref), depth + 1);
    }
    if (schema.contains("example")) return schema["example"];
    if (schema.contains("default")) return schema["default"];

    std::string type = schema.value("type", "object");
    if (type == "string") {
        std::string fmt = schema.value("format", "");
        if (fmt == "date-time") return "2026-10-04T12:00:00Z";
        if (fmt == "email") return "user@example.com";
        return "sample_string";
    } else if (type == "integer" || type == "number") {
        return schema.value("minimum", 0);
    } else if (type == "boolean") {
        return true;
    } else if (type == "array") {
        json arr = json::array();
        if (schema.contains("items")) {
            arr.push_back(GenerateSample(root, schema["items"], depth + 1));
        }
        return arr;
    } else if (type == "object" || schema.contains("properties")) {
        json obj = json::object();
        if (schema.contains("properties")) {
            for (auto it = schema["properties"].begin(); it != schema["properties"].end(); ++it) {
                obj[it.key()] = GenerateSample(root, it.value(), depth + 1);
            }
        }
        return obj;
    }
    return json();
}

Collection OpenApiParser::Parse(const std::string& specJson) {
    json root = json::parse(specJson);
    Collection col;

    json info = root.value("info", json::object());
    col.name = info.value("title", "Imported OpenAPI Collection");
    col.description = info.value("description", "");
    col.id = "col_" + std::to_string(GetTickCount64());

    std::string baseUrl = "{{base_url}}";
    if (root.contains("servers") && root["servers"].is_array() && !root["servers"].empty()) {
        baseUrl = root["servers"][0].value("url", "{{base_url}}");
    } else if (root.contains("host")) {
        std::string scheme = "https";
        if (root.contains("schemes") && root["schemes"].is_array() && !root["schemes"].empty()) {
            scheme = root["schemes"][0].get<std::string>();
        }
        baseUrl = scheme + "://" + root["host"].get<std::string>() + root.value("basePath", "");
    }

    std::map<std::string, std::vector<ApiRequest>> taggedRequests;
    std::vector<ApiRequest> untaggedRequests;

    if (root.contains("paths") && root["paths"].is_object()) {
        for (auto pIt = root["paths"].begin(); pIt != root["paths"].end(); ++pIt) {
            std::string path = pIt.key();
            const json& pathItem = pIt.value();

            static const std::vector<std::string> methods = {"get", "post", "put", "delete", "patch", "head", "options"};
            for (const auto& m : methods) {
                if (!pathItem.contains(m)) continue;
                const json& op = pathItem[m];

                ApiRequest req;
                req.id = "req_" + std::to_string(GetTickCount64() + rand());
                req.method = HttpMethodFromString(m);
                req.name = op.value("summary", op.value("operationId", m + " " + path));
                req.url = baseUrl + path;

                // Parameters
                if (op.contains("parameters") && op["parameters"].is_array()) {
                    for (const auto& param : op["parameters"]) {
                        json p = param;
                        if (p.contains("$ref")) {
                            p = ResolveRef(root, p["$ref"].get<std::string>());
                        }
                        std::string pIn = p.value("in", "");
                        std::string pName = p.value("name", "");
                        std::string pDesc = p.value("description", "");
                        bool reqd = p.value("required", false);

                        if (pIn == "query") {
                            req.params.push_back({pName, "", reqd, pDesc});
                        } else if (pIn == "path") {
                            req.pathParams.push_back({pName, "", true, pDesc});
                        } else if (pIn == "header") {
                            req.headers.push_back({pName, "", reqd, pDesc});
                        }
                    }
                }

                // Request body (OpenAPI 3)
                if (op.contains("requestBody")) {
                    json rb = op["requestBody"];
                    if (rb.contains("$ref")) rb = ResolveRef(root, rb["$ref"].get<std::string>());
                    if (rb.contains("content")) {
                        const json& content = rb["content"];
                        if (content.contains("application/json")) {
                            const json& jsonContent = content["application/json"];
                            if (jsonContent.contains("schema")) {
                                json sample = GenerateSample(root, jsonContent["schema"]);
                                req.body.type = BodyType::Json;
                                req.body.content = sample.dump(2);
                                req.headers.push_back({"Content-Type", "application/json", true, ""});
                            }
                        }
                    }
                }

                req.tests.push_back({"status_code", "200", "", "", "Status code is 200", true});

                // Tag grouping
                std::string tag = "Default";
                if (op.contains("tags") && op["tags"].is_array() && !op["tags"].empty()) {
                    tag = op["tags"][0].get<std::string>();
                    taggedRequests[tag].push_back(req);
                } else {
                    untaggedRequests.push_back(req);
                }
            }
        }
    }

    col.requests = untaggedRequests;
    for (const auto& pair : taggedRequests) {
        Folder f;
        f.id = "fld_" + std::to_string(GetTickCount64() + rand());
        f.name = pair.first;
        f.requests = pair.second;
        col.folders.push_back(f);
    }

    return col;
}

} // namespace native_app
