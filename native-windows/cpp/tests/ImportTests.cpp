#include "native/ImportDocument.h"
#include "native/JsonSerialization.h"
#include "native/OpenApiParser.h"
#include "native/VariableResolver.h"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
namespace {
using json=nlohmann::json;using namespace native_app;
void CheckImport(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>void RejectImport(F action,const char* message){bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}CheckImport(rejected,message);}
}
void TestNativeImportFormats(){
    const auto yaml=ParseImportDocument(R"YAML(
defaults: &defaults
  active: true
  number: 7
  quoted: "007"
node:
  <<: *defaults
  number: 9
sequence: [null, false, "true", 2.5]
)YAML");
    CheckImport(yaml["node"]["active"]==true&&yaml["node"]["number"]==9&&yaml["node"]["quoted"]=="007"&&yaml["sequence"][2]=="true","YAML aliases, merge precedence, or quoted scalar types were lost.");
    const std::string spec=R"YAML(
openapi: 3.0.3
info:
  title: "Unicode 日本語"
servers:
  - url: https://api.example.com/v1
components:
  schemas:
    Input:
      type: object
      properties:
        enabled: {type: boolean, default: false}
paths:
  /items/{id}:
    parameters:
      - name: id
        in: path
        required: true
        schema: {type: string, default: "a/b"}
    post:
      description: "Saved request description"
      x-prerequest-script: "pm.variables.set('imported', true)"
      requestBody:
        content:
          application/json:
            schema: {$ref: "#/components/schemas/Input"}
      responses:
        "201": {description: Created}
)YAML";
    const auto imported=CollectionJson::Import(spec);
    CheckImport(imported.size()==1&&imported[0].name==u8"Unicode 日本語"&&imported[0].requests.size()==1,"Native YAML OpenAPI routing failed.");
    const auto& request=imported[0].requests[0];
    CheckImport(json::parse(request.body.content)["enabled"]==false&&request.description=="Saved request description"&&!request.preRequestScript.empty(),"YAML references, body, description, or script hooks were lost.");
    CheckImport(VariableResolver::PrepareRequest(request,{},true).url=="https://api.example.com/v1/items/a%2Fb","Imported YAML path variables were not dispatchable.");
    const std::string postman=R"JSON({
      "info":{"name":"Postman fixture","description":{"content":"Collection description"},"schema":"https://schema.getpostman.com/json/collection/v2.1.0/collection.json"},
      "auth":{"type":"bearer","bearer":[{"key":"token","value":"{{access_token}}"}]},
      "variable":[{"key":"collectionValue","value":5},{"key":"disabled","value":"x","disabled":true}],
      "event":[{"listen":"prerequest","script":{"exec":["pm.variables.set('order', 'collection')","console.log('imported')"]}}],
      "item":[{"name":"Folder","description":"Folder description","auth":{"type":"noauth"},
        "event":[{"listen":"test","script":{"exec":"pm.test('folder',()=>pm.expect(true).to.be.true)"}}],
        "item":[{"name":"Create","description":{"content":"Request description"},"variable":[{"key":"local","value":"text"}],
          "event":[{"listen":"prerequest","script":{"exec":["pm.request.headers.upsert({key:'X-Import',value:'yes'})"]}}],
          "request":{"method":"POST","url":{"raw":"https://api.example.com/items/:id?literal=old","query":[{"key":"literal","value":"new"},{"key":"off","value":"x","disabled":true}],"variable":[{"key":"id","value":"a/b"}]},
            "header":[{"key":"X-Enabled","value":"yes"},{"key":"X-Disabled","value":"no","disabled":true}],
            "body":{"mode":"raw","raw":"{\"ready\":true}"}}}
        ]},
        {"name":"Upload","request":{"url":"https://api.example.com/upload","method":"POST","body":{"mode":"formdata","formdata":[{"key":"file","type":"file","src":"C:/does-not-exist/private.bin"},{"key":"note","value":"text"}]}}}
      ]
    })JSON";
    const auto collections=CollectionJson::Import("["+postman+"]");const auto& collection=collections[0];const auto& folder=collection.folders[0];const auto& converted=folder.requests[0];
    CheckImport(collection.auth.type==AuthType::Bearer&&collection.variables[0].value=="5"&&!collection.variables[1].enabled&&!collection.preRequestScript.empty(),"Postman inherited auth, variables, or scripts were lost.");
    CheckImport(CollectionJson::ToJson(collection)["variables"][0]["value"]==5,"Typed Postman collection variables were reduced to text.");
    CheckImport(folder.description=="Folder description"&&folder.auth.type==AuthType::None&&!folder.postResponseScript.empty(),"Postman folder metadata was lost.");
    CheckImport(converted.description=="Request description"&&converted.auth.type==AuthType::Inherit&&converted.body.type==BodyType::Json&&converted.headers.size()==2&&!converted.headers[1].enabled&&!converted.preRequestScript.empty(),"Postman request metadata/body/scripts were lost.");
    CheckImport(converted.pathParams[0].value=="a/b"&&converted.params[0].value=="new"&&!converted.params[1].enabled,"Postman query/path parameters or disabled flags were lost.");
    CheckImport(collection.requests[0].body.formItems[0].type=="file"&&collection.requests[0].body.formItems[0].filename=="private.bin"&&collection.requests[0].body.formItems[0].contentBase64.empty(),"Postman file references read arbitrary file content or lost file metadata.");
    const auto copy=CollectionJson::FromJson(CollectionJson::ToJson(collection),true);
    CheckImport(copy.id!=collection.id&&copy.folders[0].requests[0].id!=converted.id&&copy.folders[0].requests[0].description==converted.description,"Collection import IDs or description round trip failed.");
    auto noIds=CollectionJson::ToJson(collection);noIds["id"]="";noIds["requests"][0]["id"]="";noIds["folders"][0]["id"]="";noIds["folders"][0]["requests"][0]["id"]="";
    const auto repaired=CollectionJson::FromJson(noIds);
    CheckImport(!repaired.id.empty()&&!repaired.requests[0].id.empty()&&!repaired.folders[0].id.empty()&&!repaired.folders[0].requests[0].id.empty(),"Nested empty identifiers were not repaired.");
    auto duplicates=CollectionJson::ToJson(collection);duplicates["folders"][0]["requests"][0]["id"]=duplicates["requests"][0]["id"];
    RejectImport([&]{CollectionJson::FromJson(duplicates);},"Duplicate collection item identifiers were accepted.");
    const auto renewed=CollectionJson::FromJson(duplicates,true);
    CheckImport(renewed.folders[0].requests[0].id!=renewed.requests[0].id,"Duplicate imported identifiers were not regenerated.");
    auto legacy=CollectionJson::ToJson(collection);legacy["requests"][0]["body"]="legacy raw body";
    CheckImport(CollectionJson::FromJson(legacy).requests[0].body.content=="legacy raw body","Native legacy string request body was lost.");
    auto requestJson=RequestJson::ToJson(converted);requestJson["variables"].push_back({{"key","typed"},{"value",{{"items",{1,2}}}}});requestJson["options"]={{"timeout",3.5},{"follow_redirects",false},{"verify_tls",false}};
    const auto typed=RequestJson::FromJson(requestJson);
    CheckImport(!typed.variables.back().scriptJsonValue.empty()&&typed.options.timeoutSec==3.5&&!typed.options.followRedirects&&!typed.options.verifyTls,"Typed request variables or original request-option names failed.");
    CheckImport(RequestJson::ToJson(typed)["variables"].back()["value"]["items"][1]==2,"Typed request variable export failed.");
    requestJson["iterationData"]={{"fromData",7},{"precedence","data"}};requestJson["iteration"]=11;requestJson["url"]="https://api.example.com/{{fromData}}/{{precedence}}";requestJson["params"]=json::array();requestJson["pathParams"]=json::array();requestJson["variables"]=json::array({{{"key","precedence"},{"value","local"}}});
    const auto iterationRequest=RequestJson::FromJson(requestJson);const auto iterationExport=RequestJson::ToJson(iterationRequest);
    CheckImport(iterationRequest.hasIteration&&iterationRequest.iteration==11&&iterationExport["iterationData"]["fromData"]==7,"Request iteration metadata was lost.");
    CheckImport(VariableResolver::PrepareRequest(iterationRequest,{{"fromData","global"},{"precedence","environment"}},true).url=="https://api.example.com/7/local","Iteration data did not resolve without scripts before request-local values.");
    requestJson["iterationData"]=json::array({{{"key","fromData"},{"value",9}},{{"key","off"},{"value",1},{"enabled",false}}});
    CheckImport(RequestIterationData(RequestJson::FromJson(requestJson))["fromData"]==9,"List-shaped iteration variables were lost.");
    requestJson["variables"]={{"precedence","object-local"},{"typedLocal",7}};const auto objectVariables=RequestJson::FromJson(requestJson);
    CheckImport(objectVariables.variables.size()==2&&RequestJson::ToJson(objectVariables)["variables"][1]["value"]==7,"Object-shaped typed request variables were lost.");
    auto duplicateVariables=objectVariables;duplicateVariables.variables.push_back(duplicateVariables.variables[0]);RejectImport([&]{VariableResolver::PrepareRequest(duplicateVariables,{});},"Direct HTTP/snippet preparation accepted duplicate request variables.");
    auto objectScopes=CollectionJson::ToJson(collection);objectScopes["variables"]={{"typedScope",{{"ready",true}}}};objectScopes["folders"][0]["variables"]={{"number",3}};
    const auto objectCollection=CollectionJson::FromJson(objectScopes);
    CheckImport(CollectionJson::ToJson(objectCollection)["variables"][0]["value"]["ready"]==true&&objectCollection.folders[0].variables[0].value=="3","Object-shaped collection/folder scopes were lost.");
    requestJson["variables"]=json::array({{{"key","same"},{"value",1}},{{"key"," same "},{"value",2}}});RejectImport([&]{RequestJson::FromJson(requestJson);},"Duplicate enabled variable names were silently accepted.");
    for(const auto& invalid:{std::string("node: &node [*node]"),std::string("object: !!python/object:danger {}"),std::string("one: 1\none: 2"),std::string("---\none: 1\n---\ntwo: 2")})
        RejectImport([&]{ParseImportDocument(invalid);},"Unsafe or ambiguous YAML was accepted.");
    RejectImport([&]{CollectionJson::Import(R"({"info":{"schema":"https://example.com/v1/"},"item":[]})");},"Unsupported Postman version was accepted.");
    RejectImport([&]{CollectionJson::Import(R"({"info":{},"item":[],"event":[{"listen":"test","script":{"exec":[42]}}]})");},"Invalid Postman script lines were accepted.");
    std::string deep="value: ";for(int i=0;i<65;++i)deep+='[';deep+="0";for(int i=0;i<65;++i)deep+=']';
    RejectImport([&]{ParseImportDocument(deep);},"Excessive YAML nesting was accepted.");
    std::string deepJson;for(int i=0;i<66;++i)deepJson+='[';deepJson+="0";for(int i=0;i<66;++i)deepJson+=']';
    RejectImport([&]{ParseImportDocument(deepJson);},"Excessive JSON nesting was accepted.");
    std::string expanded="root: &root [";for(int i=0;i<1001;++i){if(i)expanded+=',';expanded+='0';}expanded+="]\nexpanded: [";for(int i=0;i<101;++i){if(i)expanded+=',';expanded+="*root";}expanded+=']';
    RejectImport([&]{ParseImportDocument(expanded);},"YAML aliases escaped the expanded node bound.");
}
