#include "database_channel.h"

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
    if (!opened_) {
        LOG_ERROR("DatabaseChannel::CreateReader: channel not opened (%s.%s)", type_.c_str(), name_.c_str());
        return -1;
    }
    if (!session_factory_) {
        LOG_ERROR("DatabaseChannel::CreateReader: session factory not set (%s.%s)", type_.c_str(), name_.c_str());
        return -1;
    }

    // 使用 Session 创建 Reader
    auto session = session_factory_();
    if (!session) {
        LOG_ERROR("DatabaseChannel::CreateReader: failed to create session (%s.%s)", type_.c_str(), name_.c_str());
        return -1;
    }

    // 检查 Session 是否支持 IBatchReadable
    auto* batch_readable = dynamic_cast<IBatchReadable*>(session.get());
    if (!batch_readable) {
        LOG_ERROR("DatabaseChannel: session does not support batch reading");
        return -1;
    }

    LOG_INFO("DatabaseChannel::CreateReader: executing query on %s.%s: %s",
           type_.c_str(), name_.c_str(), query);
    int ret = batch_readable->CreateReader(query, reader);
    if (ret != 0) {
        LOG_ERROR("DatabaseChannel::CreateReader: CreateReader failed with code %d", ret);
    }
    return ret;
}

int DatabaseChannel::CreateWriter(const char* table, IBatchWriter** writer) {
    if (!opened_ || !session_factory_) return -1;

    // 使用 Session 创建 Writer
    auto session = session_factory_();
    if (!session) {
        return -1;
    }

    // 检查 Session 是否支持 IBatchWritable
    auto* batch_writable = dynamic_cast<IBatchWritable*>(session.get());
    if (!batch_writable) {
        LOG_ERROR("DatabaseChannel: session does not support batch writing");
        return -1;
    }

    return batch_writable->CreateWriter(table, writer);
}

bool DatabaseChannel::IsConnected() {
    if (!driver_) return false;
    return driver_->IsConnected();
}

// ==================== 列式数据库接口实现 ====================

int DatabaseChannel::CreateArrowReader(const char* query, IArrowReader** reader) {
    // 行式数据库不支持 Arrow 直读，调用方应使用 CreateReader + IBatchReader
    (void)query;
    *reader = nullptr;
    return -1;
}

int DatabaseChannel::CreateArrowWriter(const char* table, IArrowWriter** writer) {
    // 行式数据库不支持 Arrow 直写，调用方应使用 CreateWriter + IBatchWriter
    (void)table;
    *writer = nullptr;
    return -1;
}

int DatabaseChannel::ExecuteQueryArrow(const char* query,
                                       std::vector<std::shared_ptr<arrow::RecordBatch>>* batches,
                                       std::string* error) {
    if (!opened_ || !session_factory_) {
        if (error) *error = "Channel not opened or session factory not set";
        return -1;
    }

    auto session = session_factory_();
    if (!session) {
        if (error) *error = "Failed to create session";
        return -1;
    }

    // 直接调用 Session 的 ExecuteQueryArrow
    return session->ExecuteQueryArrow(query, batches, error);
}

int DatabaseChannel::WriteArrowBatches(const char* table,
                                       const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                                       std::string* error) {
    if (!opened_ || !session_factory_) {
        if (error) *error = "Channel not opened or session factory not set";
        return -1;
    }

    auto session = session_factory_();
    if (!session) {
        if (error) *error = "Failed to create session";
        return -1;
    }

    // 直接调用 Session 的 WriteArrowBatches
    return session->WriteArrowBatches(table, batches, error);
}

}  // namespace database
}  // namespace flowsql
