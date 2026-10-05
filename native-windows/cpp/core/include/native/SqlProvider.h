#pragma once
#include "NativeConnections.h"
#include "DbValues.h"
#include <algorithm>
#include <climits>
#include <stdexcept>
namespace native_app {
class SqlProvider : public DatabaseProvider {
public:
    using DatabaseProvider::DatabaseProvider;
    virtual QueryResult Prepared(const std::string& query, const std::vector<DbValue>& values,
        int maximumRows, const std::atomic_bool* cancellation) = 0;
    virtual std::string Dialect() const { return profile_.type; }
    std::string Identifier(const std::string& value) const { return QuoteSqlIdentifier(value,Dialect()); }
    std::string Qualified(const std::string& name) const {
        return profile_.schema.empty()?Identifier(name):Identifier(profile_.schema)+"."+Identifier(name);
    }
    TableMeta Metadata(const std::string& name, const std::atomic_bool* cancellation) {
        const auto schema=Schema(cancellation);auto found=std::find_if(schema.tables.begin(),schema.tables.end(),[&](const auto& table){return table.name==name;});
        if(found==schema.tables.end())throw std::runtime_error("Unknown table or view.");return *found;
    }
    TableDataResult Table(const std::string& table,int page,int pageSize,const std::string& sort,const std::string& direction,
        const std::string& search,const std::string& filterColumn,const std::string& filterValue,const std::atomic_bool* cancellation) override {
        TableDataResult result;result.table=table;result.page=std::max(page,1);result.pageSize=std::clamp(pageSize,1,200);
        try{RequireNotCancelled(cancellation);const auto metadata=Metadata(table,cancellation);for(const auto& column:metadata.columns)result.columns.push_back(column.name);result.primaryKeys=metadata.primaryKeys;result.columnMetadata=metadata.columns;
            auto select=Selection(metadata,sort,direction,search,filterColumn,filterValue);auto count=Prepared("SELECT COUNT(*) AS total FROM "+Qualified(table)+select.where,select.parameters,1,cancellation);if(!count.error.empty())throw std::runtime_error(count.error);
            if(count.rows.empty())throw std::runtime_error("Database returned no count result.");result.totalRows=std::stoll(count.rows[0].begin()->second);result.totalPages=static_cast<int>(std::clamp<int64_t>((result.totalRows+result.pageSize-1)/result.pageSize,1,INT_MAX));result.page=std::min(result.page,result.totalPages);
            const auto offset=static_cast<int64_t>(result.page-1)*result.pageSize;std::string query="SELECT * FROM "+Qualified(table)+select.where+select.order;
            if(Dialect()=="mssql"||Dialect()=="oracle"){if(select.order.empty())query+=" ORDER BY "+Identifier(result.columns.front());query+=" OFFSET "+std::to_string(offset)+" ROWS FETCH NEXT "+std::to_string(result.pageSize)+" ROWS ONLY";}
            else query+=" LIMIT "+std::to_string(result.pageSize)+" OFFSET "+std::to_string(offset);
            auto rows=Prepared(query,select.parameters,result.pageSize,cancellation);if(!rows.error.empty())throw std::runtime_error(rows.error);result.rows=std::move(rows.rows);result.typedRows=std::move(rows.typedRows);
        }catch(const std::exception& error){result.error=ScrubConnectionError(error.what(),profile_);result.rows.clear();result.typedRows.clear();}return result;
    }
    bool Insert(const std::string& table,const TypedRow& values,const std::atomic_bool* cancellation) override {
        const auto result=InsertResult(table,values,cancellation);return result.rowsAffected>0||!result.typedRows.empty();
    }
    QueryResult InsertResult(const std::string& table,const TypedRow& values,const std::atomic_bool* cancellation) {
        Writable();const auto metadata=Metadata(table,cancellation);CheckColumns(metadata,values);std::string query="INSERT INTO "+Qualified(table);std::vector<DbValue> parameters;
        std::string returned;for(const auto& key:metadata.primaryKeys){if(!returned.empty())returned+=", ";returned+=(Dialect()=="mssql"?"INSERTED.":"")+Identifier(key);}
        std::string valuesClause;
        if(values.empty())valuesClause=Dialect()=="mysql"||Dialect()=="mariadb"?" () VALUES ()":" DEFAULT VALUES";
        else{query+=" (";std::string placeholders;for(const auto& item:values){if(!parameters.empty()){query+=", ";placeholders+=", ";}query+=Identifier(item.first);placeholders+='?';parameters.push_back(item.second);}query+=")";valuesClause=" VALUES ("+placeholders+")";}
        if(Dialect()=="mssql"&&!returned.empty())query+=" OUTPUT "+returned;
        query+=valuesClause;
        if((Dialect()=="duckdb"||Dialect()=="postgresql")&&!returned.empty())query+=" RETURNING "+returned;
        const auto result=Prepared(query,parameters,1,cancellation);if(!result.error.empty())throw std::runtime_error(result.error);return result;
    }
    TypedRow Find(const std::string& table,const TypedRow& key,const std::atomic_bool* cancellation) {
        const auto metadata=Metadata(table,cancellation);CheckKey(metadata,key);std::vector<DbValue> parameters;const auto where=KeyWhere(key,parameters);
        std::string query=(Dialect()=="mssql"?"SELECT TOP (1) * FROM ":"SELECT * FROM ")+Qualified(table)+" WHERE "+where;
        if(Dialect()=="oracle")query+=" FETCH FIRST 1 ROWS ONLY";else if(Dialect()!="mssql")query+=" LIMIT 1";
        const auto result=Prepared(query,parameters,1,cancellation);if(!result.error.empty())throw std::runtime_error(result.error);return result.typedRows.empty()?TypedRow{}:result.typedRows.front();
    }
    bool Update(const std::string& table,const TypedRow& key,const TypedRow& values,const std::atomic_bool* cancellation) override {
        Writable();if(values.empty())return false;const auto metadata=Metadata(table,cancellation);CheckColumns(metadata,values);CheckKey(metadata,key);
        std::string query="UPDATE "+Qualified(table)+" SET ";std::vector<DbValue> parameters;for(const auto& item:values){if(!parameters.empty())query+=", ";query+=Identifier(item.first)+" = ?";parameters.push_back(item.second);}query+=" WHERE "+KeyWhere(key,parameters);
        const auto result=Prepared(query,parameters,1,cancellation);if(!result.error.empty())throw std::runtime_error(result.error);return result.rowsAffected==1;
    }
    bool Delete(const std::string& table,const TypedRow& key,const std::atomic_bool* cancellation) override {
        Writable();const auto metadata=Metadata(table,cancellation);CheckKey(metadata,key);std::vector<DbValue> parameters;
        const auto result=Prepared("DELETE FROM "+Qualified(table)+" WHERE "+KeyWhere(key,parameters),parameters,1,cancellation);if(!result.error.empty())throw std::runtime_error(result.error);return result.rowsAffected==1;
    }
    std::string Export(const std::string& table,const std::string& format,const std::string& search,const std::string& sort,const std::string& direction,const std::atomic_bool* cancellation) override {
        const auto metadata=Metadata(table,cancellation);auto selection=Selection(metadata,sort,direction,search,"","");
        const auto result=Prepared("SELECT * FROM "+Qualified(table)+selection.where+selection.order,selection.parameters,INT_MAX,cancellation);
        if(!result.error.empty())throw std::runtime_error(result.error);if(result.truncated)throw std::runtime_error("Export exceeded the provider's result limit; narrow the filter.");RequireNotCancelled(cancellation);
        const auto text=ExportTypedRows(result.columns,result.typedRows,format,table,Dialect());RequireNotCancelled(cancellation);return text;
    }
protected:
    struct SelectParts {std::string where,order;std::vector<DbValue> parameters;};
    void Writable() const {if(profile_.readOnly)throw std::runtime_error("This connection is read-only.");}
    void CheckColumns(const TableMeta& metadata,const TypedRow& values) const {
        if(!metadata.editable)throw std::runtime_error("This table is read-only.");for(const auto& item:values){if(std::none_of(metadata.columns.begin(),metadata.columns.end(),[&](const auto& column){return column.name==item.first;}))throw std::runtime_error("Unknown table column.");DbValueJson(item.second);}
    }
    void CheckKey(const TableMeta& metadata,const TypedRow& key) const {
        if(key.size()!=metadata.primaryKeys.size()||key.empty())throw std::runtime_error("Supply the complete primary key to edit this row.");for(const auto& column:metadata.primaryKeys)if(!key.count(column)||key.at(column).type==DbValueType::Null)throw std::runtime_error("Supply the complete non-null primary key to edit this row.");
    }
    std::string KeyWhere(const TypedRow& key,std::vector<DbValue>& parameters) const {
        std::string where;for(const auto& item:key){if(!where.empty())where+=" AND ";where+=Identifier(item.first)+(item.second.type==DbValueType::Null?" IS NULL":" = ?");if(item.second.type!=DbValueType::Null)parameters.push_back(item.second);}return where;
    }
    SelectParts Selection(const TableMeta& metadata,const std::string& sort,const std::string& direction,const std::string& search,const std::string& filterColumn,const std::string& filterValue) const {
        SelectParts result;auto exists=[&](const std::string& name){return std::any_of(metadata.columns.begin(),metadata.columns.end(),[&](const auto& column){return column.name==name;});};
        if(!sort.empty()&&!exists(sort))throw std::runtime_error("Unknown sort column.");if(!filterColumn.empty()&&!exists(filterColumn))throw std::runtime_error("Unknown filter column.");
        auto pattern=[](const std::string& value){std::string result="%";for(char c:value){if(c=='%'||c=='_'||c=='!')result+='!';result+=c;}return result+"%";};
        auto predicate=[&](const std::string& column){return "CAST("+Identifier(column)+" AS "+(Dialect()=="oracle"?"VARCHAR2(4000)":Dialect()=="mssql"?"NVARCHAR(4000)":Dialect()=="bigquery"||Dialect()=="databricks"?"STRING":"VARCHAR(4096)")+") LIKE ? ESCAPE '!'";};
        if(!filterColumn.empty()){result.where=" WHERE "+predicate(filterColumn);result.parameters.push_back({DbValueType::Text,pattern(filterValue)});}
        if(!search.empty()){result.where+=result.where.empty()?" WHERE (":" AND (";for(size_t i=0;i<metadata.columns.size();++i){if(i)result.where+=" OR ";result.where+=predicate(metadata.columns[i].name);result.parameters.push_back({DbValueType::Text,pattern(search)});}result.where+=')';}
        const auto ordering=sort.empty()?metadata.primaryKeys:std::vector<std::string>{sort};for(size_t i=0;i<ordering.size();++i){result.order+=i?", ":" ORDER BY ";result.order+=Identifier(ordering[i])+(_stricmp(direction.c_str(),"desc")==0?" DESC":" ASC");}return result;
    }
};
} // namespace native_app
