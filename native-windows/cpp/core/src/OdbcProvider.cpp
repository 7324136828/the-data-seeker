#include "SqlProvider.h"
#include "VariableResolver.h"
#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include <regex>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <set>
#include <limits>
#pragma comment(lib,"odbc32.lib")
namespace native_app {
bool IncludeOdbcNamespace(const std::string& configuredSchema,const std::string& discoveredSchema) {
    if(configuredSchema.empty()&&!discoveredSchema.empty())throw std::runtime_error("Set an explicit schema in this connection before browsing, exporting, editing rows, or exposing its mock API. The SQL console remains available.");
    return configuredSchema.empty()||configuredSchema==discoveredSchema;
}
bool IncludeOdbcCatalog(const std::string& currentCatalog,const std::string& discoveredCatalog) {
    if(discoveredCatalog.empty())return true;
    if(currentCatalog.empty())throw std::runtime_error("Select an explicit database in this connection: the ODBC driver did not identify its current catalog. The SQL console remains available.");
    return currentCatalog==discoveredCatalog;
}
std::string OdbcViewDefinitionQuery(const std::string& dialect) {
    if(dialect=="postgresql")return "SELECT definition FROM pg_catalog.pg_views WHERE schemaname = ? AND viewname = ?";
    if(dialect=="mysql"||dialect=="mariadb"||dialect=="snowflake")return "SELECT VIEW_DEFINITION FROM INFORMATION_SCHEMA.VIEWS WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ?";
    if(dialect=="mssql")return "SELECT m.definition FROM sys.sql_modules m JOIN sys.views v ON m.object_id = v.object_id JOIN sys.schemas s ON v.schema_id = s.schema_id WHERE s.name = ? AND v.name = ?";
    if(dialect=="oracle")return "SELECT TEXT FROM ALL_VIEWS WHERE OWNER = ? AND VIEW_NAME = ?";
    return {};
}
std::string OdbcColumnType(const std::map<std::string,std::string>& row,const std::string& dialect) {
    const auto name=row.at("TYPE_NAME");if(name.find('(')!=std::string::npos)return name;
    auto normalized=name;std::transform(normalized.begin(),normalized.end(),normalized.begin(),[](unsigned char c){return static_cast<char>(tolower(c));});
    static const std::set<std::string> sizedTypes{"char","character","varchar","character varying","nchar","nvarchar","national character","national character varying","varchar2","nvarchar2","binary","varbinary","binary varying"};
    static const std::set<std::string> decimalTypes{"decimal","dec","numeric","number"};
    try {
        const auto type=std::stoi(row.at("DATA_TYPE"));const auto size=std::stoll(row.at("COLUMN_SIZE"));
        if((type==SQL_DECIMAL||type==SQL_NUMERIC)&&decimalTypes.count(normalized)) {
            const auto scale=std::stoi(row.at("DECIMAL_DIGITS"));
            if(size>0&&scale>=0&&scale<=size)return name+"("+std::to_string(size)+","+std::to_string(scale)+")";
        }
        if((type==SQL_CHAR||type==SQL_VARCHAR||type==SQL_WCHAR||type==SQL_WVARCHAR||type==SQL_BINARY||type==SQL_VARBINARY)&&sizedTypes.count(normalized)) {
            if(dialect=="mssql"&&(type==SQL_VARCHAR||type==SQL_WVARCHAR||type==SQL_VARBINARY)&&size==0)return name+"(MAX)";
            if(size>0)return name+"("+std::to_string(size)+")";
        }
    } catch(const std::exception&) {}
    return name;
}
namespace {
std::string ForeignKeyAction(const std::map<std::string,std::string>& row,const std::string& field) {
    const auto found=row.find(field);if(found==row.end()||found->second=="NULL")return {};
    switch(std::stoi(found->second)){case SQL_CASCADE:return "CASCADE";case SQL_RESTRICT:return "RESTRICT";case SQL_SET_NULL:return "SET NULL";case SQL_NO_ACTION:return "NO ACTION";case SQL_SET_DEFAULT:return "SET DEFAULT";default:return {};}
}
}
std::string BuildOdbcSchemaDdl(const TableMeta& table,const std::string& dialect,const std::string& schema,
    const std::vector<std::map<std::string,std::string>>& foreignRows,const std::string& viewDefinition) {
    if(table.type=="view")return viewDefinition.empty()?"-- View definition unavailable: the server or driver does not expose it, or the account lacks metadata permission.":viewDefinition;
    if(table.columns.empty())return "-- Table definition unavailable: column metadata was not returned by the driver.";
    auto quote=[&](const std::string& value){return QuoteSqlIdentifier(value,dialect);};
    const auto qualified=schema.empty()?quote(table.name):quote(schema)+"."+quote(table.name);
    std::ostringstream ddl;ddl<<"CREATE TABLE "<<qualified<<" (\n";
    for(size_t i=0;i<table.columns.size();++i){const auto& column=table.columns[i];if(i)ddl<<",\n";ddl<<"  "<<quote(column.name)<<' '<<column.type;if(column.notnull)ddl<<" NOT NULL";if(!column.dfltValue.empty()&&column.dfltValue!="TRUNCATED")ddl<<" DEFAULT "<<column.dfltValue;}
    if(!table.primaryKeys.empty()){ddl<<",\n  PRIMARY KEY (";for(size_t i=0;i<table.primaryKeys.size();++i){if(i)ddl<<", ";ddl<<quote(table.primaryKeys[i]);}ddl<<')';}
    std::map<std::string,std::map<int,std::map<std::string,std::string>>> groups;int anonymous=0;bool incomplete=false;
    for(const auto& row:foreignRows){
        try {const int sequence=std::stoi(row.at("KEY_SEQ"));auto name=row.count("FK_NAME")&&row.at("FK_NAME")!="NULL"?row.at("FK_NAME"):"";if(name.empty()){if(sequence==1)++anonymous;name="anonymous:"+std::to_string(anonymous);}if(sequence<1||groups[name].count(sequence)){incomplete=true;continue;}groups[name][sequence]=row;}
        catch(const std::exception&){incomplete=true;}
    }
    for(const auto& group:groups){
        try {
            const auto& keys=group.second;const auto& first=keys.at(1);std::string source,target;int sequence=1;
            const auto targetName=first.at("PKTABLE_NAME");
            const auto targetSchema=first.count("PKTABLE_SCHEM")&&first.at("PKTABLE_SCHEM")!="NULL"?first.at("PKTABLE_SCHEM"):"";
            const auto targetCatalog=first.count("PKTABLE_CAT")&&first.at("PKTABLE_CAT")!="NULL"?first.at("PKTABLE_CAT"):"";
            for(const auto& key:keys){const auto& row=key.second;const auto same=[&](const std::string& field){const auto a=row.find(field),b=first.find(field);return (a==row.end()?"NULL":a->second)==(b==first.end()?"NULL":b->second);};if(key.first!=sequence++||row.at("PKTABLE_NAME")!=targetName||!same("PKTABLE_SCHEM")||!same("PKTABLE_CAT")||!same("UPDATE_RULE")||!same("DELETE_RULE"))throw std::runtime_error("Incomplete foreign key.");if(!source.empty()){source+=", ";target+=", ";}source+=quote(row.at("FKCOLUMN_NAME"));target+=quote(row.at("PKCOLUMN_NAME"));}
            const auto referenceSchema=targetSchema.empty()&&(dialect=="mysql"||dialect=="mariadb")?targetCatalog:targetSchema;
            std::ostringstream clause;if(first.count("FK_NAME")&&first.at("FK_NAME")!="NULL"&&!first.at("FK_NAME").empty())clause<<"CONSTRAINT "<<quote(first.at("FK_NAME"))<<' ';
            clause<<"FOREIGN KEY ("<<source<<") REFERENCES "<<(referenceSchema.empty()?"":quote(referenceSchema)+".")<<quote(targetName)<<" ("<<target<<')';
            auto update=ForeignKeyAction(first,"UPDATE_RULE"),remove=ForeignKeyAction(first,"DELETE_RULE");
            if(dialect=="mssql"){if(update=="RESTRICT")update="NO ACTION";if(remove=="RESTRICT")remove="NO ACTION";}
            if(!update.empty()&&dialect!="oracle")clause<<" ON UPDATE "<<update;
            if(!remove.empty()&&(dialect!="oracle"||remove=="CASCADE"||remove=="SET NULL"))clause<<" ON DELETE "<<remove;
            ddl<<",\n  "<<clause.str();
        }catch(const std::exception&){incomplete=true;}
    }
    ddl<<"\n);\n-- Reconstructed from available ODBC column, primary-key, and foreign-key metadata.\n-- Indexes are listed separately; vendor-specific table options may be unavailable.";
    if(incomplete)ddl<<"\n-- An incomplete foreign-key definition was omitted.";
    if(std::any_of(table.columns.begin(),table.columns.end(),[](const auto& column){return column.dfltValue=="TRUNCATED";}))ddl<<"\n-- A default expression truncated by the driver was omitted.";
    return ddl.str();
}
namespace {
std::wstring Wide(const std::string& value) { if(value.empty())return {};int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);if(!count)throw std::runtime_error("Database text contains invalid UTF-8.");std::wstring result(count,0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count);return result; }
std::string Utf8(const std::wstring& value) {if(value.empty())return {};int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);if(!count)throw std::runtime_error("Database returned invalid UTF-16.");std::string result(count,0);WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count,nullptr,nullptr);return result;}
std::string Lower(std::string value){std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return static_cast<char>(tolower(c));});return value;}
std::string Diagnostic(SQLSMALLINT type,SQLHANDLE handle){std::string result;for(SQLSMALLINT i=1;i<16;++i){SQLWCHAR state[6]{},message[2048]{};SQLINTEGER native=0;SQLSMALLINT length=0;auto rc=SQLGetDiagRecW(type,handle,i,state,&native,message,2048,&length);if(rc==SQL_NO_DATA)break;if(!SQL_SUCCEEDED(rc))break;if(!result.empty())result+='\n';result+=Utf8(std::wstring(reinterpret_cast<wchar_t*>(message),std::min<int>(length,2047)));}return result.empty()?"The ODBC driver reported an error.":result;}
void Check(SQLRETURN result,SQLSMALLINT type,SQLHANDLE handle,const std::atomic_bool* cancellation=nullptr){RequireNotCancelled(cancellation);if(!SQL_SUCCEEDED(result)&&result!=SQL_NO_DATA)throw std::runtime_error(Diagnostic(type,handle));}
struct OdbcHandle {SQLSMALLINT type;SQLHANDLE value=SQL_NULL_HANDLE;explicit OdbcHandle(SQLSMALLINT t,SQLHANDLE parent=SQL_NULL_HANDLE):type(t){if(!SQL_SUCCEEDED(SQLAllocHandle(type,parent,&value)))throw std::runtime_error("Cannot allocate a native ODBC handle.");}~OdbcHandle(){if(value)SQLFreeHandle(type,value);}OdbcHandle(const OdbcHandle&)=delete;};
class CancelGuard {
public:
    CancelGuard(SQLSMALLINT type,SQLHANDLE handle,const std::atomic_bool* cancellation):type_(type),handle_(handle){if(cancellation)worker_=std::thread([this,cancellation]{std::unique_lock<std::mutex> lock(mutex_);while(!done_){if(cancellation->load()){SQLCancelHandle(type_,handle_);break;}changed_.wait_for(lock,std::chrono::milliseconds(20));}});}
    ~CancelGuard(){{std::lock_guard<std::mutex> lock(mutex_);done_=true;}changed_.notify_all();if(worker_.joinable())worker_.join();}
private:SQLSMALLINT type_;SQLHANDLE handle_;std::mutex mutex_;std::condition_variable changed_;bool done_=false;std::thread worker_;
};
std::string Option(const std::string& value){std::string result="{";for(char c:value){if(c=='\0')throw std::runtime_error("ODBC values cannot contain NUL.");if(c=='}')result+='}';result+=c;}return result+'}';}
ConnectionProfile UrlProfile(ConnectionProfile profile){if(profile.url.empty())return profile;std::smatch match;static const std::regex url(R"(^([A-Za-z0-9+_.-]+)://([^/?#]+)(/[^?#]*)?(?:\?([^#]*))?$)");if(!std::regex_match(profile.url,match,url))throw std::runtime_error("Use a native ODBC connection string or a valid database connection URL.");std::string authority=match[2];auto at=authority.rfind('@');if(at!=std::string::npos){const auto credentials=authority.substr(0,at);authority.erase(0,at+1);const auto colon=credentials.find(':');profile.username=DecodeConnectionComponent(credentials.substr(0,colon));if(colon!=std::string::npos)profile.password=DecodeConnectionComponent(credentials.substr(colon+1));}auto colon=authority.rfind(':');if(colon!=std::string::npos&&authority.find(']')==std::string::npos){profile.host=authority.substr(0,colon);profile.port=std::stoi(authority.substr(colon+1));}else profile.host=authority;if(match[3].matched&&match[3].length()>1)profile.database=DecodeConnectionComponent(match[3].str().substr(1));return profile;}
std::string DriverFor(const ConnectionProfile& p,const std::vector<std::string>& drivers){if(!p.driver.empty()){if(std::find(drivers.begin(),drivers.end(),p.driver)==drivers.end())throw std::runtime_error("The selected x64 ODBC driver is not installed.");return p.driver;}std::vector<std::string> matches;for(const auto& driver:drivers){const auto name=Lower(driver);const auto type=p.type;if((type=="mssql"&&name.find("sql server")!=std::string::npos)||(type=="postgresql"&&name.find("postgres")!=std::string::npos)||(type=="mysql"&&name.find("mysql")!=std::string::npos)||(type=="mariadb"&&name.find("mariadb")!=std::string::npos)||(type=="oracle"&&name.find("oracle")!=std::string::npos)||(type=="redshift"&&name.find("redshift")!=std::string::npos)||(name.find(type)!=std::string::npos&&type!="odbc"&&type!="sqlalchemy"))matches.push_back(driver);}if(matches.empty())throw std::runtime_error("Install a matching x64 ODBC driver, or provide a native DSN connection string.");return matches.back();}
std::string ConnectionString(const ConnectionProfile& original){if(!original.odbcConnectionString.empty())return original.odbcConnectionString;const auto p=UrlProfile(original);const auto driver=DriverFor(p,InstalledOdbcDrivers());std::string result="DRIVER="+Option(driver)+";";auto add=[&](const std::string& key,const std::string& value){if(!value.empty())result+=key+'='+Option(value)+';';};
    if(p.type=="mssql"){add("SERVER",p.host+(p.port?","+std::to_string(p.port):""));add("DATABASE",p.database);add("UID",p.username);add("PWD",p.password);add("Encrypt",p.sslMode=="disable"?"No":"Yes");add("TrustServerCertificate","No");if(p.username.empty())add("Trusted_Connection","Yes");}
    else{add("SERVER",p.host);if(p.port)add("PORT",std::to_string(p.port));add(p.type=="oracle"?"DBQ":"DATABASE",p.database);add("UID",p.username);add("PWD",p.password);if(p.type=="postgresql"||p.type=="redshift"){add("SSLmode",p.sslMode=="disable"?"disable":"verify-full");add("SSLrootcert",p.sslCa);}else if(p.type=="mysql"){add("SSLMODE",p.sslMode=="disable"?"DISABLED":"VERIFY_IDENTITY");add("SSLCA",p.sslCa);}else if(p.type=="mariadb"){add("SSLVERIFY",p.sslMode=="disable"?"0":"1");add("SSLCA",p.sslCa);}}
    const auto options=nlohmann::json::parse(p.optionsJson);for(auto item=options.begin();item!=options.end();++item){if(item.key().empty()||std::any_of(item.key().begin(),item.key().end(),[](unsigned char c){return !isalnum(c)&&c!='_';}))throw std::runtime_error("ODBC option keys may contain only letters, numbers, and underscores.");if(!item.value().is_primitive())throw std::runtime_error("ODBC options must be scalar values.");add(item.key(),item.value().is_string()?item.value().get<std::string>():item.value().dump());}return result;}
class Session {
public:
    OdbcHandle environment{SQL_HANDLE_ENV};OdbcHandle connection;
    Session(const ConnectionProfile& profile,const std::atomic_bool* cancellation):connection(SQL_HANDLE_DBC,PrepareEnvironment(environment)){
        RequireNotCancelled(cancellation);Check(SQLSetConnectAttrW(connection.value,SQL_LOGIN_TIMEOUT,reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(profile.timeoutSec)),0),SQL_HANDLE_DBC,connection.value,cancellation);
        if(profile.readOnly)Check(SQLSetConnectAttrW(connection.value,SQL_ATTR_ACCESS_MODE,reinterpret_cast<SQLPOINTER>(SQL_MODE_READ_ONLY),0),SQL_HANDLE_DBC,connection.value,cancellation);
        const auto text=Wide(ConnectionString(profile));CancelGuard guard(SQL_HANDLE_DBC,connection.value,cancellation);SQLWCHAR output[2048]{};SQLSMALLINT length=0;
        Check(SQLDriverConnectW(connection.value,nullptr,reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(text.c_str())),SQL_NTS,output,2048,&length,SQL_DRIVER_NOPROMPT),SQL_HANDLE_DBC,connection.value,cancellation);
    }
    ~Session(){SQLDisconnect(connection.value);}
private:
    static SQLHANDLE PrepareEnvironment(OdbcHandle& environment){Check(SQLSetEnvAttr(environment.value,SQL_ATTR_ODBC_VERSION,reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3_80),0),SQL_HANDLE_ENV,environment.value);return environment.value;}
};
DbValue ReadValue(SQLHSTMT statement,SQLUSMALLINT column,SQLSMALLINT type,const std::atomic_bool* cancellation){SQLLEN length=0;if(type==SQL_INTEGER||type==SQL_SMALLINT||type==SQL_TINYINT||type==SQL_BIGINT){SQLBIGINT value=0;Check(SQLGetData(statement,column,SQL_C_SBIGINT,&value,sizeof(value),&length),SQL_HANDLE_STMT,statement,cancellation);return length==SQL_NULL_DATA?DbValue{DbValueType::Null,""}:DbValue{DbValueType::Integer,std::to_string(value)};}if(type==SQL_REAL||type==SQL_FLOAT||type==SQL_DOUBLE){double value=0;Check(SQLGetData(statement,column,SQL_C_DOUBLE,&value,sizeof(value),&length),SQL_HANDLE_STMT,statement,cancellation);std::ostringstream text;text<<std::setprecision(17)<<value;return length==SQL_NULL_DATA?DbValue{DbValueType::Null,""}:DbValue{DbValueType::Real,text.str()};}if(type==SQL_BIT){unsigned char value=0;Check(SQLGetData(statement,column,SQL_C_BIT,&value,sizeof(value),&length),SQL_HANDLE_STMT,statement,cancellation);return length==SQL_NULL_DATA?DbValue{DbValueType::Null,""}:DbValue{DbValueType::Boolean,value?"true":"false"};}
    const bool binary=type==SQL_BINARY||type==SQL_VARBINARY||type==SQL_LONGVARBINARY;std::string bytes;std::wstring text;
    for(;;){RequireNotCancelled(cancellation);alignas(wchar_t) char buffer[8192]{};const auto rc=SQLGetData(statement,column,binary?SQL_C_BINARY:SQL_C_WCHAR,buffer,sizeof(buffer),&length);if(length==SQL_NULL_DATA)return {DbValueType::Null,""};if(rc==SQL_NO_DATA)break;Check(rc,SQL_HANDLE_STMT,statement,cancellation);
        if(binary){if(rc==SQL_SUCCESS&&length==SQL_NO_TOTAL)throw std::runtime_error("The ODBC driver did not report the final BLOB length.");const auto count=rc==SQL_SUCCESS_WITH_INFO?sizeof(buffer):std::min<size_t>(sizeof(buffer),static_cast<size_t>(std::max<SQLLEN>(0,length)));bytes.append(buffer,count);}
        else text.append(reinterpret_cast<wchar_t*>(buffer),wcsnlen_s(reinterpret_cast<wchar_t*>(buffer),sizeof(buffer)/sizeof(wchar_t)));
        if(bytes.size()+text.size()*2>64*1024*1024)throw std::runtime_error("A database cell exceeds the 64 MiB value limit.");if(rc==SQL_SUCCESS)break;
    }
    return {binary?DbValueType::Blob:DbValueType::Text,binary?bytes:Utf8(text)};
}
QueryResultSet Fetch(SQLHSTMT statement,int maximumRows,const std::atomic_bool* cancellation){QueryResultSet result;SQLSMALLINT columns=0;Check(SQLNumResultCols(statement,&columns),SQL_HANDLE_STMT,statement,cancellation);std::vector<SQLSMALLINT> types;
    for(SQLUSMALLINT i=1;i<=columns;++i){SQLWCHAR name[1024]{};SQLSMALLINT length=0,type=0,decimal=0,nullable=0;SQLULEN size=0;Check(SQLDescribeColW(statement,i,name,1024,&length,&type,&size,&decimal,&nullable),SQL_HANDLE_STMT,statement,cancellation);if(length>=1024)throw std::runtime_error("An ODBC result column label is too long.");result.columns.push_back(UniqueColumnLabel(Utf8(std::wstring(reinterpret_cast<wchar_t*>(name),length)),result.columns));types.push_back(type);}
    SQLRETURN fetched=SQL_SUCCESS;while((fetched=SQLFetch(statement))!=SQL_NO_DATA){Check(fetched,SQL_HANDLE_STMT,statement,cancellation);if(result.rows.size()>=static_cast<size_t>(maximumRows)){result.truncated=true;break;}TypedRow typed;std::map<std::string,std::string> row;for(SQLUSMALLINT i=1;i<=columns;++i){const auto value=ReadValue(statement,i,types[i-1],cancellation);typed[result.columns[i-1]]=value;row[result.columns[i-1]]=DbValueDisplay(value);}result.rows.push_back(std::move(row));result.typedRows.push_back(std::move(typed));}
    result.rowCount=static_cast<int64_t>(result.rows.size());return result;
}
struct BoundValue {std::wstring text;std::string bytes;SQLBIGINT integer=0;double real=0;unsigned char boolean=0;SQLLEN length=0;};
void Bind(SQLHSTMT statement,const std::vector<DbValue>& values,std::vector<BoundValue>& storage,const std::atomic_bool* cancellation){storage.resize(values.size());for(size_t i=0;i<values.size();++i){const auto& value=values[i];auto& bound=storage[i];SQLSMALLINT ctype=SQL_C_WCHAR,type=SQL_WVARCHAR;SQLPOINTER pointer=nullptr;SQLLEN capacity=0;SQLULEN size=1;
    if(value.type==DbValueType::Null){bound.length=SQL_NULL_DATA;pointer=&bound.integer;ctype=SQL_C_SBIGINT;type=SQL_BIGINT;}
    else if(value.type==DbValueType::Integer){bound.integer=DbValueJson(value).get<int64_t>();bound.length=sizeof(bound.integer);pointer=&bound.integer;capacity=sizeof(bound.integer);ctype=SQL_C_SBIGINT;type=SQL_BIGINT;}
    else if(value.type==DbValueType::Real){bound.real=DbValueJson(value).get<double>();bound.length=sizeof(bound.real);pointer=&bound.real;capacity=sizeof(bound.real);ctype=SQL_C_DOUBLE;type=SQL_DOUBLE;}
    else if(value.type==DbValueType::Boolean){bound.boolean=DbValueJson(value).get<bool>()?1:0;bound.length=1;pointer=&bound.boolean;capacity=1;ctype=SQL_C_BIT;type=SQL_BIT;}
    else if(value.type==DbValueType::Blob){bound.bytes=value.text;bound.length=static_cast<SQLLEN>(bound.bytes.size());pointer=bound.bytes.data();capacity=bound.length;size=std::max<SQLULEN>(1,bound.bytes.size());ctype=SQL_C_BINARY;type=bound.bytes.size()>8000?SQL_LONGVARBINARY:SQL_VARBINARY;}
    else{bound.text=Wide(value.type==DbValueType::Json?DbValueJson(value).dump():value.text);bound.length=static_cast<SQLLEN>(bound.text.size()*sizeof(wchar_t));pointer=bound.text.data();capacity=bound.length+sizeof(wchar_t);size=std::max<SQLULEN>(1,bound.text.size());type=bound.text.size()>4000?SQL_WLONGVARCHAR:SQL_WVARCHAR;}
    Check(SQLBindParameter(statement,static_cast<SQLUSMALLINT>(i+1),SQL_PARAM_INPUT,ctype,type,size,0,pointer,capacity,&bound.length),SQL_HANDLE_STMT,statement,cancellation);
}}
class OdbcProvider final:public SqlProvider {
public:
    using SqlProvider::SqlProvider;
    void Test(const std::atomic_bool* cancellation) override {Session connection(profile_,cancellation);}
    QueryResult Query(const std::string& text,int rows,const std::atomic_bool* cancellation) override {return Prepared(text,{},std::clamp(rows,1,100000),cancellation);}
    QueryResult Prepared(const std::string& text,const std::vector<DbValue>& values,int maximumRows,const std::atomic_bool* cancellation) override {
        QueryResult result;result.query=text;const auto started=std::chrono::steady_clock::now();
        try{RequireNotCancelled(cancellation);if(profile_.readOnly)RequireReadOnlySql(text);Session session(profile_,cancellation);bool automatic=true;const auto parsed=SqlStatements(text);for(const auto& sql:parsed){const auto token=Lower(sql);if(std::regex_search(token,std::regex(R"(^\s*(begin|commit|rollback|savepoint|release|vacuum|pragma|attach|detach)\b)")))automatic=false;}if(automatic)Check(SQLSetConnectAttrW(session.connection.value,SQL_ATTR_AUTOCOMMIT,reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF),0),SQL_HANDLE_DBC,session.connection.value,cancellation);
            bool completed=false;try{const auto statements=values.empty()?SqlStatements(text):std::vector<std::string>{text};for(const auto& sql:statements){RequireNotCancelled(cancellation);OdbcHandle statement(SQL_HANDLE_STMT,session.connection.value);Check(SQLSetStmtAttrW(statement.value,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(profile_.timeoutSec)),0),SQL_HANDLE_STMT,statement.value,cancellation);CancelGuard guard(SQL_HANDLE_STMT,statement.value,cancellation);const auto query=Wide(sql);
                if(values.empty())Check(SQLExecDirectW(statement.value,reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(query.c_str())),SQL_NTS),SQL_HANDLE_STMT,statement.value,cancellation);
                else{Check(SQLPrepareW(statement.value,reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(query.c_str())),SQL_NTS),SQL_HANDLE_STMT,statement.value,cancellation);std::vector<BoundValue> storage;Bind(statement.value,values,storage,cancellation);Check(SQLExecute(statement.value),SQL_HANDLE_STMT,statement.value,cancellation);}
                for(;;){SQLSMALLINT columns=0;Check(SQLNumResultCols(statement.value,&columns),SQL_HANDLE_STMT,statement.value,cancellation);SQLLEN affected=0;Check(SQLRowCount(statement.value,&affected),SQL_HANDLE_STMT,statement.value,cancellation);if(affected>0)result.rowsAffected+=affected;
                    if(columns){auto set=Fetch(statement.value,maximumRows,cancellation);set.query=sql;set.rowsAffected=affected>0?affected:0;result.columns=set.columns;result.rows=set.rows;result.typedRows=set.typedRows;result.rowCount=set.rowCount;result.truncated=set.truncated;result.resultSets.push_back(std::move(set));}
                    const auto next=SQLMoreResults(statement.value);if(next==SQL_NO_DATA)break;Check(next,SQL_HANDLE_STMT,statement.value,cancellation);
                }}RequireNotCancelled(cancellation);if(automatic)Check(SQLEndTran(SQL_HANDLE_DBC,session.connection.value,SQL_COMMIT),SQL_HANDLE_DBC,session.connection.value,cancellation);completed=true;}
            catch(...){if(automatic)SQLEndTran(SQL_HANDLE_DBC,session.connection.value,SQL_ROLLBACK);throw;}(void)completed;
        }catch(const std::exception& error){result.error=ScrubConnectionError(error.what(),profile_);result.columns.clear();result.rows.clear();result.typedRows.clear();result.resultSets.clear();result.rowsAffected=0;result.rowCount=0;}
        result.executionTimeMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();return result;
    }
    DatabaseSchema Schema(const std::atomic_bool* cancellation) override {
        Session session(profile_,cancellation);DatabaseSchema schema;schema.databaseId=profile_.id;schema.databaseName=profile_.name;schema.dialect=Dialect();schema.capabilities=ProviderCapabilities("sql",profile_.readOnly,schema.dialect);
        std::string currentCatalog=UrlProfile(profile_).database;SQLWCHAR currentDatabase[4096]{};SQLSMALLINT databaseLength=0;
        const auto databaseInfo=SQLGetInfoW(session.connection.value,SQL_DATABASE_NAME,currentDatabase,sizeof(currentDatabase),&databaseLength);RequireNotCancelled(cancellation);
        if(SQL_SUCCEEDED(databaseInfo)&&databaseLength>0){if(databaseLength>=static_cast<SQLSMALLINT>(sizeof(currentDatabase))||databaseLength%sizeof(SQLWCHAR))throw std::runtime_error("The ODBC driver's current catalog name was truncated. Select an explicit database before schema inspection.");currentCatalog=Utf8(std::wstring(reinterpret_cast<wchar_t*>(currentDatabase),databaseLength/sizeof(SQLWCHAR)));}
        OdbcHandle statement(SQL_HANDLE_STMT,session.connection.value);CancelGuard guard(SQL_HANDLE_STMT,statement.value,cancellation);const auto namespaceName=Wide(profile_.schema),currentCatalogName=Wide(currentCatalog);
        Check(SQLTablesW(statement.value,currentCatalogName.empty()?nullptr:reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(currentCatalogName.c_str())),currentCatalogName.empty()?0:SQL_NTS,namespaceName.empty()?nullptr:reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(namespaceName.c_str())),namespaceName.empty()?0:SQL_NTS,nullptr,0,nullptr,0),SQL_HANDLE_STMT,statement.value,cancellation);
        auto tables=Fetch(statement.value,10000,cancellation);if(tables.truncated)throw std::runtime_error("Schema contains more than 10,000 objects. Specify a schema to narrow inspection.");
        std::map<std::string,std::string> identities;
        for(const auto& row:tables.rows){RequireNotCancelled(cancellation);const auto name=row.at("TABLE_NAME"),type=row.at("TABLE_TYPE");const auto namespaceValue=row.count("TABLE_SCHEM")&&row.at("TABLE_SCHEM")!="NULL"?row.at("TABLE_SCHEM"):"";if(type!="TABLE"&&type!="VIEW"&&type!="BASE TABLE")continue;if(namespaceValue=="information_schema"||namespaceValue=="pg_catalog"||namespaceValue=="sys")continue;
            const auto catalog=row.count("TABLE_CAT")&&row.at("TABLE_CAT")!="NULL"?row.at("TABLE_CAT"):"";if(!IncludeOdbcCatalog(currentCatalog,catalog)||!IncludeOdbcNamespace(profile_.schema,namespaceValue))continue;const auto identity=catalog+"\n"+namespaceValue;
            const auto existing=identities.find(name);if(existing!=identities.end()){if(existing->second!=identity)throw std::runtime_error("Several database namespaces contain the same table name. Specify the database and schema in this connection before browsing or editing rows.");continue;}identities[name]=identity;
            TableMeta table;table.name=name;table.type=type=="VIEW"?"view":"table";table.editable=table.type=="table"&&!profile_.readOnly;table.rowCount=-1;
            auto metadata=[&](auto callback){OdbcHandle command(SQL_HANDLE_STMT,session.connection.value);CancelGuard cancel(SQL_HANDLE_STMT,command.value,cancellation);callback(command.value);return Fetch(command.value,10000,cancellation);};
            const auto tableName=Wide(name),schemaName=Wide(namespaceValue),catalogName=Wide(catalog);auto schemaPointer=schemaName.empty()?nullptr:reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(schemaName.c_str()));auto catalogPointer=catalogName.empty()?nullptr:reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(catalogName.c_str()));
            auto columns=metadata([&](SQLHSTMT command){Check(SQLColumnsW(command,catalogPointer,catalogPointer?SQL_NTS:0,schemaPointer,schemaPointer?SQL_NTS:0,reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(tableName.c_str())),SQL_NTS,nullptr,0),SQL_HANDLE_STMT,command,cancellation);});
            for(size_t ci=0;ci<columns.rows.size();++ci){const auto& item=columns.rows[ci];if(item.count("TABLE_NAME")&&item.at("TABLE_NAME")!=name)continue;if(item.count("TABLE_SCHEM")&&(item.at("TABLE_SCHEM")=="NULL"?std::string():item.at("TABLE_SCHEM"))!=namespaceValue)continue;if(item.count("TABLE_CAT")&&(item.at("TABLE_CAT")=="NULL"?std::string():item.at("TABLE_CAT"))!=catalog)continue;ColumnMeta column;column.name=item.at("COLUMN_NAME");column.type=OdbcColumnType(item,Dialect());column.cid=std::stoi(item.at("ORDINAL_POSITION"))-1;column.notnull=item.at("NULLABLE")=="0";if(item.count("COLUMN_DEF")&&ci<columns.typedRows.size()&&columns.typedRows[ci].count("COLUMN_DEF")&&columns.typedRows[ci].at("COLUMN_DEF").type!=DbValueType::Null)column.dfltValue=item.at("COLUMN_DEF");table.columns.push_back(column);}
            auto keys=metadata([&](SQLHSTMT command){Check(SQLPrimaryKeysW(command,catalogPointer,catalogPointer?SQL_NTS:0,schemaPointer,schemaPointer?SQL_NTS:0,reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(tableName.c_str())),SQL_NTS),SQL_HANDLE_STMT,command,cancellation);});
            std::map<int,std::string> ordered;for(const auto& key:keys.rows)ordered[std::stoi(key.at("KEY_SEQ"))]=key.at("COLUMN_NAME");for(const auto& key:ordered){table.primaryKeys.push_back(key.second);for(auto& column:table.columns)if(column.name==key.second)column.pk=true;}
            std::vector<std::map<std::string,std::string>> foreignRows;
            try{auto foreign=metadata([&](SQLHSTMT command){Check(SQLForeignKeysW(command,nullptr,0,nullptr,0,nullptr,0,catalogPointer,catalogPointer?SQL_NTS:0,schemaPointer,schemaPointer?SQL_NTS:0,reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(tableName.c_str())),SQL_NTS),SQL_HANDLE_STMT,command,cancellation);});if(foreign.truncated)throw std::runtime_error("Foreign-key metadata exceeds the inspection limit.");foreignRows=foreign.rows;int foreignIdentity=-1;for(const auto& key:foreign.rows){ForeignKeyMeta item;item.seq=std::stoi(key.at("KEY_SEQ"))-1;if(item.seq==0)++foreignIdentity;item.id=std::max(foreignIdentity,0);item.fromColumn=key.at("FKCOLUMN_NAME");item.targetTable=key.at("PKTABLE_NAME");item.toColumn=key.at("PKCOLUMN_NAME");item.onUpdate=ForeignKeyAction(key,"UPDATE_RULE");item.onDelete=ForeignKeyAction(key,"DELETE_RULE");table.foreignKeys.push_back(item);}}catch(...){RequireNotCancelled(cancellation);}
            try{auto indexes=metadata([&](SQLHSTMT command){Check(SQLStatisticsW(command,catalogPointer,catalogPointer?SQL_NTS:0,schemaPointer,schemaPointer?SQL_NTS:0,reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(tableName.c_str())),SQL_NTS,SQL_INDEX_ALL,SQL_ENSURE),SQL_HANDLE_STMT,command,cancellation);});std::map<std::string,IndexMeta> found;for(const auto& index:indexes.rows){if(index.at("INDEX_NAME")=="NULL")continue;auto& item=found[index.at("INDEX_NAME")];item.name=index.at("INDEX_NAME");item.unique=index.at("NON_UNIQUE")=="false"||index.at("NON_UNIQUE")=="0";item.origin="ODBC";if(index.at("COLUMN_NAME")!="NULL")item.columns.push_back(index.at("COLUMN_NAME"));}for(const auto& index:found)table.indexes.push_back(index.second);}catch(...){RequireNotCancelled(cancellation);}
            try{const auto count=metadata([&](SQLHSTMT command){Check(SQLSetStmtAttrW(command,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(profile_.timeoutSec)),0),SQL_HANDLE_STMT,command,cancellation);const auto source=namespaceValue.empty()?Identifier(name):Identifier(namespaceValue)+"."+Identifier(name);const auto query=Wide("SELECT COUNT(*) AS total FROM "+source);Check(SQLExecDirectW(command,reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(query.c_str())),SQL_NTS),SQL_HANDLE_STMT,command,cancellation);});if(!count.rows.empty())table.rowCount=std::stoll(count.rows.front().begin()->second);}catch(...){RequireNotCancelled(cancellation);}
            std::string viewDefinition;const auto definitionQuery=table.type=="view"?OdbcViewDefinitionQuery(Dialect()):"";
            if(!definitionQuery.empty())try {
                OdbcHandle command(SQL_HANDLE_STMT,session.connection.value);CancelGuard cancel(SQL_HANDLE_STMT,command.value,cancellation);
                Check(SQLSetStmtAttrW(command.value,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(profile_.timeoutSec)),0),SQL_HANDLE_STMT,command.value,cancellation);
                const auto query=Wide(definitionQuery);Check(SQLPrepareW(command.value,reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(query.c_str())),SQL_NTS),SQL_HANDLE_STMT,command.value,cancellation);
                const auto definitionSchema=namespaceValue.empty()?catalog:namespaceValue;
                const std::vector<DbValue> values{{DbValueType::Text,definitionSchema},{DbValueType::Text,name}};std::vector<BoundValue> storage;Bind(command.value,values,storage,cancellation);
                Check(SQLExecute(command.value),SQL_HANDLE_STMT,command.value,cancellation);const auto definition=Fetch(command.value,1,cancellation);
                if(!definition.truncated&&definition.typedRows.size()==1&&definition.typedRows[0].size()==1&&definition.typedRows[0].begin()->second.type==DbValueType::Text)viewDefinition=definition.typedRows[0].begin()->second.text;
            }catch(...){RequireNotCancelled(cancellation);}
            const auto ddlNamespace=namespaceValue.empty()&&(Dialect()=="mysql"||Dialect()=="mariadb")?catalog:namespaceValue;
            table.ddl=BuildOdbcSchemaDdl(table,Dialect(),ddlNamespace,foreignRows,viewDefinition);
            schema.tables.push_back(std::move(table));
        }return schema;
    }
};
}
std::vector<std::string> InstalledOdbcDrivers(){OdbcHandle environment(SQL_HANDLE_ENV);Check(SQLSetEnvAttr(environment.value,SQL_ATTR_ODBC_VERSION,reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3_80),0),SQL_HANDLE_ENV,environment.value);std::vector<std::string> result;SQLUSMALLINT direction=SQL_FETCH_FIRST;for(;;){SQLWCHAR description[1024]{},attributes[8192]{};SQLSMALLINT length=0,attributeLength=0;auto rc=SQLDriversW(environment.value,direction,description,1024,&length,attributes,8192,&attributeLength);if(rc==SQL_NO_DATA)break;Check(rc,SQL_HANDLE_ENV,environment.value);if(length<1024)result.push_back(Utf8(std::wstring(reinterpret_cast<wchar_t*>(description),length)));direction=SQL_FETCH_NEXT;}return result;}
std::shared_ptr<DatabaseProvider> CreateOdbcProvider(const ConnectionProfile& profile){return std::make_shared<OdbcProvider>(profile);}
} // namespace native_app
