#include <winsock2.h>
#include <ws2tcpip.h>
#include "ScriptRuntime.h"
#include "ScriptStore.h"
#include "JsonSerialization.h"
#include "HttpEngine.h"
#include "TempWorkspace.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#pragma comment(lib,"ws2_32.lib")
using namespace native_app;
namespace {
using json=nlohmann::json;
void RequireScript(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> void MustRejectScript(F run,const char* message){bool rejected=false;try{run();}catch(const std::exception&){rejected=true;}RequireScript(rejected,message);}
json Frame(const std::string& script,const char* phase="pre-request") {
    ApiRequest request;request.id="script_request";request.name="Native script fixture";request.url="http://127.0.0.1";
    return {{"scripts",json::array({{{"name","Fixture"},{"code",script}}})},{"request",RequestJson::ToJson(request)},
        {"phase",phase},{"environmentEnabled",true},{"collectionEnabled",true},{"iteration",2},
        {"scopes",{{"globals",{{"precedence","global"}}},{"collection",{{"precedence","collection"}}},
        {"environment",{{"precedence","environment"}}},{"data",{{"precedence","data"},{"row",7}}},
        {"local",{{"precedence","local"}}}}}};
}
class ScriptHttpFixture {
public:
    std::string received;
    ScriptHttpFixture(){
        WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))throw std::runtime_error("Script loopback initialization failed.");
        listener_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(listener_==INVALID_SOCKET||bind(listener_,reinterpret_cast<sockaddr*>(&address),sizeof(address))||listen(listener_,1))throw std::runtime_error("Script loopback setup failed.");
        int size=sizeof(address);getsockname(listener_,reinterpret_cast<sockaddr*>(&address),&size);port_=ntohs(address.sin_port);
        worker_=std::thread([this]{WSAPOLLFD descriptor{listener_,POLLRDNORM,0};if(WSAPoll(&descriptor,1,4000)<=0)return;SOCKET client=accept(listener_,nullptr,nullptr);if(client==INVALID_SOCKET)return;
            DWORD timeout=1500;setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));char buffer[4096];
            for(;;){int count=recv(client,buffer,sizeof(buffer),0);if(count<=0)break;received.append(buffer,count);const auto end=received.find("\r\n\r\n");if(end==std::string::npos)continue;size_t length=0;const auto marker=received.find("Content-Length:");if(marker!=std::string::npos)length=std::stoul(received.substr(marker+15));if(received.size()>=end+4+length)break;}
            const std::string response="HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\nConnection: close\r\n\r\n{\"ok\":true}";send(client,response.data(),static_cast<int>(response.size()),0);closesocket(client);});
    }
    ~ScriptHttpFixture(){if(worker_.joinable())worker_.join();if(listener_!=INVALID_SOCKET)closesocket(listener_);WSACleanup();}
    std::string Url()const{return "http://127.0.0.1:"+std::to_string(port_)+"/script";}
    void Join(){if(worker_.joinable())worker_.join();}
private:SOCKET listener_=INVALID_SOCKET;uint16_t port_=0;std::thread worker_;
};
std::string ReadScriptFile(const std::filesystem::path& file){std::ifstream input(file,std::ios::binary);std::ostringstream text;text<<input.rdbuf();return text.str();}
}
void TestNativeScriptRuntime(){
    auto frame=Frame(R"JS(
pm.test('scopes',()=>{pm.expect(pm.variables.get('precedence')).to.equal('local');pm.expect(pm.iterationData.get('row')).to.equal(7);pm.expect(pm.info.iteration).to.equal(2);});
pm.globals.set('number',42);pm.collectionVariables.set('object',{enabled:true});pm.environment.set('text','sample');pm.variables.set('typed',{items:[1,2]});
pm.request.headers.upsert({key:'X-Script',value:'native'});pm.request.url.query.upsert({key:'query',value:'changed'});pm.request.url.variables.upsert({key:'id',value:'123'});pm.request.url.update('http://127.0.0.1/changed');pm.request.body.update({ready:true});
pm.test('assertions',()=>{pm.expect({b:2,a:1}).to.deep.equal({a:1,b:2});pm.expect([1,2]).to.have.lengthOf(2);pm.expect('sample').to.match(/^sam/);pm.expect(pm.variables.replaceIn('id={{$guid}}')).not.to.include('{{');});
pm.test('host isolation',()=>{pm.expect(typeof require).to.equal('undefined');pm.expect(typeof process).to.equal('undefined');pm.expect(typeof fetch).to.equal('undefined');pm.expect(typeof Atomics).to.equal('undefined');pm.expect(typeof SharedArrayBuffer).to.equal('undefined');pm.expect(typeof setTimeout).to.equal('undefined');});
for(let i=0;i<120;i++)console.log('bounded');
)JS");
    const auto result=ScriptRuntime::RunScripts(frame);
    for(const auto& test:result["tests"])if(!test.value("passed",false))
        throw std::runtime_error("Native pm fixture "+test.value("name",std::string{})+": "+test.value("message",std::string{}));
    RequireScript(result["tests"].size()==3&&std::all_of(result["tests"].begin(),result["tests"].end(),[](const json& t){return t["passed"].get<bool>();}),"Native pm assertions/scopes/host isolation failed.");
    RequireScript(result["request"]["url"]=="http://127.0.0.1/changed"&&result["request"]["body"]["type"]=="json"&&result["request"]["pathParams"][0]["value"]=="123","Script request mutations did not round-trip.");
    RequireScript(result["changes"]["globals"]["set"]["number"]==42&&result["local"]["typed"]["items"][1]==2&&result["console"].size()==100,"Script typed values or console limits failed.");
    MustRejectScript([&]{ScriptRuntime::RunScripts(Frame("while(true){}"));},"Infinite pre script was not interrupted.");
    const auto timed=ScriptRuntime::RunScripts(Frame("while(true){}","post-response"));RequireScript(timed["tests"].size()==1&&!timed["tests"][0]["passed"].get<bool>(),"Post timeout was not a failed test.");
    MustRejectScript([&]{ScriptRuntime::RunScripts(Frame("new ArrayBuffer(128*1024*1024)"));},"Native script memory limit failed.");
    MustRejectScript([&]{ScriptRuntime::RunScripts(Frame("function recurse(){return recurse()} recurse()"));},"Native script stack limit failed.");
    MustRejectScript([&]{ScriptRuntime::RunScripts(Frame("return Promise.resolve(1)"));},"Asynchronous script was accepted.");
    auto disabled=Frame("pm.environment.set('key','value')");disabled["environmentEnabled"]=false;MustRejectScript([&]{ScriptRuntime::RunScripts(disabled);},"Disabled environment write was accepted.");
    MustRejectScript([&]{ScriptRuntime::RunScripts(Frame("pm.iterationData.set('row',8)"));},"Iteration data accepted a script write.");
    auto oversized=Frame(std::string(65537,' '));MustRejectScript([&]{ScriptRuntime::RunScripts(oversized);},"Script size limit failed.");
    auto many=Frame("true");for(int i=0;i<25;i++)many["scripts"].push_back(many["scripts"][0]);MustRejectScript([&]{ScriptRuntime::RunScripts(many);},"Inherited script count limit failed.");
    auto large=Frame("true");large["request"]["body"]["content"]=std::string(4*1024*1024,'x');MustRejectScript([&]{ScriptRuntime::RunScripts(large);},"Sandbox input limit failed.");
    std::atomic_bool cancelled{false};std::thread cancellation([&]{std::this_thread::sleep_for(std::chrono::milliseconds(30));cancelled.store(true);});
    const auto start=std::chrono::steady_clock::now();bool cancelledFrame=false;
    try{ScriptRuntime::RunScripts(Frame("while(true){}"),&cancelled);}catch(const std::exception&){cancelledFrame=true;}
    cancellation.join();RequireScript(cancelledFrame,"Running script cancellation failed.");
    RequireScript(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(750),"Script cancellation did not interrupt promptly.");
    const auto healthy=ScriptRuntime::RunScripts(Frame("pm.test('after errors',()=>pm.expect(1).to.equal(1))"));RequireScript(healthy["tests"][0]["passed"].get<bool>(),"Native runtime did not recover after rejected frames.");
    auto noScripts=Frame("");noScripts["request"]["body"]["content"]=std::string(4*1024*1024,'x');
    RequireScript(ScriptRuntime::RunScripts(noScripts)["tests"].empty(),"An absent script incorrectly imposed the sandbox body limit.");
    MustRejectScript([&]{ScriptRuntime::RunScripts(Frame("/(a+)+$/.test('a'.repeat(30)+'!')"));},"Native regexp execution escaped its timeout.");
    std::exception_ptr workerFailure;json workerResult;
    std::thread worker([&]{try{MustRejectScript([&]{ScriptRuntime::RunScripts(Frame("function recurse(){return recurse()} recurse()"));},"Worker-thread recursion exceeded the native stack reserve.");workerResult=ScriptRuntime::RunScripts(frame);}catch(...){workerFailure=std::current_exception();}});worker.join();
    if(workerFailure)std::rethrow_exception(workerFailure);
    RequireScript(workerResult["tests"][1]["passed"].get<bool>(),"Native worker-thread script stack failed.");
    ScriptHttpFixture server;HttpEngine http;ApiRequest request;request.id="native_script_http";request.name="Script HTTP";request.url=server.Url();request.method=HttpMethod::POST;
    request.variables.push_back({"requestDefault","old",true,""});request.headers.push_back({"X-Local-Value","{{requestDefault}}",true,""});request.headers.push_back({"X-Data-Value","{{wireData}}",true,""});request.iterationDataJson=R"({"wireData":"iteration-wire","row":7,"requestDefault":"data"})";request.iteration=11;request.hasIteration=true;
    request.preRequestScript="pm.variables.set('requestDefault','changed');pm.expect(pm.variables.get('order')).to.equal('collection-folder'); pm.request.headers.upsert({key:'X-Native-Script',value:'yes'}); pm.request.body.update({message:'unicode-日本語'});pm.variables.set('requestOnly',9);";
    request.postResponseScript="pm.test('native response',()=>{pm.response.to.have.status(200);pm.expect(pm.response.json().ok).to.be.true;pm.expect(pm.variables.get('requestOnly')).to.equal(9)});pm.globals.set('count',2);console.info('post');";
    ScriptContext context;context.preRequestScripts={{"Collection","pm.variables.set('order','collection')"},{"Folder","pm.variables.set('order',pm.variables.get('order')+'-folder');console.log('pre')"}};
    context.postResponseScripts={{"Collection","pm.globals.set('count',1)"}};
    context.preRequestScripts.push_back({"Iteration metadata","pm.expect(pm.iterationData.get('row')).to.equal(7);pm.expect(pm.info.iteration).to.equal(11);pm.expect(pm.variables.get('requestDefault')).to.equal('old');"});
    const auto executed=ScriptRuntime::Execute(http,request,context);server.Join();
    RequireScript(executed.response.statusCode==200&&executed.response.error.empty()&&executed.response.testResults.size()==1&&executed.response.testResults[0].passed,"Native pre/WinHTTP/post integration failed.");
    RequireScript(server.received.find("X-Native-Script: yes")!=std::string::npos&&server.received.find(u8"unicode-日本語")!=std::string::npos,"Native HTTP did not send script-mutated headers/body.");
    RequireScript(server.received.find("X-Local-Value: changed")!=std::string::npos&&executed.request.variables[0].value=="old","Script locals did not override saved request defaults or configuration was mutated.");
    RequireScript(server.received.find("X-Data-Value: iteration-wire")!=std::string::npos,"Script dispatch did not apply iteration data to HTTP fields.");
    RequireScript(executed.changes["globals"]["set"]["count"]==2&&executed.local["requestOnly"]==9&&executed.response.scriptConsole.size()==2,"Combined native script results failed.");
    request.variables.push_back(request.variables[0]);MustRejectScript([&]{ScriptRuntime::Execute(http,request,context);},"Script dispatch accepted duplicate request variables.");
}
void TestNativeScriptPersistence(){
    TempWorkspace workspace("native_script_store_");const auto directory=workspace.GetSubpath("store");
    Collection collection;collection.id="script_collection";collection.name="Script collection";collection.preRequestScript="console.log('synthetic-script-source')";
    Folder folder;folder.id="script_folder";folder.name="Script folder";folder.postResponseScript="pm.test('folder',()=>pm.expect(true).to.be.true)";folder.variables={{"owned","5",true,"5"}};
    ApiRequest request;request.id="saved_script";request.name="Script request";request.url="http://127.0.0.1";request.preRequestScript="pm.globals.set('fixture','synthetic-script-value')";request.postResponseScript="console.log('synthetic-post-source')";folder.requests={request};collection.folders={folder};collection.variables={{"owned","4",true,"4"}};
    collection.description="synthetic-collection-description";folder.description="synthetic-folder-description";request.description="synthetic-request-description";
    request.headers.push_back({"X-Fixture","value",true,"synthetic-header-description"});request.params.push_back({"q","value",true,"synthetic-parameter-description"});request.pathParams.push_back({"id","value",true,"synthetic-path-description"});request.variables.push_back({"local","value",true,"synthetic-variable-description"});folder.requests={request};collection.folders={folder};
    request.iterationDataJson=R"({"fixture":"synthetic-private-iteration-data","number":7})";request.hasIteration=true;request.iteration=3;folder.requests={request};collection.folders={folder};
    {
        StoreManager store(directory.u8string());store.SaveCollections({collection});auto duplicate=collection;duplicate.id="other_collection";duplicate.folders[0].id="other_folder";MustRejectScript([&]{store.SaveCollections({collection,duplicate});},"Cross-collection request identifier collision was accepted.");MustRejectScript([&]{store.SaveCollections({collection,collection});},"Duplicate root collection identifier was accepted.");RequireScript(store.GetCollections().size()==1,"Rejected workspace identifiers changed the saved state.");Environment environment;environment.id="script_environment";environment.name="Fixture";store.SaveEnvironments({environment.id,{environment}});
        const json changes={{"globals",{{"set",{{"typedCount",42}}},{"unset",json::array()}}},
            {"collection",{{"set",{{"owned",10},{"newObject",{{"ready",true}}}}},{"unset",json::array()}}},
            {"environment",{{"set",{{"numeric",7}}},{"unset",json::array()}}}};
        const json noScope={{"collection",{{"set",json::object()},{"unset",{"absent"}}}},{"environment",{{"set",json::object()},{"unset",{"absent"}}}}};
        RequireScript(!ScriptStore::ApplyChanges(store,noScope,"","",""),"Unsetting an absent unselected script scope should be a no-op.");
        RequireScript(ScriptStore::ApplyChanges(store,changes,collection.id,folder.id,environment.id),"Script store deltas were not applied.");
        HistoryEntry history;history.id="script_history";history.url=request.url;history.requestSnapshot=request;store.AddHistory(history);
    }
    const auto disk=ReadScriptFile(directory/"collections.json");RequireScript(disk.find("synthetic-script-source")==std::string::npos&&disk.find("synthetic-script-value")==std::string::npos&&disk.find("synthetic-post-source")==std::string::npos,"Saved script source was not protected at rest.");
    for(const auto* privateText:{"synthetic-collection-description","synthetic-folder-description","synthetic-request-description","synthetic-header-description","synthetic-parameter-description","synthetic-path-description","synthetic-variable-description"})RequireScript(disk.find(privateText)==std::string::npos,"A saved free-form description was stored as plaintext.");
    RequireScript(disk.find("synthetic-private-iteration-data")==std::string::npos,"Iteration data was not protected at rest.");
    StoreManager reopened(directory.u8string());const auto collections=reopened.GetCollections();const auto& saved=collections[0];
    RequireScript(saved.preRequestScript==collection.preRequestScript&&saved.folders[0].postResponseScript==folder.postResponseScript&&saved.folders[0].requests[0].preRequestScript==request.preRequestScript,"Protected inherited/request scripts did not restore.");
    const auto& restoredRequest=saved.folders[0].requests[0];RequireScript(saved.description==collection.description&&saved.folders[0].description==folder.description&&restoredRequest.description==request.description&&restoredRequest.headers[0].description==request.headers[0].description&&restoredRequest.params[0].description==request.params[0].description&&restoredRequest.pathParams[0].description==request.pathParams[0].description&&restoredRequest.variables[0].description==request.variables[0].description,"Protected descriptions did not restore.");
    RequireScript(restoredRequest.iterationDataJson==request.iterationDataJson&&restoredRequest.hasIteration&&restoredRequest.iteration==3,"Protected iteration metadata did not restore.");
    RequireScript(ScriptStore::VariablesToJson(reopened.GetGlobals())["typedCount"]==42&&ScriptStore::VariablesToJson(saved.folders[0].variables)["owned"]==10&&ScriptStore::VariablesToJson(saved.variables)["owned"]==4,"Typed global/collection variables lost their values or nearest owner.");
    RequireScript(ScriptStore::VariablesToJson(reopened.GetEnvironments().environments[0].variables)["numeric"]=="7","Environment script writes did not preserve text persistence semantics.");
    const auto history=reopened.GetHistory();RequireScript(history[0].requestSnapshot.preRequestScript.empty()&&history[0].requestSnapshot.postResponseScript.empty(),"History retained script source.");
    const auto& historyRequest=history[0].requestSnapshot;RequireScript(historyRequest.description.empty()&&historyRequest.headers[0].description.empty()&&historyRequest.params[0].description.empty()&&historyRequest.pathParams[0].description.empty()&&historyRequest.variables[0].description.empty(),"History retained free-form descriptions.");
    RequireScript(historyRequest.iterationDataJson.empty(),"History retained iteration data.");
    const auto publicValue=CollectionJson::ToJson(saved);const auto imported=CollectionJson::FromJson(publicValue,true);
    RequireScript(imported.id!=saved.id&&imported.folders[0].id!=saved.folders[0].id&&imported.folders[0].requests[0].id!=request.id&&imported.preRequestScript==saved.preRequestScript,"Native collection import lost scripts or reused IDs.");
    RequireScript(ScriptStore::VariablesToJson(imported.variables)["newObject"]["ready"].get<bool>(),"Public collection JSON lost typed script variable metadata.");
    auto legacy=json::parse(disk);legacy[0]["description"]="legacy-description-fixture";{std::ofstream legacyFile(directory/"collections.json",std::ios::binary|std::ios::trunc);legacyFile<<legacy.dump();}
    StoreManager legacyStore(directory.u8string());RequireScript(legacyStore.GetCollections()[0].description=="legacy-description-fixture","Legacy plaintext descriptions no longer load.");
}
