#pragma once
#include "StoreManager.h"
#include <nlohmann/json.hpp>
namespace native_app {
namespace ScriptStore {
nlohmann::json VariablesToJson(const std::vector<Variable>& variables);
nlohmann::json VariablesToJson(const std::vector<KeyValuePair>& variables);
// Launch identities target the original saved context even if selection changes.
bool ApplyChanges(StoreManager& store, const nlohmann::json& changes,
    const std::string& collectionId, const std::string& folderId,
    const std::string& environmentId);
}
}
