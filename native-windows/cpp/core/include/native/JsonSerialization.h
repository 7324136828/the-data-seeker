#pragma once
#include "Types.h"
#include <nlohmann/json.hpp>
#include <vector>
namespace native_app {
namespace RequestJson {
nlohmann::json ToJson(const ApiRequest& request);
ApiRequest FromJson(const nlohmann::json& value, bool regenerateId=false);
}
namespace CollectionJson {
nlohmann::json ToJson(const Collection& collection);
Collection FromJson(const nlohmann::json& value, bool regenerateIds=false);
std::vector<Collection> Import(const std::string& text, bool regenerateIds=true);
}
}