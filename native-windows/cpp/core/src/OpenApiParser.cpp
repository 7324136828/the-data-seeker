#include "OpenApiParser.h"
#include "JsonSerialization.h"
#include "ImportDocument.h"
#include <windows.h>
#include <objbase.h>
#include <nlohmann/json.hpp>
#include <map>
#include <vector>
#include <set>
#include <sstream>
#include <regex>
#include <ctime>
#include <stdexcept>
#pragma comment(lib, "ole32.lib")

namespace native_app {
namespace {
using json = nlohmann::json;
std::string Identifier(const char* prefix) {
    GUID guid{}; if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Cannot allocate imported request identifiers.");
    wchar_t text[40]{}; StringFromGUID2(guid, text, 40);
    std::string id(prefix); for (const wchar_t* p = text; *p; ++p) id += static_cast<char>(*p);
    return id;
}
std::string Text(const json& value) { return value.is_null() ? "" : value.is_string() ? value.get<std::string>() : value.dump(); }
json Resolve(const json& root, json value, std::set<std::string> seen = {}) {
    if (!value.is_object() || !value.contains("$ref")) return value;
    if (!value["$ref"].is_string()) throw std::runtime_error("OpenAPI references must be strings.");
    const auto reference = value["$ref"].get<std::string>();
    if (reference.rfind("#/", 0) != 0) throw std::runtime_error("External OpenAPI references must be inlined before import.");
    if (!seen.insert(reference).second) return json::object();
    json target;
    try { target = root.at(json::json_pointer(reference.substr(1))); }
    catch (...) { throw std::runtime_error("An OpenAPI reference could not be resolved. Check its JSON pointer."); }
    target = Resolve(root, target, std::move(seen));
    if (!target.is_object()) throw std::runtime_error("An OpenAPI reference must point to an object.");
    value.erase("$ref"); target.update(value); return target;
}
json Sample(const json& root, const json& rawSchema, int depth = 0) {
    if (depth > 8 || !rawSchema.is_object()) return nullptr;
    const auto schema = Resolve(root, rawSchema);
    for (const auto* key : {"example", "default", "const"}) if (schema.contains(key)) return schema[key];
    if (schema.contains("enum") && schema["enum"].is_array() && !schema["enum"].empty()) return schema["enum"][0];
    for (const auto* key : {"oneOf", "anyOf"})
        if (schema.contains(key) && schema[key].is_array() && !schema[key].empty()) return Sample(root, schema[key][0], depth + 1);
    if (schema.contains("allOf") && schema["allOf"].is_array()) {
        json result = json::object();
        for (const auto& part : schema["allOf"]) { const auto sample = Sample(root, part, depth + 1); if (sample.is_object()) result.update(sample); }
        return result;
    }
    std::string type = "object";
    if (schema.contains("type")) {
        if (schema["type"].is_string()) type = schema["type"].get<std::string>();
        else if (schema["type"].is_array()) { type = "null"; for (const auto& item : schema["type"]) if (item.is_string() && item != "null") { type = item.get<std::string>(); break; } }
        else throw std::runtime_error("OpenAPI schema type must be a string or array.");
    }
    if (type == "object" || schema.contains("properties")) {
        json result = json::object();
        if (schema.contains("properties")) {
            if (!schema["properties"].is_object()) throw std::runtime_error("OpenAPI schema properties must be an object.");
            for (auto it = schema["properties"].begin(); it != schema["properties"].end(); ++it)
                if (!it.value().is_object() || !it.value().value("readOnly", false)) result[it.key()] = Sample(root, it.value(), depth + 1);
        }
        return result;
    }
    if (type == "array") return json::array({Sample(root, schema.value("items", json::object()), depth + 1)});
    if (type == "string") {
        const auto format = schema.value("format", "");
        if (format == "email") return "user@example.com";
        if (format == "date" || format == "date-time") {
            const auto now = std::time(nullptr); std::tm utc{}; gmtime_s(&utc, &now); char text[40]{};
            std::strftime(text, sizeof(text), format == "date" ? "%Y-%m-%d" : "%Y-%m-%dT%H:%M:%SZ", &utc); return text;
        }
        return "sample_string";
    }
    if (type == "integer" || type == "number") return schema.value("minimum", json(0));
    if (type == "boolean") return true;
    return nullptr;
}
json ParameterSample(const json& root, const json& parameter) {
    if (parameter.contains("example")) return parameter["example"];
    if (parameter.contains("examples") && parameter["examples"].is_object() && !parameter["examples"].empty()) {
        const auto example = Resolve(root, parameter["examples"].begin().value()); return example.value("value", json(""));
    }
    return Sample(root, parameter.value("schema", parameter));
}
std::string Server(const json& servers, const std::string& fallback) {
    if (servers.is_null() || (servers.is_array() && servers.empty())) return fallback;
    if (!servers.is_array() || !servers[0].is_object()) throw std::runtime_error("OpenAPI servers must be an array of objects.");
    std::string result = servers[0].value("url", fallback);
    const auto variables = servers[0].value("variables", json::object());
    if (!variables.is_object()) throw std::runtime_error("OpenAPI server variables must be an object.");
    for (auto it = variables.begin(); it != variables.end(); ++it) {
        const auto placeholder = "{" + it.key() + "}";
        const auto replacement = Text(it.value().value("default", json("")));
        size_t pos = 0; while ((pos = result.find(placeholder, pos)) != std::string::npos) { result.replace(pos, placeholder.size(), replacement); pos += replacement.size(); }
    }
    return result;
}
AuthConfig Authorization(const json& root, const json& requirements, const json& schemes) {
    AuthConfig auth;
    if (requirements.is_null()) return auth;
    if (!requirements.is_array()) throw std::runtime_error("OpenAPI security requirements must be an array.");
    for (const auto& requirement : requirements) {
        if (!requirement.is_object()) throw std::runtime_error("OpenAPI security requirement must be an object.");
        if (requirement.empty()) return auth;
        if (requirement.size() > 1) throw std::runtime_error("Combined OpenAPI security schemes require manual configuration before import.");
        for (auto it = requirement.begin(); it != requirement.end(); ++it) {
            if (!schemes.is_object() || !schemes.contains(it.key())) throw std::runtime_error("An OpenAPI security scheme is missing.");
            const auto scheme = Resolve(root, schemes[it.key()]); const auto type = scheme.value("type", "");
            if (type == "apiKey") {
                auth.type = AuthType::ApiKey; auth.key = scheme.value("name", "X-API-Key"); auth.in = scheme.value("in", "header"); auth.value = "{{api_key}}";
                if (auth.in != "header" && auth.in != "query") throw std::runtime_error("Cookie API-key authentication requires a manually configured Cookie header.");
                return auth;
            }
            if (type == "basic" || (type == "http" && scheme.value("scheme", "") == "basic")) {
                auth.type = AuthType::Basic; auth.username = "{{username}}"; auth.password = "{{password}}"; return auth;
            }
            if (type == "oauth2" || type == "openIdConnect" || (type == "http" && scheme.value("scheme", "") == "bearer")) {
                auth.type = AuthType::Bearer; auth.token = "{{access_token}}"; return auth;
            }
        }
    }
    if (!requirements.empty()) throw std::runtime_error("This OpenAPI authentication scheme requires manual configuration.");
    return auth;
}
std::pair<std::string,std::string> Scripts(const json& object) {
    auto script=[&](const char* key,const char* alternate){
        const auto value=object.value(key,object.value(alternate,json("")));
        if(!value.is_string()||value.get_ref<const std::string&>().size()>65536)throw std::runtime_error("Imported scripts must be text no larger than 64 KiB.");
        return value.get<std::string>();
    };
    std::pair<std::string,std::string> result{script("preRequestScript","x-prerequest-script"),script("postResponseScript","x-test-script")};
    if(object.contains("event")||object.contains("x-postman-event")){
        auto converted=PostmanCollectionToNativeJson({{"info",{{"name","Hooks"}}},{"item",json::array()},{"event",object.value("event",object.value("x-postman-event",json::array()))}});
        auto append=[](std::string& target,const std::string& text){if(!target.empty()&&!text.empty())target+='\n';target+=text;if(target.size()>65536)throw std::runtime_error("Imported scripts exceed 64 KiB.");};
        append(result.first,converted["preRequestScript"]);append(result.second,converted["postResponseScript"]);
    }
    return result;
}

void Body(const json& root, const json& operation, const std::map<std::pair<std::string, std::string>, json>& parameters, ApiRequest& request) {
    const auto definition = Resolve(root, operation.value("requestBody", json::object()));
    auto content = definition.value("content", json::object());
    if (!content.is_object()) throw std::runtime_error("OpenAPI request-body content must be an object.");
    if (content.empty()) {
        for (const auto& parameter : parameters) if (parameter.first.second == "body") {
            std::string media = "application/json";
            const auto consumes = operation.value("consumes", root.value("consumes", json::array()));
            if (consumes.is_array() && !consumes.empty()) media = consumes[0].get<std::string>();
            content[media] = json{{"schema", parameter.second.value("schema", json::object())}}; break;
        }
    }
    if (!content.empty()) {
        auto selected = content.begin();
        for (auto it = content.begin(); it != content.end(); ++it) if (it.key().find("json") != std::string::npos) { selected = it; break; }
        const auto media = selected.key(); const auto definitionMedia = Resolve(root, selected.value());
        json sample;
        if (definitionMedia.contains("example")) sample = definitionMedia["example"];
        else if (definitionMedia.contains("examples") && definitionMedia["examples"].is_object() && !definitionMedia["examples"].empty()) sample = Resolve(root, definitionMedia["examples"].begin().value()).value("value", json());
        else sample = Sample(root, definitionMedia.value("schema", json::object()));
        if (media == "multipart/form-data" || media == "application/x-www-form-urlencoded") {
            request.body.type = media == "multipart/form-data" ? BodyType::FormData : BodyType::UrlEncoded;
            if (sample.is_object()) for (auto it = sample.begin(); it != sample.end(); ++it) { FormItem item; item.key = it.key(); item.value = Text(it.value()); request.body.formItems.push_back(item); }
        } else {
            request.body.type = media.find("json") != std::string::npos ? BodyType::Json : BodyType::Raw;
            request.body.content = request.body.type == BodyType::Json || !sample.is_string() ? sample.dump(2) : sample.get<std::string>();
        }
        if (request.body.type != BodyType::FormData) request.headers.push_back({"Content-Type", media, true, ""});
    } else {
        for (const auto& parameter : parameters) if (parameter.first.second == "formData") {
            if (parameter.second.value("type", "") == "file") throw std::runtime_error("Swagger file uploads require a manually configured multipart file.");
            std::string media = "application/x-www-form-urlencoded";
            const auto consumes = operation.value("consumes", root.value("consumes", json::array()));
            if (consumes.is_array() && !consumes.empty()) media = consumes[0].get<std::string>();
            request.body.type = media == "multipart/form-data" ? BodyType::FormData : BodyType::UrlEncoded;
            FormItem item; item.key = parameter.first.first; item.value = Text(ParameterSample(root, parameter.second)); request.body.formItems.push_back(item);
        }
    }
}
}
Collection OpenApiParser::Parse(const std::string& specJson) {
    if (specJson.empty() || specJson.size() > 16 * 1024 * 1024) throw std::runtime_error("Provide a nonempty API import no larger than 16 MiB.");
    const auto root = ParseImportDocument(specJson);
    if(root.is_object()&&root.contains("info")&&root.contains("item"))return CollectionJson::FromJson(PostmanCollectionToNativeJson(root),true);
    if(root.is_object()&&(root.contains("requests")||root.contains("folders")))return CollectionJson::FromJson(root,true);
    if (!root.is_object()) throw std::runtime_error("Provide an OpenAPI, Swagger, native, or Postman collection.");
    const auto version = root.value("openapi", "");
    const auto swagger = root.value("swagger", "");
    if ((version.rfind("3.", 0) != 0 && swagger != "2.0") || !root.contains("paths") || !root["paths"].is_object())
        throw std::runtime_error("Provide an OpenAPI 3, Swagger 2, native, or Postman v2 collection in JSON or YAML.");
    const auto rootScripts=Scripts(root);
    Collection collection; const auto info = root.value("info", json::object());
    collection.name = info.value("title", "Imported API"); collection.description = info.value("description", ""); collection.id = Identifier("col_");
    collection.preRequestScript=rootScripts.first;collection.postResponseScript=rootScripts.second;
    std::string base = "{{base_url}}";
    if (root.contains("servers")) base = Server(root["servers"], base);
    else if (root.contains("host")) {
        std::string scheme = "https"; const auto schemes = root.value("schemes", json::array());
        if (schemes.is_array() && !schemes.empty()) scheme = schemes[0].get<std::string>();
        base = scheme + "://" + root["host"].get<std::string>() + root.value("basePath", "");
    }
    const auto securitySchemes = root.value("components", json::object()).value("securitySchemes", root.value("securityDefinitions", json::object()));
    std::map<std::string, std::vector<ApiRequest>> grouped;
    static const std::vector<std::string> methods = {"get", "post", "put", "delete", "patch", "head", "options"};
    for (auto path = root["paths"].begin(); path != root["paths"].end(); ++path) {
        const auto pathItem = Resolve(root, path.value()); if (!pathItem.is_object()) throw std::runtime_error("OpenAPI path items must be objects.");
        const auto pathScripts=Scripts(pathItem);
        const auto pathServer = Server(pathItem.value("servers", json()), base);
        for (const auto& method : methods) {
            if (!pathItem.contains(method)) continue;
            const auto operation = Resolve(root, pathItem[method]); if (!operation.is_object()) throw std::runtime_error("OpenAPI operations must be objects.");
            const auto operationScripts=Scripts(operation);
            ApiRequest request; request.id = Identifier("req_"); request.method = HttpMethodFromString(method);
            request.name = operation.value("summary", operation.value("operationId", method + " " + path.key()));
            request.description=operation.value("description","");
            request.preRequestScript=pathScripts.first+(pathScripts.first.empty()||operationScripts.first.empty()?"":"\n")+operationScripts.first;
            request.postResponseScript=pathScripts.second+(pathScripts.second.empty()||operationScripts.second.empty()?"":"\n")+operationScripts.second;
            if(request.preRequestScript.size()>65536||request.postResponseScript.size()>65536)throw std::runtime_error("Imported inherited scripts exceed 64 KiB.");
            std::string url = Server(operation.value("servers", json()), pathServer);
            while (!url.empty() && url.back() == '/') url.pop_back();
            std::string requestPath = path.key(); while (!requestPath.empty() && requestPath.front() == '/') requestPath.erase(requestPath.begin());
            request.url = url + "/" + requestPath;
            std::map<std::pair<std::string, std::string>, json> parameters;
            for (const auto& source : {pathItem.value("parameters", json::array()), operation.value("parameters", json::array())}) {
                if (!source.is_array()) throw std::runtime_error("OpenAPI parameters must be an array.");
                for (const auto& raw : source) {
                    const auto parameter = Resolve(root, raw);
                    if (!parameter.is_object()) throw std::runtime_error("OpenAPI parameters must be objects.");
                    const auto name = parameter.value("name", ""), in = parameter.value("in", "");
                    if (name.empty() || in.empty()) throw std::runtime_error("OpenAPI parameters require name and location.");
                    parameters[{name, in}] = parameter;
                }
            }
            for (const auto& entry : parameters) {
                const auto& parameter = entry.second; const auto& name = entry.first.first; const auto& in = entry.first.second;
                const auto schema = Resolve(root, parameter.value("schema", parameter));
                const bool enabled = parameter.value("required", false) || parameter.contains("example") || parameter.contains("examples") || schema.contains("default") || schema.contains("example") || schema.contains("enum");
                const KeyValuePair pair{name, Text(ParameterSample(root, parameter)), enabled, parameter.value("description", "")};
                if (in == "query") request.params.push_back(pair);
                else if (in == "header") request.headers.push_back(pair);
                else if (in == "path") { auto pathPair = pair; pathPair.enabled = true; request.pathParams.push_back(pathPair); }
                else if (in != "body" && in != "formData") throw std::runtime_error("Unsupported OpenAPI parameter location. Configure it manually before import.");
            }
            request.auth = Authorization(root, operation.value("security", root.value("security", json::array())), securitySchemes);
            Body(root, operation, parameters, request);
            std::vector<std::string> successes;
            const auto responses = operation.value("responses", json::object());
            if (!responses.is_object()) throw std::runtime_error("OpenAPI responses must be an object.");
            for (auto response = responses.begin(); response != responses.end(); ++response) {
                Resolve(root, response.value()); // Reject unresolved response references too.
                const auto& code = response.key();
                if (code.size() == 3 && code[0] == '2' && isdigit(static_cast<unsigned char>(code[1])) && isdigit(static_cast<unsigned char>(code[2]))) successes.push_back(code);
            }
            if (successes.size() == 1) request.tests.push_back({"status_code", successes[0], "", "", "Status code is " + successes[0], true});
            else request.tests.push_back({"status_2xx", "", "", "", "Status is 2xx", true});
            std::string tag = "Default"; const auto tags = operation.value("tags", json::array());
            if (tags.is_array() && !tags.empty()) tag = tags[0].get<std::string>();
            if (tag == "Default") collection.requests.push_back(request); else grouped[tag].push_back(request);
        }
    }
    for (auto& entry : grouped) { Folder folder; folder.id = Identifier("fld_"); folder.name = entry.first; folder.requests = std::move(entry.second); collection.folders.push_back(std::move(folder)); }
    return collection;
}
} // namespace native_app