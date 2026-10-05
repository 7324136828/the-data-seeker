#include "CodeGen.h"
#include "VariableResolver.h"
#include <sstream>
#include <algorithm>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace native_app {
namespace {
using json=nlohmann::json;
std::string ShellQuote(const std::string& value){std::string result="'";for(char character:value)result+=character=='\''?"'\\''":std::string(1,character);return result+"'";}
std::string Literal(const std::string& value){return json(value).dump(-1,' ',false,json::error_handler_t::replace);}
std::string PythonLiteral(const json& value){
    if(value.is_null())return "None";if(value.is_boolean())return value.get<bool>()?"True":"False";
    if(value.is_string())return Literal(value.get<std::string>());if(value.is_number())return value.dump();
    std::string result=value.is_array()?"[":"{";bool first=true;
    for(auto item=value.begin();item!=value.end();++item){if(!first)result+=", ";first=false;if(value.is_object())result+=Literal(item.key())+": ";result+=PythonLiteral(item.value());}return result+(value.is_array()?"]":"}");
}
}
std::string CodeGen::Generate(const ApiRequest& input,const std::string& language){
    auto req=VariableResolver::PrepareRequest(input,{});std::string lang=language;std::transform(lang.begin(),lang.end(),lang.begin(),[](unsigned char value){return static_cast<char>(tolower(value));});
    const auto method=HttpMethodToString(req.method);const bool canBody=req.body.type!=BodyType::None;
    const bool multipart=canBody&&req.body.type==BodyType::FormData;
    std::vector<FormItem> form;for(const auto& item:req.body.formItems)if(item.enabled&&!item.key.empty())form.push_back(item);
    std::string body=req.body.type==BodyType::Raw||req.body.type==BodyType::Json?req.body.content:std::string{};
    if(canBody&&req.body.type==BodyType::UrlEncoded){body.clear();for(const auto& item:form){if(!body.empty())body+='&';body+=VariableResolver::UrlEncode(item.key)+"="+VariableResolver::UrlEncode(item.value);}bool exists=false;for(const auto& header:req.headers)if(header.enabled&&_stricmp(header.key.c_str(),"Content-Type")==0)exists=true;if(!exists)req.headers.push_back({"Content-Type","application/x-www-form-urlencoded",true,""});}
    if(multipart)req.headers.erase(std::remove_if(req.headers.begin(),req.headers.end(),[](const KeyValuePair& header){return _stricmp(header.key.c_str(),"Content-Type")==0;}),req.headers.end());
    if((lang=="javascript"||lang=="fetch")&&(req.method==HttpMethod::GET||req.method==HttpMethod::HEAD)&&(multipart||!body.empty()))throw std::runtime_error("Browser fetch cannot send a GET or HEAD body. Choose cURL, Python requests, or Axios for this request.");
    json headers=json::object();for(const auto& header:req.headers)if(header.enabled&&!header.key.empty())headers[header.key]=header.value;
    std::ostringstream out;
    if(lang=="curl"){
        out<<"curl -X "<<method<<" "<<ShellQuote(req.url)<<" \\\n  --max-time "<<req.options.timeoutSec;
        if(req.options.followRedirects)out<<" \\\n  -L";if(!req.options.verifyTls)out<<" \\\n  --insecure";
        for(const auto& header:req.headers)if(header.enabled&&!header.key.empty())out<<" \\\n  -H "<<ShellQuote(header.key+": "+header.value);
        if(!body.empty())out<<" \\\n  --data-raw "<<ShellQuote(body);
        if(multipart)for(const auto& item:form)out<<" \\\n  "<<(item.type=="file"?"-F ":"--form-string ")<<ShellQuote(item.key+"="+(item.type=="file"?"@"+item.filename:item.value));
        return out.str();
    }
    if(lang=="python"||lang=="requests"){
        out<<"import requests\n";if(multipart)out<<"import base64\n";out<<"\nurl = "<<Literal(req.url)<<"\nheaders = "<<PythonLiteral(headers)<<"\n";
        if(multipart){out<<"data = [\n";for(const auto& item:form)if(item.type!="file")out<<"    ("<<Literal(item.key)<<", "<<Literal(item.value)<<"),\n";out<<"]\nfiles = [\n";for(const auto& item:form)if(item.type=="file")out<<"    ("<<Literal(item.key)<<", ("<<Literal(item.filename)<<", base64.b64decode("<<Literal(item.contentBase64)<<"), "<<Literal(item.contentType)<<")),\n";out<<"]\n";}
        else if(!body.empty()){if(req.body.type==BodyType::Json){auto parsed=json::parse(body,nullptr,false);if(!parsed.is_discarded())out<<"payload = "<<PythonLiteral(parsed)<<"\n";else out<<"data = "<<Literal(body)<<"\n";}else out<<"data = "<<Literal(body)<<"\n";}
        out<<"response = requests.request("<<Literal(method)<<", url, headers=headers";
        if(multipart)out<<", data=data, files=files";else if(!body.empty())out<<(req.body.type==BodyType::Json&&!json::parse(body,nullptr,false).is_discarded()?", json=payload":", data=data");
        out<<", timeout="<<req.options.timeoutSec<<", allow_redirects="<<(req.options.followRedirects?"True":"False")<<", verify="<<(req.options.verifyTls?"True":"False")<<")\nprint(response.status_code)\nprint(response.text)\n";return out.str();
    }
    if(lang=="javascript"||lang=="fetch"||lang=="axios"){
        const bool axios=lang=="axios";
        if(axios)out<<"const axios = require('axios');\n";
        if(multipart){if(axios)out<<"const FormData = require('form-data');\n";out<<"const form = new FormData();\n";for(const auto& item:form){out<<"form.append("<<Literal(item.key)<<", ";if(item.type=="file"){if(axios)out<<"Buffer.from("<<Literal(item.contentBase64)<<", 'base64'), { filename: "<<Literal(item.filename)<<", contentType: "<<Literal(item.contentType)<<" }";else out<<"new Blob([Uint8Array.from(atob("<<Literal(item.contentBase64)<<"), c => c.charCodeAt(0))], { type: "<<Literal(item.contentType)<<" }), "<<Literal(item.filename);}else out<<Literal(item.value);out<<");\n";}}
        out<<"const headers = "<<headers.dump(2,' ',false,json::error_handler_t::replace)<<";\n";
        if(axios){out<<"const config = {\n  method: "<<Literal(method)<<",\n  url: "<<Literal(req.url)<<",\n  headers: "<<(multipart?"{ ...headers, ...form.getHeaders() }":"headers")<<",\n  timeout: "<<static_cast<int>(req.options.timeoutSec*1000)<<",\n  maxRedirects: "<<(req.options.followRedirects?20:0)<<",\n";if(multipart)out<<"  data: form,\n";else if(!body.empty())out<<"  data: "<<Literal(body)<<",\n";out<<"};\n";if(!req.options.verifyTls)out<<"config.httpsAgent = new (require('https').Agent)({ rejectUnauthorized: false });\n";out<<"axios(config)\n  .then(response => console.log(response.data))\n  .catch(error => console.error(error));\n";}
        else {if(!req.options.verifyTls)out<<"// Browser fetch uses the browser's TLS certificate policy.\n";out<<"fetch("<<Literal(req.url)<<", {\n  method: "<<Literal(method)<<",\n  headers,\n  redirect: "<<Literal(req.options.followRedirects?"follow":"manual")<<",\n  signal: AbortSignal.timeout("<<static_cast<int>(req.options.timeoutSec*1000)<<"),\n";if(multipart)out<<"  body: form,\n";else if(!body.empty())out<<"  body: "<<Literal(body)<<",\n";out<<"})\n  .then(response => response.text())\n  .then(data => console.log(data))\n  .catch(error => console.error(error));\n";}
        return out.str();
    }
    throw std::runtime_error("Choose cURL, Python requests, JavaScript fetch, or Axios.");
}
}
