#pragma once
#include "Types.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>

namespace native_app {
class DatabaseProvider {
public:
    explicit DatabaseProvider(ConnectionProfile profile) : profile_(std::move(profile)) {}
    virtual ~DatabaseProvider() = default;
    virtual void Test(const std::atomic_bool* cancellation) = 0;
    virtual DatabaseSchema Schema(const std::atomic_bool* cancellation) = 0;
    virtual QueryResult Query(const std::string& query, int maximumRows, const std::atomic_bool* cancellation) = 0;
    virtual TableDataResult Table(const std::string& table, int page, int pageSize, const std::string& sort,
        const std::string& direction, const std::string& search, const std::string& filterColumn,
        const std::string& filterValue, const std::atomic_bool* cancellation) = 0;
    virtual bool Insert(const std::string& table, const TypedRow& values, const std::atomic_bool* cancellation) = 0;
    virtual bool Update(const std::string& table, const TypedRow& key, const TypedRow& values, const std::atomic_bool* cancellation) = 0;
    virtual bool Delete(const std::string& table, const TypedRow& key, const std::atomic_bool* cancellation) = 0;
    virtual std::string Export(const std::string& table, const std::string& format, const std::string& search,
        const std::string& sort, const std::string& direction, const std::atomic_bool* cancellation) = 0;
    const ConnectionProfile& Profile() const { return profile_; }
protected:
    ConnectionProfile profile_;
};

class NativeConnections {
public:
    explicit NativeConnections(const std::string& workspace);
    std::vector<DatabaseDriverInfo> Drivers() const;
    std::vector<DatabaseInfo> List() const;
    ConnectionProfile Profile(const std::string& id) const;
    bool Has(const std::string& id) const;
    std::string Test(const ConnectionProfile& profile, const std::atomic_bool* cancellation);
    DatabaseInfo Save(const ConnectionProfile& profile, bool connect, const std::atomic_bool* cancellation);
    bool Disconnect(const std::string& id);
    bool Remove(const std::string& id);
    std::shared_ptr<DatabaseProvider> Provider(const std::string& id);
private:
    std::string workspace_;
    mutable std::recursive_mutex mutex_;
    std::map<std::string, ConnectionProfile> profiles_, sessions_;
    std::map<std::string, std::shared_ptr<DatabaseProvider>> providers_;
    ConnectionProfile Normalize(ConnectionProfile profile, bool stored = false) const;
    void Write() const;
};

std::shared_ptr<DatabaseProvider> CreateOdbcProvider(const ConnectionProfile& profile);
std::shared_ptr<DatabaseProvider> CreateDuckDbProvider(const ConnectionProfile& profile);
std::shared_ptr<DatabaseProvider> CreateMongoProvider(const ConnectionProfile& profile);
std::shared_ptr<DatabaseProvider> CreateRedisProvider(const ConnectionProfile& profile);
std::vector<std::string> InstalledOdbcDrivers();
bool IncludeOdbcNamespace(const std::string& configuredSchema, const std::string& discoveredSchema);
DatabaseCapabilities ProviderCapabilities(const std::string& kind, bool readOnly, const std::string& dialect);
void RequireNotCancelled(const std::atomic_bool* cancellation);
void RequireReadOnlySql(const std::string& query);
std::vector<std::string> SqlStatements(const std::string& query);
std::string QuoteSqlIdentifier(const std::string& value, const std::string& dialect);
std::string ExportTypedRows(const std::vector<std::string>& columns, const std::vector<TypedRow>& rows,
    const std::string& format, const std::string& table = "", const std::string& dialect = "sqlite");
std::string ScrubConnectionError(const std::string& message, const ConnectionProfile& profile);
std::string DecodeConnectionComponent(const std::string& encoded);
} // namespace native_app
