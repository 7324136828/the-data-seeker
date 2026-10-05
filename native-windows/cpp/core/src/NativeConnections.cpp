#include "NativeConnections.h"
#include "DbValues.h"
#include "VariableResolver.h"
#include <windows.h>
#include <wincrypt.h>
#include <objbase.h>
#include <winsqlite/winsqlite3.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <stdexcept>
#include <functional>
#pragma comment(lib,"crypt32.lib")
#pragma comment(lib,"winsqlite3.lib")
namespace native_app {
namespace {
using json=nlohmann::json; namespace fs=std::filesystem;
std::wstring Wide(const std::string& value) { return fs::u8path(value).wstring(); }
std::string Lower(std::string value) { std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return static_cast<char>(tolower(c));});return value; }
std::string Protect(const std::string& value) {
    if(value.empty()) return value;
    DATA_BLOB input{static_cast<DWORD>(value.size()),reinterpret_cast<BYTE*>(const_cast<char*>(value.data()))},output{};
    if(!CryptProtectData(&input,L"DataForge connection",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output)) throw std::runtime_error("Windows could not protect connection settings.");
    DWORD bytes=0; CryptBinaryToStringA(output.pbData,output.cbData,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,nullptr,&bytes);
    std::string result(bytes,0); const BOOL valid=CryptBinaryToStringA(output.pbData,output.cbData,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,result.data(),&bytes);LocalFree(output.pbData);
    if(!valid) throw std::runtime_error("Windows could not encode connection settings.");
    result.resize(bytes);if(!result.empty()&&result.back()==0)result.pop_back();return "dpapi:v1:"+result;
}
std::string Unprotect(const std::string& value) {
    if(value.rfind("dpapi:v1:",0)!=0)return value;
    DWORD bytes=0; const auto encoded=value.substr(9);
    if(!CryptStringToBinaryA(encoded.c_str(),static_cast<DWORD>(encoded.size()),CRYPT_STRING_BASE64,nullptr,&bytes,nullptr,nullptr))throw std::runtime_error("Protected connection settings are damaged.");
    std::vector<BYTE> decoded(bytes);if(!CryptStringToBinaryA(encoded.c_str(),static_cast<DWORD>(encoded.size()),CRYPT_STRING_BASE64,decoded.data(),&bytes,nullptr,nullptr))throw std::runtime_error("Protected connection settings are damaged.");
    DATA_BLOB input{bytes,decoded.data()},output{};
    if(!CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output))throw std::runtime_error("Connection settings belong to another Windows account or are damaged.");
    std::string result(reinterpret_cast<const char*>(output.pbData),output.cbData);LocalFree(output.pbData);return result;
}
std::string Id() { GUID guid{};if(FAILED(CoCreateGuid(&guid)))throw std::runtime_error("Cannot allocate connection identifier.");wchar_t value[40]{};StringFromGUID2(guid,value,40);return "conn_"+fs::path(value).u8string(); }
std::string Environment(const std::string& reference) {
    if(reference.empty())return {};
    const auto name=Wide(reference);DWORD bytes=GetEnvironmentVariableW(name.c_str(),nullptr,0);
    if(!bytes)throw std::runtime_error("Set the configured connection environment variable before connecting.");
    std::wstring value(bytes,0);DWORD count=GetEnvironmentVariableW(name.c_str(),value.data(),bytes);if(!count||count>=bytes)throw std::runtime_error("Cannot read the configured connection environment variable.");
    value.resize(count);return fs::path(value).u8string();
}
ConnectionProfile Resolved(ConnectionProfile profile) {
    if(!profile.passwordEnv.empty())profile.password=Environment(profile.passwordEnv);
    if(!profile.urlEnv.empty())profile.url=Environment(profile.urlEnv);
    if(!profile.odbcEnv.empty())profile.odbcConnectionString=Environment(profile.odbcEnv);
    return profile;
}
std::shared_ptr<DatabaseProvider> Create(const ConnectionProfile& profile) {
    if(profile.type=="duckdb")return CreateDuckDbProvider(profile);
    if(profile.type=="mongodb")return CreateMongoProvider(profile);
    if(profile.type=="redis")return CreateRedisProvider(profile);
    if(profile.type=="sqlite")return nullptr;
    return CreateOdbcProvider(profile);
}
bool SensitiveConnectionKey(const std::string& key){const auto name=Lower(key);return VariableResolver::IsSensitiveKey(key)||name=="pwd"||name=="pass"||name.find("access_key")!=std::string::npos||name.find("accesskey")!=std::string::npos||name.find("private_key")!=std::string::npos||name.find("credential")!=std::string::npos;}
json SafeOptions(const json& options) { if(options.is_array()){json result=json::array();for(const auto& item:options)result.push_back(SafeOptions(item));return result;}if(options.is_object()){json result=json::object();for(auto item=options.begin();item!=options.end();++item)if(!SensitiveConnectionKey(item.key()))result[item.key()]=SafeOptions(item.value());return result;}return options; }
json JsonProfile(const ConnectionProfile& profile) {
    const bool keep=profile.persistCredentials;
    return {{"id",profile.id},{"name",profile.name},{"type",profile.type},{"host",profile.host},{"port",profile.port},{"database",profile.database},{"schema",profile.schema},{"path",profile.path},
        {"username",Protect(profile.username)},{"password",keep?Protect(profile.password):""},{"url",keep?Protect(profile.url):""},{"odbcConnectionString",keep?Protect(profile.odbcConnectionString):""},
        {"passwordEnv",profile.passwordEnv},{"urlEnv",profile.urlEnv},{"odbcEnv",profile.odbcEnv},{"driver",profile.driver},{"sslMode",profile.sslMode},{"sslCa",profile.sslCa},
        {"optionsJson",Protect(keep?profile.optionsJson:SafeOptions(json::parse(profile.optionsJson)).dump())},{"timeoutSec",profile.timeoutSec},{"readOnly",profile.readOnly},{"persistCredentials",profile.persistCredentials},{"sessionCredentials",profile.sessionCredentials}};
}
ConnectionProfile ParseProfile(const json& record) {
    ConnectionProfile p;
    p.id=record.at("id").get<std::string>();p.name=record.at("name").get<std::string>();p.type=record.at("type").get<std::string>();
    p.host=record.value("host","localhost");p.port=record.value("port",0);p.database=record.value("database","");p.schema=record.value("schema","");p.path=record.value("path","");
    p.username=Unprotect(record.value("username",""));p.password=Unprotect(record.value("password",""));p.url=Unprotect(record.value("url",""));p.odbcConnectionString=Unprotect(record.value("odbcConnectionString",""));
    p.passwordEnv=record.value("passwordEnv","");p.urlEnv=record.value("urlEnv","");p.odbcEnv=record.value("odbcEnv","");p.driver=record.value("driver","");p.sslMode=record.value("sslMode","verify-full");p.sslCa=record.value("sslCa","");
    p.optionsJson=Unprotect(record.value("optionsJson","{}"));p.timeoutSec=record.value("timeoutSec",10);p.readOnly=record.value("readOnly",false);p.persistCredentials=record.value("persistCredentials",false);p.sessionCredentials=record.value("sessionCredentials",false);return p;
}
void AtomicJson(const fs::path& target,const json& value) {
    GUID guid{};if(FAILED(CoCreateGuid(&guid)))throw std::runtime_error("Cannot allocate connection save file.");wchar_t suffix[40]{};StringFromGUID2(guid,suffix,40);
    const fs::path pending=target.wstring()+suffix+L".pending";
    const auto bytes=value.dump(2);if(bytes.size()>16*1024*1024)throw std::runtime_error("Connection profiles exceed the 16 MiB storage limit.");
    HANDLE file=CreateFileW(pending.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot save connection profiles.");
    DWORD written=0;bool valid=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file);CloseHandle(file);
    if(!valid||!MoveFileExW(pending.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(pending.c_str());throw std::runtime_error("Connection save failed. Previous profiles were preserved.");}
}
DatabaseInfo Info(const ConnectionProfile& p,bool connected) {
    DatabaseInfo info;info.id=p.id;info.name=p.name;info.path=p.path;info.type=p.type;info.dialect=p.type=="odbc"||p.type=="sqlalchemy"?"standard":p.type;
    info.kind=p.type=="mongodb"?"document":p.type=="redis"?"keyvalue":"sql";info.connected=connected;info.readOnly=p.readOnly;info.capabilities=ProviderCapabilities(info.kind,p.readOnly,info.dialect);return info;
}
void TestSqlite(const ConnectionProfile& p) {
    sqlite3* raw=nullptr;const int result=sqlite3_open_v2(p.path.c_str(),&raw,SQLITE_OPEN_READONLY,nullptr);std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,&sqlite3_close);
    if(result!=SQLITE_OK)throw std::runtime_error("Cannot open the selected SQLite database.");
    sqlite3_stmt* statement=nullptr;if(sqlite3_prepare_v2(db.get(),"SELECT name FROM sqlite_master LIMIT 1;",-1,&statement,nullptr)!=SQLITE_OK)throw std::runtime_error("The selected file is not a valid SQLite database.");sqlite3_finalize(statement);
}
}
void RequireNotCancelled(const std::atomic_bool* cancellation) { if(cancellation&&cancellation->load())throw std::runtime_error("Query cancelled."); }
std::string DecodeConnectionComponent(const std::string& encoded){std::string escaped;escaped.reserve(encoded.size());for(char byte:encoded){if(byte=='+')escaped+="%2B";else escaped+=byte;}return VariableResolver::UrlDecode(escaped);}
DatabaseCapabilities ProviderCapabilities(const std::string& kind,bool readOnly,const std::string& dialect) {
    DatabaseCapabilities c;c.crud=!readOnly;c.readOnly=readOnly;c.erDiagram=kind=="sql";c.sqlExport=kind=="sql";c.explain=kind=="sql"&&(dialect=="sqlite"||dialect=="duckdb"||dialect=="postgresql"||dialect=="mysql"||dialect=="mariadb");return c;
}
std::string ScrubConnectionError(const std::string& message,const ConnectionProfile& p) {
    std::string result=message;
    auto hide=[&](const std::string& secret){if(secret.empty())return;size_t position=0;while((position=result.find(secret,position))!=std::string::npos){result.replace(position,secret.size(),"[REDACTED]");position+=10;}};
    hide(p.password);hide(p.url);hide(p.odbcConnectionString);
    // Driver diagnostics sometimes echo one credential instead of the entire
    // input URL/string. Mask those decoded values as well as the original input.
    const auto scheme=p.url.find("://");if(scheme!=std::string::npos){const auto begin=scheme+3,end=p.url.find_first_of("/?#",begin);const auto authority=p.url.substr(begin,end==std::string::npos?std::string::npos:end-begin);const auto at=authority.rfind('@');if(at!=std::string::npos){const auto credentials=authority.substr(0,at);const auto colon=credentials.find(':');if(colon!=std::string::npos){const auto password=credentials.substr(colon+1);hide(password);hide(DecodeConnectionComponent(password));}}}
    const auto question=p.url.find('?');if(question!=std::string::npos){for(size_t position=question+1;position<p.url.size();){const auto end=p.url.find('&',position),finish=end==std::string::npos?p.url.size():end,equal=p.url.find('=',position);if(equal!=std::string::npos&&equal<finish&&SensitiveConnectionKey(DecodeConnectionComponent(p.url.substr(position,equal-position)))){const auto value=p.url.substr(equal+1,finish-equal-1);hide(value);hide(DecodeConnectionComponent(value));hide(VariableResolver::UrlDecode(value));}position=finish+1;}}
    for(size_t position=0;position<p.odbcConnectionString.size();){const auto equal=p.odbcConnectionString.find('=',position);if(equal==std::string::npos)break;auto key=p.odbcConnectionString.substr(position,equal-position);const auto begin=key.find_first_not_of(" ;\t\r\n"),end=key.find_last_not_of(" \t\r\n");key=begin==std::string::npos?"":key.substr(begin,end-begin+1);position=equal+1;while(position<p.odbcConnectionString.size()&&isspace(static_cast<unsigned char>(p.odbcConnectionString[position])))++position;std::string value;
        if(position<p.odbcConnectionString.size()&&p.odbcConnectionString[position]=='{'){++position;while(position<p.odbcConnectionString.size()){const char byte=p.odbcConnectionString[position++];if(byte=='}'){if(position<p.odbcConnectionString.size()&&p.odbcConnectionString[position]=='}'){value+='}';++position;continue;}break;}value+=byte;}const auto next=p.odbcConnectionString.find(';',position);position=next==std::string::npos?p.odbcConnectionString.size():next+1;}
        else{const auto next=p.odbcConnectionString.find(';',position);value=p.odbcConnectionString.substr(position,next==std::string::npos?std::string::npos:next-position);position=next==std::string::npos?p.odbcConnectionString.size():next+1;}if(SensitiveConnectionKey(key))hide(value);
    }
    try {std::function<void(const json&)> nested=[&](const json& value){if(value.is_object()){for(auto i=value.begin();i!=value.end();++i){if(SensitiveConnectionKey(i.key()))hide(i.value().is_string()?i.value().get<std::string>():i.value().dump());nested(i.value());}}else if(value.is_array())for(const auto& item:value)nested(item);};nested(json::parse(p.optionsJson));}catch(...){}
    return result.substr(0,1500);
}
std::vector<std::string> SqlStatements(const std::string& query) {
    std::vector<std::string> statements;size_t start=0;char quote=0;bool line=false,block=false;
    for(size_t i=0;i<query.size();++i){const char c=query[i],next=i+1<query.size()?query[i+1]:0;
        if(line){if(c=='\n')line=false;continue;}if(block){if(c=='*'&&next=='/'){block=false;++i;}continue;}
        if(quote){if(c==quote){if(next==quote)++i;else quote=0;}continue;}
        if(c=='-'&&next=='-'){line=true;++i;continue;}if(c=='/'&&next=='*'){block=true;++i;continue;}
        if(c=='\''||c=='"'||c=='`'||c=='['){quote=c=='['?']':c;continue;}
        if(c==';'){statements.push_back(query.substr(start,i-start));start=i+1;}
    }
    if(quote||block)throw std::runtime_error("SQL contains an unterminated string, identifier, or comment.");
    if(start<query.size())statements.push_back(query.substr(start));
    statements.erase(std::remove_if(statements.begin(),statements.end(),[](const auto& text){return text.find_first_not_of(" \t\r\n")==std::string::npos;}),statements.end());return statements;
}
void RequireReadOnlySql(const std::string& query) {
    const auto statements=SqlStatements(query);if(statements.empty())throw std::runtime_error("Enter a SELECT query.");
    static const std::set<std::string> forbidden={"INSERT","UPDATE","DELETE","REPLACE","MERGE","CREATE","DROP","ALTER","TRUNCATE","GRANT","REVOKE","CALL","EXEC","EXECUTE","COPY","PRAGMA","ATTACH","DETACH","VACUUM","INTO","FOR","LOCK","SET","DO"};
    for(const auto& statement:statements){std::vector<std::string> words;char quote=0;bool line=false,block=false;
        for(size_t i=0;i<statement.size();){char c=statement[i],next=i+1<statement.size()?statement[i+1]:0;
            if(line){if(c=='\n')line=false;++i;continue;}if(block){if(c=='*'&&next=='/'){block=false;i+=2;}else ++i;continue;}
            if(quote){if(c==quote){if(next==quote){i+=2;continue;}quote=0;}++i;continue;}
            if(c=='-'&&next=='-'){line=true;i+=2;continue;}if(c=='/'&&next=='*'){block=true;i+=2;continue;}
            if(c=='\''||c=='"'||c=='`'||c=='['){quote=c=='['?']':c;++i;continue;}
            if(isalpha(static_cast<unsigned char>(c))||c=='_'){std::string word;while(i<statement.size()&&(isalnum(static_cast<unsigned char>(statement[i]))||statement[i]=='_'))word+=static_cast<char>(toupper(static_cast<unsigned char>(statement[i++])));words.push_back(word);}else ++i;
        }
        if(words.empty())continue;const bool explain=words.front()=="EXPLAIN"&&std::find(words.begin(),words.end(),"SELECT")!=words.end();if(words.front()!="SELECT"&&words.front()!="WITH"&&!explain)throw std::runtime_error("This connection is read-only; only SELECT queries are allowed.");
        for(const auto& word:words)if(forbidden.count(word))throw std::runtime_error("This connection is read-only; the SQL contains a mutation or transaction command.");
    }
}
std::string QuoteSqlIdentifier(const std::string& value,const std::string& dialect) {
    if(value.empty()||value.find('\0')!=std::string::npos)throw std::runtime_error("Database identifiers cannot be empty or contain NUL.");
    const char start=dialect=="mysql"||dialect=="mariadb"?'`':dialect=="mssql"?'[':'"',end=start=='['?']':start;
    std::string result(1,start);for(char c:value){if(c==end)result+=end;result+=c;}return result+end;
}
std::string ExportTypedRows(const std::vector<std::string>& columns,const std::vector<TypedRow>& rows,const std::string& format,const std::string& table,const std::string& dialect) {
    if(format!="json"&&format!="csv"&&format!="sql")throw std::runtime_error("Supported exports are CSV, JSON, and SQL.");
    json document=json::array();std::ostringstream output;
    auto csv=[](const std::string& value){std::string result="\"";for(char c:value){if(c=='"')result+='"';result+=c;}return result+"\"";};
    if(format=="csv"){for(size_t i=0;i<columns.size();++i){if(i)output<<',';output<<csv(columns[i]);}output<<"\r\n";}
    for(const auto& row:rows){if(format=="json"){json item=json::object();for(const auto& column:columns){auto found=row.find(column);item[column]=found==row.end()?json():DbValueJson(found->second);}document.push_back(std::move(item));continue;}
        if(format=="sql"){output<<"INSERT INTO "<<QuoteSqlIdentifier(table,dialect)<<" (";for(size_t i=0;i<columns.size();++i){if(i)output<<", ";output<<QuoteSqlIdentifier(columns[i],dialect);}output<<") VALUES (";}
        for(size_t i=0;i<columns.size();++i){if(i)output<<(format=="csv"?",":", ");auto found=row.find(columns[i]);const DbValue value=found==row.end()?DbValue{DbValueType::Null,""}:found->second;
            if(format=="csv")output<<csv(value.type==DbValueType::Null?"":DbValueDisplay(value));else if(value.type==DbValueType::Null)output<<"NULL";else if(value.type==DbValueType::Integer||value.type==DbValueType::Real)output<<DbValueJson(value).dump();else if(value.type==DbValueType::Boolean)output<<(DbValueJson(value).get<bool>()?1:0);else if(value.type==DbValueType::Blob)output<<"X'"<<BlobHex(value.text)<<'\'';else{output<<'\'';for(char c:value.text){if(c=='\'')output<<'\'';output<<c;}output<<'\'';}}
        output<<(format=="sql"?");\r\n":"\r\n");}
    return format=="json"?document.dump(2):output.str();
}
NativeConnections::NativeConnections(const std::string& workspace):workspace_(workspace) {
    const auto path=fs::u8path(workspace_)/"connections.json";if(!fs::exists(path))return;
    try{if(fs::file_size(path)>16*1024*1024)throw std::runtime_error("Profiles too large");std::ifstream file(path);const auto data=json::parse(file);if(!data.is_array())throw std::runtime_error("Invalid profile list");for(const auto& item:data){auto profile=Normalize(ParseProfile(item),true);if(profiles_.count(profile.id))throw std::runtime_error("Duplicate profile id");profiles_[profile.id]=std::move(profile);}}
    catch(...){throw std::runtime_error("Cannot read connections.json. Restore a valid backup; the existing file was preserved.");}
}
std::vector<DatabaseDriverInfo> NativeConnections::Drivers() const {
    const auto installed=InstalledOdbcDrivers();std::vector<DatabaseDriverInfo> result;
    auto add=[&](std::string id,std::string name,std::string kind,std::string mode,int port){DatabaseDriverInfo info;info.id=id;info.name=name;info.kind=kind;info.dialect=id;info.mode=mode;info.defaultPort=port;for(const auto& driver:installed){const auto text=Lower(driver);if(id=="odbc"||id=="sqlalchemy"||text.find(id)!=std::string::npos||(id=="postgresql"&&text.find("postgres")!=std::string::npos)||(id=="mssql"&&text.find("sql server")!=std::string::npos))info.installedDrivers.push_back(driver);}info.available=id=="sqlite"||id=="duckdb"||id=="mongodb"||id=="redis"||!info.installedDrivers.empty();info.installHint=info.available?"":"Install a matching x64 ODBC driver and select it or a DSN.";result.push_back(std::move(info));};
    add("sqlite","SQLite","sql","file",0);add("duckdb","DuckDB","sql","file",0);add("postgresql","PostgreSQL / compatible services","sql","network",5432);add("mysql","MySQL / compatible services","sql","network",3306);add("mariadb","MariaDB","sql","network",3306);add("mssql","SQL Server / Azure SQL","sql","network",1433);add("oracle","Oracle","sql","network",1521);add("mongodb","MongoDB / Atlas","document","network",27017);add("redis","Redis / compatible services","keyvalue","network",6379);
    for(const auto& type:std::vector<std::pair<std::string,std::string>>{{"odbc","Generic ODBC data source"},{"snowflake","Snowflake"},{"bigquery","Google BigQuery"},{"clickhouse","ClickHouse"},{"databricks","Databricks"},{"redshift","Amazon Redshift"},{"trino","Trino / Presto"},{"sqlalchemy","Other installed native ODBC driver"}})add(type.first,type.second,"sql","odbc",0);
    return result;
}
ConnectionProfile NativeConnections::Normalize(ConnectionProfile profile, bool stored) const {
    const auto drivers=Drivers();auto driver=std::find_if(drivers.begin(),drivers.end(),[&](const auto& item){return item.id==profile.type;});if(driver==drivers.end())throw std::runtime_error("Select a supported database driver.");
    const auto begin=profile.name.find_first_not_of(" \t\r\n"),end=profile.name.find_last_not_of(" \t\r\n");if(begin==std::string::npos)throw std::runtime_error("Connection name cannot be empty.");profile.name=profile.name.substr(begin,end-begin+1);if(profile.id.empty())profile.id=Id();
    if(profile.timeoutSec<1||profile.timeoutSec>60)throw std::runtime_error("Connection timeout must be between 1 and 60 seconds.");
    if(profile.sslMode!="verify-full"&&profile.sslMode!="disable")throw std::runtime_error("TLS mode must be verify-full or disable.");
    if(profile.port==0)profile.port=driver->defaultPort;if(driver->defaultPort&& (profile.port<1||profile.port>65535))throw std::runtime_error("Connection port must be between 1 and 65535.");
    if(!json::parse(profile.optionsJson.empty()?"{}":profile.optionsJson).is_object())throw std::runtime_error("Driver options must be a JSON object.");
    if(driver->mode=="file"){if(profile.path.empty())throw std::runtime_error("Specify a database file path.");auto path=fs::u8path(profile.path);if(!path.is_absolute())path=fs::u8path(workspace_)/path;path=fs::absolute(path).lexically_normal();if(!stored&&!fs::is_directory(path.parent_path()))throw std::runtime_error("The database file's parent folder must already exist.");if(!stored&&fs::exists(path)&&!fs::is_regular_file(path))throw std::runtime_error("The database path is not a file.");if(!stored&&!fs::is_regular_file(path)&&!profile.create)throw std::runtime_error("Database file does not exist. Select Create to create it when saving.");profile.path=path.u8string();}
    if(!stored&&driver->mode=="odbc"&&profile.odbcConnectionString.empty()&&profile.odbcEnv.empty()&&profile.url.empty()&&profile.urlEnv.empty())throw std::runtime_error("Provide an ODBC connection string, URL, or environment-variable reference.");
    const auto options=json::parse(profile.optionsJson.empty()?"{}":profile.optionsJson);profile.optionsJson=options.dump();profile.sessionCredentials=!profile.persistCredentials&&(profile.sessionCredentials||(!profile.password.empty()&&profile.passwordEnv.empty())||(!profile.url.empty()&&profile.urlEnv.empty())||(!profile.odbcConnectionString.empty()&&profile.odbcEnv.empty())||options!=SafeOptions(options));return profile;
}
void NativeConnections::Write() const {json data=json::array();for(const auto& pair:profiles_)data.push_back(JsonProfile(pair.second));AtomicJson(fs::u8path(workspace_)/"connections.json",data);}
bool NativeConnections::Has(const std::string& id) const {std::lock_guard<std::recursive_mutex> lock(mutex_);return profiles_.count(id)!=0;}
ConnectionProfile NativeConnections::Profile(const std::string& id) const {std::lock_guard<std::recursive_mutex> lock(mutex_);auto session=sessions_.find(id);if(session!=sessions_.end())return session->second;auto found=profiles_.find(id);if(found==profiles_.end())throw std::runtime_error("Connection not found.");return found->second;}
std::vector<DatabaseInfo> NativeConnections::List() const {std::lock_guard<std::recursive_mutex> lock(mutex_);std::vector<DatabaseInfo> result;for(const auto& pair:profiles_)result.push_back(Info(pair.second,providers_.count(pair.first)!=0));return result;}
std::string NativeConnections::Test(const ConnectionProfile& input,const std::atomic_bool* cancellation) {
    RequireNotCancelled(cancellation);const auto p=Normalize(input);
    if((p.type=="sqlite"||p.type=="duckdb")&&!fs::exists(fs::u8path(p.path)))return "File location is valid. Saving will create the database; Test has not created a file.";
    const auto resolved=Resolved(p);try{if(p.type=="sqlite")TestSqlite(resolved);else Create(resolved)->Test(cancellation);RequireNotCancelled(cancellation);return "Connection test succeeded.";}catch(const std::exception& error){throw std::runtime_error(ScrubConnectionError(error.what(),resolved));}
}
DatabaseInfo NativeConnections::Save(const ConnectionProfile& input,bool connect,const std::atomic_bool* cancellation) {
    RequireNotCancelled(cancellation);auto profile=Normalize(input);auto resolved=profile;std::shared_ptr<DatabaseProvider> provider;
    bool created=false;struct Reservation{HANDLE file=INVALID_HANDLE_VALUE;~Reservation(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}void Close(){if(file!=INVALID_HANDLE_VALUE){CloseHandle(file);file=INVALID_HANDLE_VALUE;}}} reservation;
    // DuckDB rejects empty files and takes an exclusive database lock. Create a
    // complete database in our own sibling directory before publishing it;
    // MoveFileEx without REPLACE_EXISTING refuses a destination created meanwhile.
    struct Staging {fs::path directory;~Staging(){if(directory.empty())return;std::error_code ignored;fs::remove(directory/"database.duckdb",ignored);fs::remove(directory/"database.duckdb.wal",ignored);fs::remove(directory,ignored);}} staging;
    try{
        if((profile.type=="sqlite"||profile.type=="duckdb")&&profile.create&&!fs::exists(fs::u8path(profile.path))){
            if(profile.type=="duckdb"){
                const auto parent=fs::u8path(profile.path).parent_path();staging.directory=parent/fs::u8path(".dataforge-create-"+Id());
                if(!CreateDirectoryW(staging.directory.c_str(),nullptr)){staging.directory.clear();throw std::runtime_error("Cannot create a private database staging directory.");}
                auto temporary=resolved;temporary.path=(staging.directory/"database.duckdb").u8string();temporary.readOnly=false;
                CreateDuckDbProvider(temporary)->Test(cancellation);RequireNotCancelled(cancellation);
                if(!MoveFileExW(fs::u8path(temporary.path).c_str(),fs::u8path(profile.path).c_str(),MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot publish the database file; existing files are never overwritten.");
                created=true;
            }else{
                reservation.file=CreateFileW(fs::u8path(profile.path).c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(reservation.file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create the database file; existing files are never overwritten.");created=true;reservation.Close();
                sqlite3* db=nullptr;const int rc=sqlite3_open(profile.path.c_str(),&db);if(db)sqlite3_close(db);if(rc!=SQLITE_OK)throw std::runtime_error("Cannot initialize SQLite file.");
            }
        }
        if(connect||created){resolved=Resolved(profile);if(profile.type=="sqlite")TestSqlite(resolved);else{provider=Create(resolved);provider->Test(cancellation);if(!connect)provider.reset();}}RequireNotCancelled(cancellation);
        std::lock_guard<std::recursive_mutex> lock(mutex_);auto previous=profiles_;auto previousSessions=sessions_;auto previousProviders=providers_;profile.create=false;profiles_[profile.id]=profile;sessions_[profile.id]=profile;if(provider||connect)providers_[profile.id]=provider;else providers_.erase(profile.id);
        try{Write();}catch(...){profiles_=std::move(previous);sessions_=std::move(previousSessions);providers_=std::move(previousProviders);throw;}}
    catch(const std::exception& error){reservation.Close();if(created){std::error_code ignored;fs::remove(fs::u8path(profile.path),ignored);}throw std::runtime_error(ScrubConnectionError(error.what(),resolved));}
    return Info(profile,connect);
}
std::shared_ptr<DatabaseProvider> NativeConnections::Provider(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);auto existing=providers_.find(id);if(existing!=providers_.end())return existing->second;
    auto found=profiles_.find(id);if(found==profiles_.end())return nullptr;if(found->second.type=="sqlite"){providers_[id]=nullptr;return nullptr;}
    const auto session=sessions_.find(id);if(found->second.sessionCredentials&&session==sessions_.end())throw std::runtime_error("Re-enter this connection's session credentials or use environment-variable references.");
    const auto resolved=Resolved(session==sessions_.end()?found->second:session->second);
    try{auto provider=Create(resolved);providers_[id]=provider;return provider;}catch(const std::exception& error){throw std::runtime_error(ScrubConnectionError(error.what(),resolved));}
}
bool NativeConnections::Disconnect(const std::string& id) {std::lock_guard<std::recursive_mutex> lock(mutex_);providers_.erase(id);return profiles_.count(id)!=0;}
bool NativeConnections::Remove(const std::string& id) {std::lock_guard<std::recursive_mutex> lock(mutex_);if(!profiles_.count(id))return false;auto previous=profiles_;profiles_.erase(id);try{Write();}catch(...){profiles_=std::move(previous);throw;}providers_.erase(id);sessions_.erase(id);return true;}
} // namespace native_app
