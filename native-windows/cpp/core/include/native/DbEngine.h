#pragma once

#include "Types.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <set>

namespace native_app {
class NativeConnections;
struct RowMutationResult { bool success = false; int64_t rowsAffected = 0; TypedRow returned; };

class DbEngine {
public:
    explicit DbEngine(const std::string& dataDir, bool seedSamples = false);
    ~DbEngine();
    const std::string& WorkspaceDirectory() const { return dataDir_; }

    std::vector<DatabaseInfo> ListDatabases();
    DatabaseInfo CreateDatabase(const std::string& name, const std::string& preset = "blank", const std::string& destinationPath = "");
    DatabaseInfo AttachDatabase(const std::string& name, const std::string& path);
    bool DeleteDatabase(const std::string& dbId);
    bool RemoveConnection(const std::string& dbId);
    std::vector<DatabaseDriverInfo> ListDrivers();
    ConnectionProfile GetConnectionProfile(const std::string& dbId);
    std::string TestConnection(const ConnectionProfile& profile, const std::atomic_bool* cancellation = nullptr);
    DatabaseInfo SaveConnection(const ConnectionProfile& profile, bool connect = false, const std::atomic_bool* cancellation = nullptr);
    bool DisconnectDatabase(const std::string& dbId);
    bool InsertTypedRow(const std::string& dbId, const std::string& table, const TypedRow& values, const std::atomic_bool* cancellation = nullptr);
    bool UpdateTypedRow(const std::string& dbId, const std::string& table, const TypedRow& key, const TypedRow& values, const std::atomic_bool* cancellation = nullptr);
    bool DeleteTypedRow(const std::string& dbId, const std::string& table, const TypedRow& key, const std::atomic_bool* cancellation = nullptr);
    RowMutationResult InsertTypedReturningRow(const std::string& dbId, const std::string& table, const TypedRow& values, const std::atomic_bool* cancellation = nullptr);
    TypedRow FindTypedRow(const std::string& dbId, const std::string& table, const TypedRow& key, const std::atomic_bool* cancellation = nullptr);

    DatabaseSchema GetSchema(const std::string& dbId, const std::atomic_bool* cancellation = nullptr);
    ERDiagram GetErDiagram(const std::string& dbId, const std::atomic_bool* cancellation = nullptr);

    QueryResult ExecuteQuery(const std::string& dbId, const std::string& sql, int maxRows = 1000, const std::atomic_bool* cancellation = nullptr);
    TableDataResult GetTableData(
        const std::string& dbId,
        const std::string& table,
        int page = 1,
        int pageSize = 25,
        const std::string& sortCol = "",
        const std::string& sortDir = "asc",
        const std::string& search = "",
        const std::string& filterCol = "",
        const std::string& filterVal = "",
        const std::atomic_bool* cancellation = nullptr
    );

    bool InsertTableRow(const std::string& dbId, const std::string& table, const std::map<std::string, std::string>& rowData);
    bool UpdateTableRow(const std::string& dbId, const std::string& table, const std::map<std::string, std::string>& pkData, const std::map<std::string, std::string>& rowData);
    bool DeleteTableRow(const std::string& dbId, const std::string& table, const std::map<std::string, std::string>& pkData);

    std::string ExportTableData(
        const std::string& dbId,
        const std::string& table,
        const std::string& format, // "csv", "json", "sql"
        const std::string& search = "",
        const std::string& sortCol = "",
        const std::string& sortDir = "asc",
        const std::atomic_bool* cancellation = nullptr
    );

    std::string FormatQuery(const std::string& query, const std::string& kind = "sql");
    QueryResult ExplainQuery(const std::string& dbId, const std::string& query, const std::atomic_bool* cancellation = nullptr);

private:
    std::string dataDir_;
    std::map<std::string, DatabaseInfo> databases_;
    std::recursive_mutex mutex_;
    std::set<std::string> attachedIds_;
    std::map<std::string, DatabaseInfo> detachedFiles_;
    std::unique_ptr<NativeConnections> connections_;
    bool IsReadOnly(const std::string& dbId);
    void SaveCatalog();

    void InitSamples();
    void SeedEcommerce(const std::string& path);
    void SeedDevStudio(const std::string& path);
    std::string GetDbPath(const std::string& dbId);
};

} // namespace native_app
