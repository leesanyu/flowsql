#include "database_channel.h"

#include "capability_interfaces.h"

#include <cstdio>

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
    if (!driver_) return -1;

    // 初始化连接（连接池在 Connect 中初始化）
    int ret = driver_->Connect({});
    if (ret == 0) opened_ = true;
    return ret;
}

int DatabaseChannel::Close() {
    if (!opened_) return 0;
    if (driver_) driver_->Disconnect();
    opened_ = false;
    return 0;
}

int DatabaseChannel::CreateReader(const char* query, IBatchReader** reader) {
    if (!opened_) {
        printf("DatabaseChannel::CreateReader: channel not opened (%s.%s)\n", type_.c_str(), name_.c_str());
        return -1;
    }
    if (!session_factory_) {
        printf("DatabaseChannel::CreateReader: session factory not set (%s.%s)\n", type_.c_str(), name_.c_str());
        return -1;
    }

    // 使用 Session 创建 Reader
    auto session = session_factory_();
    if (!session) {
        printf("DatabaseChannel::CreateReader: failed to create session (%s.%s)\n", type_.c_str(), name_.c_str());
        return -1;
    }

    // 检查 Session 是否支持 IBatchReadable
    auto* batch_readable = dynamic_cast<IBatchReadable*>(session.get());
    if (!batch_readable) {
        printf("DatabaseChannel: session does not support batch reading\n");
        return -1;
    }

    printf("DatabaseChannel::CreateReader: executing query on %s.%s: %s\n",
           type_.c_str(), name_.c_str(), query);
    int ret = batch_readable->CreateReader(query, reader);
    if (ret != 0) {
        printf("DatabaseChannel::CreateReader: CreateReader failed with code %d\n", ret);
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
        printf("DatabaseChannel: session does not support batch writing\n");
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
    if (!opened_) {
        printf("DatabaseChannel::CreateArrowReader: channel not opened (%s.%s)\n", type_.c_str(), name_.c_str());
        return -1;
    }
    if (!session_factory_) {
        printf("DatabaseChannel::CreateArrowReader: session factory not set (%s.%s)\n", type_.c_str(), name_.c_str());
        return -1;
    }

    // 使用 Session 创建 Arrow Reader
    auto session = session_factory_();
    if (!session) {
        printf("DatabaseChannel::CreateArrowReader: failed to create session (%s.%s)\n", type_.c_str(), name_.c_str());
        return -1;
    }

    // 检查 Session 是否支持 IArrowReadable
    auto* arrow_readable = dynamic_cast<IArrowReadable*>(session.get());
    if (!arrow_readable) {
        printf("DatabaseChannel: session does not support arrow reading (row-based database)\n");
        return -1;
    }

    // 创建适配器包装器（将 IArrowReadable 转换为 IArrowReader）
    class ArrowReaderAdapter : public IArrowReader {
    public:
        ArrowReaderAdapter(std::shared_ptr<IDbSession> session,
                           std::shared_ptr<arrow::Schema> schema)
            : session_(session), schema_(std::move(schema)) {}

        int ExecuteQueryArrow(const char* query,
                              std::vector<std::shared_ptr<arrow::RecordBatch>>* batches,
                              std::string* error) override {
            auto* arrow_readable = dynamic_cast<IArrowReadable*>(session_.get());
            if (!arrow_readable) {
                *error = "Session does not support Arrow reading";
                return -1;
            }
            return arrow_readable->ExecuteQueryArrow(query, batches, error);
        }

        std::shared_ptr<arrow::Schema> GetSchema() override {
            return schema_;
        }

        const char* GetLastError() override {
            return last_error_.c_str();
        }

        void Release() override {
            delete this;
        }

    private:
        std::shared_ptr<IDbSession> session_;
        std::shared_ptr<arrow::Schema> schema_;
        std::string last_error_;
    };

    // 对于行式数据库，返回不支持的错误
    *reader = nullptr;
    printf("DatabaseChannel::CreateArrowReader: %s\n", "Arrow reading not supported for row-based databases");
    return -1;
}

int DatabaseChannel::CreateArrowWriter(const char* table, IArrowWriter** writer) {
    if (!opened_ || !session_factory_) {
        return -1;
    }

    // 使用 Session 创建 Arrow Writer
    auto session = session_factory_();
    if (!session) {
        return -1;
    }

    // 检查 Session 是否支持 IArrowWritable
    auto* arrow_writable = dynamic_cast<IArrowWritable*>(session.get());
    if (!arrow_writable) {
        printf("DatabaseChannel: session does not support arrow writing (row-based database)\n");
        *writer = nullptr;
        return -1;
    }

    // 对于行式数据库，返回不支持的错误
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
