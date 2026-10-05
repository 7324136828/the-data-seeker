#include "ScriptStore.h"
#include <algorithm>
#include <set>
#include <stdexcept>
namespace native_app {
namespace ScriptStore {
using json=nlohmann::json;
json VariablesToJson(const std::vector<Variable>& variables) {
    json result=json::object();for(const auto& value:variables)if(value.enabled&&!value.key.empty())
        result[value.key]=value.scriptJsonValue.empty()?json(value.value):json::parse(value.scriptJsonValue);
    return result;
}
json VariablesToJson(const std::vector<KeyValuePair>& variables) {
    json result=json::object();for(const auto& value:variables)if(value.enabled&&!value.key.empty())result[value.key]=value.scriptJsonValue.empty()?json(value.value):json::parse(value.scriptJsonValue);return result;
}
namespace {
void CheckDelta(const json& delta) {
    if(!delta.is_object())throw std::runtime_error("Script variable changes must be objects.");
    const auto set=delta.value("set",json::object()),unset=delta.value("unset",json::array());
    if(!set.is_object()||!unset.is_array()||set.size()+unset.size()>10000)throw std::runtime_error("Script variable changes exceed their schema or limit.");
    for(auto item=set.begin();item!=set.end();++item)if(item.key().empty()||item.key().size()>256)throw std::runtime_error("Script variable names require 1 to 256 characters.");
    for(const auto& key:unset)if(!key.is_string()||key.get_ref<const std::string&>().empty()||key.get_ref<const std::string&>().size()>256)throw std::runtime_error("Invalid script variable removal.");
}
bool HasChanges(const json& delta){CheckDelta(delta);return !delta.value("set",json::object()).empty()||!delta.value("unset",json::array()).empty();}
void Patch(std::vector<Variable>& values,const json& delta,bool asText) {
    const auto removed=delta.value("unset",json::array());std::set<std::string> names;for(const auto& key:removed)names.insert(key.get<std::string>());
    values.erase(std::remove_if(values.begin(),values.end(),[&](const Variable& value){return names.count(value.key)!=0;}),values.end());
    const auto added=delta.value("set",json::object());
    for(auto item=added.begin();item!=added.end();++item){
        auto existing=std::find_if(values.begin(),values.end(),[&](const Variable& value){return value.key==item.key();});
        if(existing==values.end()){values.push_back({});existing=values.end()-1;existing->key=item.key();}
        existing->value=item.value().is_string()?item.value().get<std::string>():item.value().dump();existing->enabled=true;
        existing->scriptJsonValue=asText||item.value().is_string()?std::string{}:item.value().dump();
    }
}
bool FindChain(std::vector<Folder>& folders,const std::string& id,std::vector<std::vector<Variable>*>& chain) {
    for(auto& folder:folders){chain.push_back(&folder.variables);if(folder.id==id||FindChain(folder.folders,id,chain))return true;chain.pop_back();}return false;
}
}
bool ApplyChanges(StoreManager& store,const json& changes,const std::string& collectionId,
    const std::string& folderId,const std::string& environmentId) {
    if(!changes.is_object())throw std::runtime_error("Script changes must be an object.");
    for(auto item=changes.begin();item!=changes.end();++item){
        if(item.key()!="globals"&&item.key()!="collection"&&item.key()!="environment"&&item.key()!="local")throw std::runtime_error("Unknown script variable scope.");CheckDelta(item.value());
    }
    bool changed=false;
    // Resolve destinations before saving, so a deleted context cannot redirect a write.
    auto collections=store.GetCollections();auto environments=store.GetEnvironments();auto globals=store.GetGlobals();
    std::vector<std::vector<Variable>*> chain;Environment* environment=nullptr;
    if(changes.contains("collection")&&HasChanges(changes["collection"])&&(!collectionId.empty()||!changes["collection"].value("set",json::object()).empty())){
        auto collection=std::find_if(collections.begin(),collections.end(),[&](const Collection& c){return c.id==collectionId;});
        if(collection==collections.end())throw std::runtime_error("The script's saved collection no longer exists.");chain.push_back(&collection->variables);
        if(!folderId.empty()&&!FindChain(collection->folders,folderId,chain))throw std::runtime_error("The script's saved folder no longer exists.");
    }
    if(changes.contains("environment")&&HasChanges(changes["environment"])&&(!environmentId.empty()||!changes["environment"].value("set",json::object()).empty())){
        auto found=std::find_if(environments.environments.begin(),environments.environments.end(),[&](const Environment& e){return e.id==environmentId;});
        if(found==environments.environments.end())throw std::runtime_error("The script's selected environment no longer exists.");environment=&*found;
    }
    if(changes.contains("globals")&&HasChanges(changes["globals"])){Patch(globals,changes["globals"],false);store.SaveGlobals(globals);changed=true;}
    if(environment){Patch(environment->variables,changes["environment"],true);store.SaveEnvironments(environments);changed=true;}
    if(!chain.empty()){
        const auto& delta=changes["collection"];std::set<std::string> keys;const auto added=delta.value("set",json::object());for(auto item=added.begin();item!=added.end();++item)keys.insert(item.key());for(const auto& key:delta.value("unset",json::array()))keys.insert(key.get<std::string>());
        for(const auto& key:keys){auto* owner=chain.front();for(auto item=chain.rbegin();item!=chain.rend();++item)
            if(std::any_of((*item)->begin(),(*item)->end(),[&](const Variable& v){return v.enabled&&v.key==key;})){owner=*item;break;}
            json one={{"set",json::object()},{"unset",json::array()}};if(added.contains(key))one["set"][key]=added[key];else one["unset"].push_back(key);Patch(*owner,one,false);
        }
        store.SaveCollections(collections);changed=true;
    }
    return changed;
}
}
}
