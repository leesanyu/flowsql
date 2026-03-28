#ifndef _FLOWSQL_SERVICES_DATABASE_DATABASE_CHANNEL_H_
#define _FLOWSQL_SERVICES_DATABASE_DATABASE_CHANNEL_H_

#include <framework/interfaces/idatabase_channel.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <functional>

#include "idb_driver.h"
#include "db_session.h"

namespace flowsql {
namespace database {

// DatabaseChannel — 通用数据库通道实现
// 持有 IDbDriver 弱引用和 Session 工厂，支持连接池复用
class DatabaseChannel : public IDatabaseChannel {
 public:
    using SessionFactory = std::function<std::shared_ptr<IDbSession>()>;

    DatabaseChannel(const std::string& type, const std::string& name,
                    IDbDriver* driver,  // 弱引用，不持有所有权
                    SessionFactory session_factory);
    ~DatabaseChannel() override;

    // IChannel
    const char* Category() override { return type_.c_str(); }
    const char* Name() override { return name_.c_str(); }
    const char* Type() override { return ChannelType::kDatabase; }
    const char* Schema() override { return "[]"; }  // TODO: 查询数据库元数据

    int Open() override;
    int Close() override;
    bool IsOpened() const override { return opened_; }
    int Flush() override { return 0; }

    // IDatabaseChannel（使用 Session 创建 Reader/Writer）
    int CreateReader(const char* query, IBatchReader** reader) override;
    int CreateWriter(const char* table, IBatchWriter** writer) override;
    bool IsConnected() override;
    const char* GetLastError() override { return last_error_.c_str(); }

    // IDatabaseChannel 列式接口
    int CreateArrowReader(const char* query, IArrowReader** reader) override;
    int CreateArrowWriter(const char* table, IArrowWriter** writer) override;
    int ExecuteQueryArrow(const char* query,
                          std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) override;
    int WriteArrowBatches(const char* table,
                          const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) override;

    // IDatabaseChannel 通用接口
    int ExecuteSql(const char* sql) override;

 private:
    std::string type_;
    std::string name_;
    IDbDriver* driver_;  // 弱引用
    SessionFactory session_factory_;
    bool opened_ = false;
    std::string last_error_;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_DATABASE_CHANNEL_H_
