#ifndef _FLOWSQL_SERVICES_CATALOG_CATALOG_PLUGIN_H_
#define _FLOWSQL_SERVICES_CATALOG_CATALOG_PLUGIN_H_

#include <common/iplugin.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/idataframe_channel.h>
#include <framework/interfaces/ioperator_catalog.h>
#include <framework/interfaces/ioperator_registry.h>
#include <framework/interfaces/irouter_handle.h>

#include <rapidjson/document.h>
#include <sqlite3.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace flowsql {
namespace catalog {

class __attribute__((visibility("default"))) CatalogPlugin
    : public IPlugin, public IChannelRegistry, public IOperatorRegistry, public IOperatorCatalog, public IRouterHandle {
 public:
    CatalogPlugin() = default;
    ~CatalogPlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

    // IChannelRegistry
    int Register(const char* name, std::shared_ptr<IChannel> channel) override;
    std::shared_ptr<IChannel> Get(const char* name) override;
    int Unregister(const char* name) override;
    int Rename(const char* old_name, const char* new_name) override;
    void List(std::function<void(const char* name, std::shared_ptr<IChannel>)> callback) override;

    // IOperatorRegistry
    int Register(const char* name, OperatorFactory factory) override;
    IOperator* Create(const char* name) override;
    void List(std::function<void(const char* name)> callback) override;

    // IOperatorCatalog
    OperatorStatus QueryStatus(const std::string& catelog, const std::string& name) override;
    UpsertResult UpsertBatch(const std::vector<OperatorMeta>& operators) override;

    // IRouterHandle（9.4 再补 HTTP 端点）
    void EnumRoutes(std::function<void(const RouteItem&)> callback) override;

 private:
    int32_t HandleListChannels(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleImportCsv(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandlePreview(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleRename(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleDelete(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleQueryOperators(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleOperatorDetail(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleOperatorActivate(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleOperatorDeactivate(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleOperatorUpdate(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleOperatorUpsertBatch(const std::string& uri, const std::string& req, std::string& rsp);

    int EnsureOperatorCatalogDb();
    int EnsureOperatorCatalogSchema();
    int EnsureOperatorDbDir() const;
    std::string OperatorCatalogDbPath() const;
    static bool ParseOperatorRef(const rapidjson::Value& doc, std::string* catelog, std::string* name);
    static bool ParseOperatorRefFromName(const std::string& full_name, std::string* catelog, std::string* name);
    int QueryOperatorDetail(const std::string& catelog,
                            const std::string& name,
                            OperatorMeta* meta,
                            int* active,
                            int* editable,
                            std::string* created_at,
                            std::string* updated_at);
    int SetOperatorActive(const std::string& catelog, const std::string& name, int active);
    int UpdateOperatorFields(const std::string& catelog,
                             const std::string& name,
                             const std::string* description,
                             const std::string* position);
    std::string OperatorsDirPath() const;
    std::string OperatorContentPath(const std::string& catelog, const std::string& name) const;
    int LoadOperatorContent(const std::string& catelog, const std::string& name, std::string* content) const;
    int SaveOperatorContent(const std::string& catelog, const std::string& name, const std::string& content) const;

    std::string GenerateImportName(const std::string& filename);
    static const char* DataTypeName(DataType t);

    int PersistChannelToCsv(const std::string& name, IDataFrameChannel* channel);
    std::shared_ptr<IDataFrameChannel> LoadCsvFile(const std::string& path, const std::string& name);
    std::string CsvPath(const std::string& name) const;
    int EnsureDataDir();

    static std::vector<std::string> ParseCsvLine(const std::string& line);
    static std::string EscapeCsvField(const std::string& field);
    static bool TryParseInt64(const std::string& s, int64_t* out);
    static bool TryParseDouble(const std::string& s, double* out);
    static std::string FieldValueToString(const FieldValue& v);

    IQuerier* querier_ = nullptr;
    std::unordered_map<std::string, std::shared_ptr<IChannel>> channels_;
    std::unordered_map<std::string, OperatorFactory> op_factories_;
    std::string data_dir_ = "./dataframes";
    std::string operator_db_dir_ = "./catalog";
    std::string operator_db_path_;
    sqlite3* operator_db_ = nullptr;
    mutable std::mutex mu_;
};

}  // namespace catalog
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_CATALOG_CATALOG_PLUGIN_H_
