#include "database_channel.h"

#include "arrow_adapters.h"
#include "capability_interfaces.h"

#include <cstdio>
#include <common/log.h>

namespace flowsql {
namespace database {

DatabaseChannel::DatabaseChannel(const std::string& type, const std::string& name,
                                 IDbDriver* driver,
                                 SessionFactory session_factory)
    : type_(type), name_(name), driver_(driver), session_factory_(std::move(session_factory)) {}

DatabaseChannel::~DatabaseChannel() {
    if (opened_) Close();
}

int DatabaseChannel::Open() {
    if (opened_) return 0;
    // 驱动已由 DatabasePlugin 负责连接，这里只验证连接状态
    if (!driver_ || !driver_->IsConnected()) return -1;
    opened_ = true;
    return 0;
}

int DatabaseChannel::Close() {
    if (!opened_) return 0;
    // 只标记关闭，不调用 Disconnect()，驱动由 DatabasePlugin 统一管理生命周期
    opened_ = false;
    return 0;
}

int DatabaseChannel::CreateReader(const char* query, IBatchReader** reader) {
    last_error_.clear();
    if (!opened_) {
        last_error_ = std::string("channel not opened (") + type_ + "." + name_ + ")";
        return -1;
    }
    if (!session_factory_) {
        last_error_ = std::string("session factory not set (") + type_ + "." + name_ + ")";
        return -1;
    }

    auto session = session_factory_();
    if (!session) {
        last_error_ = std::string("failed to create session (") + type_ + "." + name_ + ")";
        return -1;
    }

    auto* batch_readable = dynamic_cast<IBatchReadable*>(session.get());
    if (!batch_readable) {
        last_error_ = std::string("session does not support batch reading (") + type_ + "." + name_ + ")";
        return -1;
    }

    LOG_INFO("DatabaseChannel::CreateReader: executing query on %s.%s: %s",
           type_.c_str(), name_.c_str(), query);
    int ret = batch_readable->CreateReader(query, reader);
    if (ret != 0) {
        // session 是局部变量，必须在销毁前拷贝错误
        last_error_ = batch_readable->GetLastError();
    }
    return ret;
}

int DatabaseChannel::CreateWriter(const char* table, IBatchWriter** writer) {
    last_error_.clear();
    if (!opened_ || !session_factory_) { last_error_ = "channel not opened"; return -1; }

    auto session = session_factory_();
    if (!session) { last_error_ = "failed to create session"; return -1; }

    auto* batch_writable = dynamic_cast<IBatchWritable*>(session.get());
    if (!batch_writable) {
        last_error_ = std::string("session does not support batch writing (") + type_ + "." + name_ + ")";
        return -1;
    }

    int ret = batch_writable->CreateWriter(table, writer);
    if (ret != 0) last_error_ = batch_writable->GetLastError();
    return ret;
}

bool DatabaseChannel::IsConnected() {
    if (!driver_) return false;
    return driver_->IsConnected();
}

// ==================== 列式数据库接口实现 ====================

int DatabaseChannel::CreateArrowReader(const char* query, IArrowReader** reader) {
    if (!opened_ || !session_factory_) { *reader = nullptr; return -1; }
    auto session = session_factory_();
    if (!session) { *reader = nullptr; return -1; }
    // dynamic_cast 检查 IArrowReadable 能力接口（仅列式数据库 Session 实现此接口）
    auto* arrow_readable = dynamic_cast<IArrowReadable*>(session.get());
    if (!arrow_readable) { *reader = nullptr; return -1; }
    *reader = new ArrowReaderAdapter(std::move(session), query);
    return 0;
}

int DatabaseChannel::CreateArrowWriter(const char* table, IArrowWriter** writer) {
    if (!opened_ || !session_factory_) { *writer = nullptr; return -1; }
    auto session = session_factory_();
    if (!session) { *writer = nullptr; return -1; }
    // dynamic_cast 检查 IArrowWritable 能力接口（仅列式数据库 Session 实现此接口）
    auto* arrow_writable = dynamic_cast<IArrowWritable*>(session.get());
    if (!arrow_writable) { *writer = nullptr; return -1; }
    *writer = new ArrowWriterAdapter(std::move(session), table);
    return 0;
}

int DatabaseChannel::ExecuteQueryArrow(const char* query,
                                       std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) {
    last_error_.clear();
    if (!opened_ || !session_factory_) {
        last_error_ = "channel not opened or session factory not set";
        return -1;
    }
    auto session = session_factory_();
    if (!session) { last_error_ = "failed to create session"; return -1; }
    int ret = session->ExecuteQueryArrow(query, batches);
    if (ret != 0) last_error_ = session->GetLastError();
    return ret;
}

int DatabaseChannel::WriteArrowBatches(const char* table,
                                       const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) {
    last_error_.clear();
    if (!opened_ || !session_factory_) {
        last_error_ = "channel not opened or session factory not set";
        return -1;
    }
    auto session = session_factory_();
    if (!session) { last_error_ = "failed to create session"; return -1; }
    int ret = session->WriteArrowBatches(table, batches);
    if (ret != 0) last_error_ = session->GetLastError();
    return ret;
}

int DatabaseChannel::ExecuteSql(const char* sql) {
    last_error_.clear();
    if (!opened_ || !session_factory_) {
        last_error_ = "channel not opened or session factory not set";
        return -1;
    }
    auto session = session_factory_();
    if (!session) { last_error_ = "failed to create session"; return -1; }
    int ret = session->ExecuteSql(sql);
    if (ret < 0) last_error_ = session->GetLastError();
    return ret;
}

}  // namespace database
}  // namespace flowsql
