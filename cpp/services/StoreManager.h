#pragma once

#include "Types.h"
#include <string>
#include <vector>
#include <mutex>

namespace native_app {

class StoreManager {
public:
    explicit StoreManager(const std::string& dataDir);
    ~StoreManager() = default;

    std::vector<Collection> GetCollections();
    void SaveCollections(const std::vector<Collection>& collections);

    EnvironmentStore GetEnvironments();
    void SaveEnvironments(const EnvironmentStore& envStore);
    void SetActiveEnvironment(const std::string& envId);

    std::vector<Variable> GetGlobals();
    void SaveGlobals(const std::vector<Variable>& globals);

    std::vector<HistoryEntry> GetHistory();
    void AddHistory(const HistoryEntry& entry);
    void ClearHistory();

    std::map<std::string, std::string> GetMergedVariables(
        const std::string& environmentId,
        const std::vector<Variable>& requestVars = {}
    );

private:
    std::string dataDir_;
    std::string collectionsFile_;
    std::string environmentsFile_;
    std::string globalsFile_;
    std::string historyFile_;

    std::vector<Collection> collections_;
    EnvironmentStore environments_;
    std::vector<Variable> globals_;
    std::vector<HistoryEntry> history_;
    std::mutex mutex_;

    void LoadAll();
    void InitDefaults();
    void SaveCollectionsInternal();
    void SaveEnvironmentsInternal();
    void SaveGlobalsInternal();
    void SaveHistoryInternal();
};

} // namespace native_app
