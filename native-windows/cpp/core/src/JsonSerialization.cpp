#include "JsonSerialization.h"
#include "ImportDocument.h"
#include "OpenApiParser.h"
#include <windows.h>
#include <objbase.h>
#include <stdexcept>
#include <algorithm>
#include <set>
#include <functional>
namespace native_app {
namespace {
using json=nlohmann::json;
static std::string Identity(std::string value){return value;}
static json VariableEntries(const json& value) {
 if(value.size()>10000||(!value.is_object()&&!value.is_array()))throw std::runtime_error("Variables must be an object or a bounded array.");
 json entries=json::array();std::set<std::string> enabledNames;
 if(value.is_object()){for(auto item=value.begin();item!=value.end();++item)entries.push_back({{"key",item.key()},{"value",item.value()},{"enabled",true}});return entries;}
 for(const auto& entry:value){if(!entry.is_object())throw std::runtime_error("Each variable must be an object.");auto normalized=entry;auto key=entry.value("key",std::string{});const auto begin=key.find_first_not_of(" \t\r\n"),end=key.find_last_not_of(" \t\r\n");if(begin==std::string::npos)continue;key=key.substr(begin,end-begin+1);normalized["key"]=key;if(entry.value("enabled",true)&&!enabledNames.insert(key).second)throw std::runtime_error("Enabled variable names must be unique within a scope.");entries.push_back(std::move(normalized));}return entries;
}
static json SerializeVariable(const Variable& v) {
    return json{{"key", v.key}, {"value", v.scriptJsonValue.empty() ? json(v.value) : json::parse(v.scriptJsonValue)}, {"enabled", v.enabled}};
}

static Variable DeserializeVariable(const json& j) {
    Variable v;
    v.key = j.value("key", "");
    const auto raw = j.value("value", json(""));
    v.value = raw.is_string() ? raw.get<std::string>() : raw.dump();
    v.scriptJsonValue = raw.is_string() ? std::string{} : raw.dump();
    v.enabled = j.value("enabled", true);
    return v;
}

static json SerializeKeyValuePair(const KeyValuePair& kv) {
    return json{{"key", kv.key}, {"value", kv.scriptJsonValue.empty() ? json(kv.value) : json::parse(kv.scriptJsonValue)}, {"enabled", kv.enabled}, {"description", kv.description}};
}

static KeyValuePair DeserializeKeyValuePair(const json& j) {
    KeyValuePair kv;
    kv.key = j.value("key", "");
    const auto raw = j.value("value", json(""));
    kv.value = raw.is_string() ? raw.get<std::string>() : raw.dump();
    kv.scriptJsonValue = raw.is_string() ? std::string{} : raw.dump();
    kv.enabled = j.value("enabled", true);
    kv.description = j.value("description", "");
    return kv;
}

static json SerializeRequest(const ApiRequest& r) {
    json j;
    j["preRequestScript"] = Identity(r.preRequestScript);
    j["postResponseScript"] = Identity(r.postResponseScript);
    j["id"] = r.id;
    j["name"] = r.name;
    j["description"] = r.description;
    j["iterationData"] = r.iterationDataJson.empty() ? json::object() : json::parse(r.iterationDataJson);
    if(r.hasIteration)j["iteration"]=r.iteration;
    j["method"] = HttpMethodToString(r.method);
    j["url"] = Identity(r.url);

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
        {"token", Identity(r.auth.token)},
        {"username", Identity(r.auth.username)},
        {"password", Identity(r.auth.password)},
        {"key", r.auth.key},
        {"value", Identity(r.auth.value)},
        {"in", r.auth.in}
    };

    json fItems = json::array();
    for (const auto& item : r.body.formItems) {
        fItems.push_back(json{
            {"key", item.key}, {"value", Identity(item.value)}, {"enabled", item.enabled},
            {"type", item.type}, {"filename", item.filename}, {"contentType", item.contentType},
            {"content", Identity(item.contentBase64)}
        });
    }
    j["body"] = json{
        {"type", BodyTypeToString(r.body.type)},
        {"content", Identity(r.body.content)},
        {"formItems", fItems}
    };

    json tests = json::array();
    for (const auto& t : r.tests) {
        tests.push_back(json{
            {"type", t.type}, {"expected", Identity(t.expected)}, {"field", Identity(t.field)},
            {"header", Identity(t.header)}, {"name", t.name}, {"enabled", t.enabled}
        });
    }
    j["tests"] = tests;
    j["variables"] = json::array();
    for (const auto& v : r.variables) j["variables"].push_back(SerializeKeyValuePair(v));
    j["options"] = {{"timeout", r.options.timeoutSec}, {"follow_redirects", r.options.followRedirects}, {"verify_tls", r.options.verifyTls}};

    return j;
}

static ApiRequest DeserializeRequest(const json& j) {
    ApiRequest r;
    r.preRequestScript = Identity(j.value("preRequestScript", ""));
    r.postResponseScript = Identity(j.value("postResponseScript", ""));
    r.id = j.value("id", "");
    r.name = j.value("name", "Untitled Request");
    r.description = j.value("description", "");
    if(j.contains("iterationData")){r.iterationDataJson=j["iterationData"].dump();(void)RequestIterationData(r);}
    if(j.contains("iteration")){if(!j["iteration"].is_number_integer())throw std::runtime_error("Request iteration must be an integer.");r.iteration=j["iteration"].get<int>();r.hasIteration=true;}
    r.method = HttpMethodFromString(j.value("method", "GET"));
    r.url = Identity(j.value("url", ""));

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
        r.auth.token = Identity(a.value("token", ""));
        r.auth.username = Identity(a.value("username", ""));
        r.auth.password = Identity(a.value("password", ""));
        r.auth.key = a.value("key", "");
        r.auth.value = Identity(a.value("value", ""));
        r.auth.in = a.value("in", "header");
    }
    if(j.contains("body")&&j["body"].is_string()){r.body.type=BodyType::Raw;r.body.content=j["body"].get<std::string>();}
    if (j.contains("body") && j["body"].is_object()) {
        const json& b = j["body"];
        r.body.type = BodyTypeFromString(b.value("type", "none"));
        r.body.content = Identity(b.value("content", ""));
        if (b.contains("formItems") && b["formItems"].is_array()) {
            for (const auto& f : b["formItems"]) {
                FormItem item;
                item.key = f.value("key", "");
                item.value = Identity(f.value("value", ""));
                item.enabled = f.value("enabled", true);
                item.type = f.value("type", "text");
                item.filename = f.value("filename", "");
                item.contentType = f.value("contentType", "application/octet-stream");
                item.contentBase64 = Identity(f.value("content", f.value("contentBase64", "")));
                r.body.formItems.push_back(item);
            }
        }
    }
    if (j.contains("tests") && j["tests"].is_array()) {
        for (const auto& t : j["tests"]) {
            TestAssertion a;
            a.type = t.value("type", "");
            a.expected = Identity(t.value("expected", ""));
            a.field = Identity(t.value("field", ""));
            a.header = Identity(t.value("header", ""));
            a.name = t.value("name", "");
            a.enabled = t.value("enabled", true);
            r.tests.push_back(a);
        }
    }

    if (j.contains("variables"))
        for (const auto& v : VariableEntries(j["variables"])) r.variables.push_back(DeserializeKeyValuePair(v));
    if (j.contains("options") && j["options"].is_object()) {
        r.options.timeoutSec = j["options"].value("timeout", j["options"].value("timeoutSec", 20.0));
        r.options.followRedirects = j["options"].value("follow_redirects", j["options"].value("followRedirects", true));
        r.options.verifyTls = j["options"].value("verify_tls", j["options"].value("verifyTls", true));
    }
    return r;
}

static json SerializeFolder(const Folder& f) {
    json j;
    j["preRequestScript"] = Identity(f.preRequestScript);
    j["postResponseScript"] = Identity(f.postResponseScript);
    j["id"] = f.id;
    j["name"] = f.name;
    j["description"] = f.description;
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
    f.preRequestScript = Identity(j.value("preRequestScript", ""));
    f.postResponseScript = Identity(j.value("postResponseScript", ""));
    f.id = j.value("id", "");
    f.name = j.value("name", "");
    f.description = j.value("description", "");
    if (j.contains("requests") && j["requests"].is_array()) {
        for (const auto& r : j["requests"]) f.requests.push_back(DeserializeRequest(r));
    }
    if (j.contains("folders") && j["folders"].is_array()) {
        for (const auto& sub : j["folders"]) f.folders.push_back(DeserializeFolder(sub));
    }
    if (j.contains("auth")) f.auth = DeserializeRequest(json{{"auth", j["auth"]}}).auth;
    if (j.contains("variables"))
        for (const auto& v : VariableEntries(j["variables"])) f.variables.push_back(DeserializeVariable(v));
    return f;
}

static json SerializeCollection(const Collection& c) {
    json j;
    j["preRequestScript"] = Identity(c.preRequestScript);
    j["postResponseScript"] = Identity(c.postResponseScript);
    j["id"] = c.id;
    j["name"] = c.name;
    j["description"] = c.description;

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
    c.preRequestScript = Identity(j.value("preRequestScript", ""));
    c.postResponseScript = Identity(j.value("postResponseScript", ""));
    c.id = j.value("id", "");
    c.name = j.value("name", "");
    c.description = j.value("description", "");
    if (j.contains("requests") && j["requests"].is_array()) {
        for (const auto& r : j["requests"]) c.requests.push_back(DeserializeRequest(r));
    }
    if (j.contains("folders") && j["folders"].is_array()) {
        for (const auto& f : j["folders"]) c.folders.push_back(DeserializeFolder(f));
    }
    if (j.contains("auth")) c.auth = DeserializeRequest(json{{"auth", j["auth"]}}).auth;
    if (j.contains("variables"))
        for (const auto& v : VariableEntries(j["variables"])) c.variables.push_back(DeserializeVariable(v));
    return c;
}


static std::string NewId(const char* prefix){
 GUID id{};if(FAILED(CoCreateGuid(&id)))throw std::runtime_error("Cannot allocate an import identifier.");
 wchar_t text[40]{};StringFromGUID2(id,text,40);std::string result(prefix);for(const wchar_t c:text){if(!c)break;result+=static_cast<char>(c);}return result;
}
static void CheckObject(const json& value,const char* description){if(!value.is_object())throw std::runtime_error(std::string(description)+" must be an object.");}
static void CheckArray(const json& value,const char* key,size_t maximum=10000){if(value.contains(key)&&(!value[key].is_array()||value[key].size()>maximum))throw std::runtime_error(std::string(key)+" must be a bounded array.");}
static void ValidateRequest(const json& value){
 CheckObject(value,"Request");for(const auto* key:{"headers","params","pathParams","tests"})CheckArray(value,key);if(value.contains("variables"))(void)VariableEntries(value["variables"]);
 if(value.contains("body")){if(value["body"].is_object())CheckArray(value["body"],"formItems");else if(!value["body"].is_string())throw std::runtime_error("Request body must be an object or text.");}
 for(const auto* key:{"preRequestScript","postResponseScript"})if(value.contains(key)&&(!value[key].is_string()||value[key].get_ref<const std::string&>().size()>65536))throw std::runtime_error("Request scripts are limited to 64 KiB.");
 if(value.contains("method")){const auto m=value["method"].get<std::string>();const auto normalized=HttpMethodToString(HttpMethodFromString(m));std::string upper=m;std::transform(upper.begin(),upper.end(),upper.begin(),[](unsigned char c){return static_cast<char>(toupper(c));});if(normalized!=upper)throw std::runtime_error("Unsupported HTTP request method.");}
}
static void ValidateNode(const json& value,unsigned depth,size_t& entries){
 if(depth>32||++entries>10000)throw std::runtime_error("Collection exceeds its nesting or item limit.");CheckObject(value,"Collection or folder");CheckArray(value,"requests");CheckArray(value,"folders");if(value.contains("variables"))(void)VariableEntries(value["variables"]);
 for(const auto* key:{"preRequestScript","postResponseScript"})if(value.contains(key)&&(!value[key].is_string()||value[key].get_ref<const std::string&>().size()>65536))throw std::runtime_error("Inherited scripts are limited to 64 KiB.");
 if(value.contains("requests"))for(const auto& r:value["requests"]){if(++entries>10000)throw std::runtime_error("Collection exceeds its item limit.");ValidateRequest(r);}
 if(value.contains("folders"))for(const auto& f:value["folders"])ValidateNode(f,depth+1,entries);
}
static void Renew(Folder& f){f.id=NewId("fld_");for(auto& r:f.requests)r.id=NewId("req_");for(auto& child:f.folders)Renew(child);}
static void NormalizeIds(Collection& collection) {
 std::set<std::string> ids;
 auto retain=[&](const std::string& id){if(!id.empty()&&!ids.insert(id).second)throw std::runtime_error("Collection contains duplicate item identifiers.");};
 retain(collection.id);
 for(const auto& r:collection.requests)retain(r.id);
 std::function<void(const Folder&)> inspect=[&](const Folder& f){retain(f.id);for(const auto& r:f.requests)retain(r.id);for(const auto& child:f.folders)inspect(child);};
 for(const auto& f:collection.folders)inspect(f);
 auto fill=[&](std::string& id,const char* prefix){if(id.empty()){do{id=NewId(prefix);}while(!ids.insert(id).second);}};
 fill(collection.id,"col_");for(auto& r:collection.requests)fill(r.id,"req_");
 std::function<void(Folder&)> repair=[&](Folder& f){fill(f.id,"fld_");for(auto& r:f.requests)fill(r.id,"req_");for(auto& child:f.folders)repair(child);};
 for(auto& f:collection.folders)repair(f);
}
}
namespace RequestJson {
json ToJson(const ApiRequest& request){return SerializeRequest(request);}
ApiRequest FromJson(const json& value,bool regenerateId){ValidateDocumentStructure(value);ValidateRequest(value);auto normalized=value;if(value.contains("body")&&value["body"].is_string())normalized["body"]={{"type","raw"},{"content",value["body"]}};auto r=DeserializeRequest(normalized);if(regenerateId||r.id.empty())r.id=NewId("req_");return r;}
}
namespace CollectionJson {
json ToJson(const Collection& collection){return SerializeCollection(collection);}
Collection FromJson(const json& value,bool regenerateIds){ValidateDocumentStructure(value);size_t entries=0;ValidateNode(value,0,entries);auto c=DeserializeCollection(value);if(regenerateIds){c.id=NewId("col_");for(auto& r:c.requests)r.id=NewId("req_");for(auto& f:c.folders)Renew(f);}NormalizeIds(c);return c;}
std::vector<Collection> Import(const std::string& text,bool regenerateIds){if(text.empty()||text.size()>16*1024*1024)throw std::runtime_error("Collection imports must contain 1 byte to 16 MiB.");const auto value=ParseImportDocument(text);std::vector<Collection> result;auto add=[&](const json& list){if(!list.is_array()||list.size()>1000)throw std::runtime_error("Collections must be a bounded array.");for(const auto& c:list){if(c.is_object()&&(c.contains("item")||c.contains("openapi")||c.contains("swagger")))result.push_back(OpenApiParser::Parse(c.dump()));else result.push_back(FromJson(c,regenerateIds));}};if(value.is_array())add(value);else if(value.is_object()&&value.contains("collections"))add(value["collections"]);else if(value.is_object()&&(value.contains("item")||value.contains("openapi")||value.contains("swagger")))result.push_back(OpenApiParser::Parse(text));else result.push_back(FromJson(value,regenerateIds));return result;}
}
}
