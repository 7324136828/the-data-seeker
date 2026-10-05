#pragma once

#include "Types.h"
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace native_app {

class DbEngine {
public:
    explicit DbEngine(const std::string& dataDir, bool seedSamples = true);
    ~DbEngine() = default;

    std::vector<DatabaseInfo> ListDatabases();
    DatabaseInfo CreateDatabase(const std::string& name, const std::string& preset = "blank");
    bool DeleteDatabase(const std::string& dbId);

    DatabaseSchema GetSchema(const std::string& dbId);
    ERDiagram GetErDiagram(const std::string& dbId);

    QueryResult ExecuteQuery(const std::string& dbId, const std::string& sql, int maxRows = 1000);
    TableDataResult GetTableData(
        const std::string& dbId,
        const std::string& table,
        int page = 1,
        int pageSize = 25,
        const std::string& sortCol = "",
        const std::string& sortDir = "asc",
        const std::string& search = "",
        const std::string& filterCol = "",
        const std::string& filterVal = ""
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
        const std::string& sortDir = "asc"
    );

    std::string FormatQuery(const std::string& query, const std::string& kind = "sql");
    QueryResult ExplainQuery(const std::string& dbId, const std::string& query);

private:
    std::string dataDir_;
    std::map<std::string, DatabaseInfo> databases_;

    void InitSamples();
    void SeedEcommerce(const std::string& path);
    void SeedDevStudio(const std::string& path);
    std::string GetDbPath(const std::string& dbId);
};

} // namespace native_app
