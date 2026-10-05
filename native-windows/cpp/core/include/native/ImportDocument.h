#pragma once
#include <nlohmann/json.hpp>
#include <string>
namespace native_app {
struct ApiRequest;
// Data-only parsing never loads files, resolves network references, or executes scripts.
nlohmann::json ParseImportDocument(const std::string& text);
void ValidateDocumentStructure(const nlohmann::json& document);
nlohmann::json RequestIterationData(const ApiRequest& request);
nlohmann::json PostmanCollectionToNativeJson(const nlohmann::json& document);
}
