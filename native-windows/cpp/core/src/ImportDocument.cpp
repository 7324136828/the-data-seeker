#include "ImportDocument.h"
#include "Types.h"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>
#include <set>
#include <stdexcept>
#include <vector>
namespace native_app {
namespace {
using json = nlohmann::json;
constexpr size_t kMaximumBytes = 16 * 1024 * 1024;
struct YamlBudget { size_t nodes = 0, bytes = 0; std::vector<YAML::Node> ancestors; };
json ConvertYaml(const YAML::Node& node, YamlBudget& budget, unsigned depth) {
    if (depth > 64 || ++budget.nodes > 100000) throw std::runtime_error("YAML exceeds its nesting or expanded node limit.");
    for (const auto& ancestor : budget.ancestors) if (node.is(ancestor)) throw std::runtime_error("Recursive YAML aliases cannot be imported.");
    const auto tag = node.Tag();
    static const std::set<std::string> allowedTags={"?","!","","tag:yaml.org,2002:str","tag:yaml.org,2002:int","tag:yaml.org,2002:float","tag:yaml.org,2002:bool","tag:yaml.org,2002:null","tag:yaml.org,2002:timestamp","tag:yaml.org,2002:seq","tag:yaml.org,2002:map"};
    if (!allowedTags.count(tag))
        throw std::runtime_error("Custom YAML object tags cannot be imported.");
    if (node.IsNull()) return nullptr;
    if (node.IsScalar()) {
        const auto value = node.Scalar(); budget.bytes += value.size();
        if (budget.bytes > kMaximumBytes) throw std::runtime_error("Expanded YAML text exceeds 16 MiB.");
        if (tag == "!" || tag == "tag:yaml.org,2002:str" || tag == "tag:yaml.org,2002:timestamp") return value;
        if (tag != "?" && !tag.empty() && tag != "tag:yaml.org,2002:int" && tag != "tag:yaml.org,2002:float" && tag != "tag:yaml.org,2002:bool" && tag != "tag:yaml.org,2002:null")
            throw std::runtime_error("This YAML scalar tag is not JSON-compatible.");
        std::string lower=value; std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        if (lower == "null" || lower == "~") return nullptr;
        if (lower == "true" || lower == "yes" || lower == "on") return true;
        if (lower == "false" || lower == "no" || lower == "off") return false;
        static const std::regex number(R"(^[-+]?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?$)");
        if (std::regex_match(value, number)) {
            auto normalized=value; if (!normalized.empty() && normalized[0]=='+') normalized.erase(normalized.begin());
            try { return json::parse(normalized); } catch (const json::exception&) { throw std::runtime_error("YAML numeric value is outside the supported range."); }
        }
        if (tag == "tag:yaml.org,2002:int" || tag == "tag:yaml.org,2002:float" || tag == "tag:yaml.org,2002:bool" || tag == "tag:yaml.org,2002:null")
            throw std::runtime_error("Explicit YAML scalar has an invalid JSON-compatible value.");
        return value;
    }
    budget.ancestors.push_back(node);
    struct Pop { std::vector<YAML::Node>& items; ~Pop(){items.pop_back();} } pop{budget.ancestors};
    if (node.IsSequence()) { json result=json::array(); for (const auto& child:node) result.push_back(ConvertYaml(child,budget,depth+1)); return result; }
    if (!node.IsMap()) throw std::runtime_error("Unsupported YAML node.");
    json result=json::object(); std::vector<std::pair<std::string,YAML::Node>> explicitEntries;
    for (const auto& item:node) {
        if (!item.first.IsScalar()) throw std::runtime_error("YAML mapping keys must be scalar text.");
        const auto key=item.first.Scalar(); budget.bytes+=key.size(); if(budget.bytes>kMaximumBytes) throw std::runtime_error("Expanded YAML text exceeds 16 MiB.");
        if(key=="<<") {
            const auto merged=ConvertYaml(item.second,budget,depth+1);
            auto merge=[&](const json& value){if(!value.is_object())throw std::runtime_error("YAML merge aliases must contain mappings.");for(auto it=value.begin();it!=value.end();++it)if(!result.contains(it.key()))result[it.key()]=it.value();};
            if(merged.is_array())for(const auto& value:merged)merge(value);else merge(merged);
        } else {
            if(std::any_of(explicitEntries.begin(),explicitEntries.end(),[&](const auto& value){return value.first==key;}))throw std::runtime_error("Duplicate YAML mapping keys cannot be imported.");
            explicitEntries.emplace_back(key,item.second);
        }
    }
    for(const auto& value:explicitEntries)result[value.first]=ConvertYaml(value.second,budget,depth+1);
    return result;
}
std::string Text(const json& value){return value.is_null()?"":value.is_string()?value.get<std::string>():value.dump();}
std::string Description(const json& value){if(value.is_string())return value.get<std::string>();if(value.is_object()&&value.contains("content")&&value["content"].is_string())return value["content"].get<std::string>();return {};}
void Array(const json& value,const char* key){if(value.contains(key)&&(!value[key].is_array()||value[key].size()>10000))throw std::runtime_error(std::string("Postman ")+key+" must be a bounded array.");}
json Variables(const json& node){
    Array(node,"variable");json result=json::array();
    for(const auto& item:node.value("variable",json::array())){if(!item.is_object())throw std::runtime_error("Postman variables must be objects.");auto key=Text(item.value("key",item.value("id",json(""))));if(!key.empty())result.push_back({{"key",key},{"value",item.value("value",json(""))},{"enabled",!item.value("disabled",false)}});}
    return result;
}
std::string Event(const json& node,const char* listen){
    Array(node,"event");
    for(const auto& entry:node.value("event",json::array())){
        if(!entry.is_object())throw std::runtime_error("Postman events must be objects.");if(entry.value("listen","")!=listen)continue;
        if(entry.value("disabled",false))return {};
        const auto script=entry.value("script",json::object());if(!script.is_object())throw std::runtime_error("Postman event script must be an object.");
        const auto type=script.value("type","text/javascript");if(type!="text/javascript"&&type!="application/javascript")throw std::runtime_error("Only JavaScript Postman events can be imported.");
        const auto exec=script.value("exec",json(""));std::string result;
        if(exec.is_string())result=exec.get<std::string>();
        else if(exec.is_array()){for(const auto& line:exec){if(!line.is_string())throw std::runtime_error("Postman script lines must be text.");if(!result.empty())result+='\n';result+=line.get<std::string>();if(result.size()>65536)throw std::runtime_error("Imported scripts are limited to 64 KiB.");}}
        else throw std::runtime_error("Postman script must contain text or text lines.");
        if(result.size()>65536)throw std::runtime_error("Imported scripts are limited to 64 KiB.");return result;
    }
    return {};
}
json Auth(const json& value){
    if(value.is_null())return {{"type","inherit"}};if(!value.is_object())throw std::runtime_error("Postman auth must be an object.");
    auto type=value.value("type","inherit");if(type=="noauth")return {{"type","none"}};if(type=="inherit")return {{"type","inherit"}};
    if(type!="basic"&&type!="bearer"&&type!="apikey")throw std::runtime_error("This Postman authentication type is unsupported; configure it explicitly before import.");
    Array(value,type.c_str());json result={{"type",type}};
    for(const auto& entry:value.value(type,json::array())){if(!entry.is_object())throw std::runtime_error("Postman auth entries must be objects.");const auto key=entry.value("key","");if(!key.empty())result[key]=Text(entry.value("value",json("")));}
    if(type=="apikey")result["in"]=result.value("in","")=="query"?"query":"header";return result;
}
void Common(const json& node,json& result){
    result["description"]=Description(node.value("description",json("")));
    result["variables"]=Variables(node);result["auth"]=Auth(node.value("auth",json()));
    result["preRequestScript"]=Event(node,"prerequest");result["postResponseScript"]=Event(node,"test");
}
json Request(const json& item){
    auto source=item.value("request",json::object());if(source.is_string())source={{"url",source}};if(!source.is_object())throw std::runtime_error("Postman request must be an object or URL.");
    json result={{"name",item.value("name","Request")},{"method",source.value("method","GET")}};Common(item,result);
    if(!item.contains("description"))result["description"]=Description(source.value("description",json("")));
    result["auth"]=Auth(source.value("auth",json()));
    auto url=source.value("url",json(""));result["url"]=url.is_string()?url.get<std::string>():url.is_object()?url.value("raw",""):"";
    if(!url.is_string()&&!url.is_object())throw std::runtime_error("Postman URL must be text or an object.");
    if(url.is_object()&&result["url"].get_ref<const std::string&>().empty()){
        auto joined=[](const json& value,const char* separator){if(value.is_string())return value.get<std::string>();if(!value.is_array())throw std::runtime_error("Postman URL host/path must be text or an array.");std::string text;for(const auto& part:value){if(!text.empty())text+=separator;text+=Text(part);}return text;};
        result["url"]=url.value("protocol","https")+"://"+joined(url.value("host",json("")),".")+(url.contains("port")?":"+Text(url["port"]):"")+"/"+joined(url.value("path",json("")),"/");
    }
    auto pairs=[](const json& node,const char* key){Array(node,key);json result=json::array();for(const auto& entry:node.value(key,json::array())){if(!entry.is_object())throw std::runtime_error("Postman request fields must be objects.");result.push_back({{"key",Text(entry.value("key",json("")))},{"value",Text(entry.value("value",json("")))},{"enabled",!entry.value("disabled",false)},{"description",Description(entry.value("description",json("")))}});}return result;};
    result["headers"]=pairs(source,"header");result["params"]=url.is_object()?pairs(url,"query"):json::array();result["pathParams"]=url.is_object()?pairs(url,"variable"):json::array();
    const auto body=source.value("body",json::object());if(!body.is_object())throw std::runtime_error("Postman body must be an object.");
    const auto mode=body.value("mode","none");json converted={{"type","none"},{"content",""},{"formItems",json::array()}};
    if(mode=="raw") {const auto content=Text(body.value("raw",json("")));auto parsed=json::parse(content,nullptr,false);converted["type"]=parsed.is_discarded()?"raw":"json";converted["content"]=content;}
    else if(mode=="urlencoded"||mode=="formdata") {
        converted["type"]=mode=="formdata"?"form_data":"x_www_form_urlencoded";Array(body,mode.c_str());
        for(const auto& entry:body.value(mode,json::array())){
            if(!entry.is_object())throw std::runtime_error("Postman form fields must be objects.");
            json field={{"key",Text(entry.value("key",json("")))},{"value",Text(entry.value("value",json("")))},{"enabled",!entry.value("disabled",false)},{"type",entry.value("type","")=="file"?"file":"text"}};
            if(field["type"]=="file"){const auto src=entry.value("src",json(""));if(src.is_string())field["filename"]=std::filesystem::u8path(src.get<std::string>()).filename().u8string();field["content"]="";field["contentType"]=entry.value("contentType","application/octet-stream");}
            converted["formItems"].push_back(std::move(field));
        }
    }
    else if(mode!="none")throw std::runtime_error("This Postman body mode cannot be imported without manual conversion.");
    result["body"]=std::move(converted);return result;
}
void Children(const json& source,json& target,unsigned depth,size_t& count){
    if(depth>32)throw std::runtime_error("Postman folder nesting exceeds 32 levels.");Array(source,"item");target["requests"]=json::array();target["folders"]=json::array();
    for(const auto& item:source.value("item",json::array())){if(!item.is_object()||++count>10000)throw std::runtime_error("Postman collection contains invalid or excessive items.");if(item.contains("item")){json folder={{"name",item.value("name","Folder")}};Common(item,folder);Children(item,folder,depth+1,count);target["folders"].push_back(std::move(folder));}else target["requests"].push_back(Request(item));}
}
}
void ValidateDocumentStructure(const json& document){
    std::vector<std::pair<const json*,unsigned>> pending{{&document,0}};size_t count=0;
    while(!pending.empty()){
        const auto next=pending.back();pending.pop_back();
        if(next.second>64||++count>100000)throw std::runtime_error("Document exceeds its nesting or node limit.");
        if(next.first->is_structured())for(const auto& child:*next.first)pending.emplace_back(&child,next.second+1);
    }
}
json RequestIterationData(const ApiRequest& request){
    if(request.iterationDataJson.empty())return json::object();
    const auto value=json::parse(request.iterationDataJson);ValidateDocumentStructure(value);
    if(value.is_object())return value;
    if(!value.is_array()||value.size()>10000)throw std::runtime_error("Iteration data must be an object or a bounded variable array.");
    json result=json::object();
    for(const auto& item:value){
        if(!item.is_object())throw std::runtime_error("Each iteration variable must be an object.");
        if(!item.value("enabled",true))continue;auto key=item.value("key",std::string{});
        const auto begin=key.find_first_not_of(" \t\r\n"),end=key.find_last_not_of(" \t\r\n");if(begin==std::string::npos)continue;key=key.substr(begin,end-begin+1);
        if(result.contains(key))throw std::runtime_error("Iteration variable names must be unique.");result[key]=item.value("value",json(""));
    }
    return result;
}
json ParseImportDocument(const std::string& text){
    if(text.empty()||text.size()>kMaximumBytes)throw std::runtime_error("Imports must contain 1 byte to 16 MiB.");
    auto result=json::parse(text,nullptr,false);if(!result.is_discarded()){ValidateDocumentStructure(result);return result;}
    try {
        const auto documents=YAML::LoadAll(text);if(documents.size()!=1)throw std::runtime_error("Import exactly one YAML document.");
        YamlBudget budget;result=ConvertYaml(documents[0],budget,0);
        if(result.dump().size()>kMaximumBytes)throw std::runtime_error("Expanded YAML exceeds 16 MiB.");return result;
    }catch(const YAML::Exception&){throw std::runtime_error("Invalid JSON or YAML import document.");}
}
json PostmanCollectionToNativeJson(const json& document){
    if(!document.is_object()||!document.contains("info")||!document["info"].is_object()||!document.contains("item"))throw std::runtime_error("Provide a Postman v2 collection.");
    const auto schema=document["info"].value("schema","");if(!schema.empty()&&schema.find("/v2.0.0/")==std::string::npos&&schema.find("/v2.1.0/")==std::string::npos)throw std::runtime_error("Only Postman collection v2.0 and v2.1 are supported.");
    json result={{"name",document["info"].value("name","Imported Collection")}};Common(document,result);
    result["description"]=Description(document["info"].value("description",json("")));size_t count=0;Children(document,result,0,count);return result;
}
}
