#include "database_channel.h"

#include "capability_interfaces.h"

#include <cstdio>

namespace flowsql {
namespace database {

DatabaseChannel::DatabaseChannel(const std::string& type, const std::string& name,
                                 std::unique_ptr<IDbDriver> driver,
                                 const std::unordered_map<std::string, std::string>& params)
    : type_(type), name_(name), driver_(std::move(driver)), params_(params) {}

DatabaseChannel::~DatabaseChannel() {
    if (opened_) Close();
}

int DatabaseChannel::Open() {
    if (opened_) return 0;
    if (!driver_) return -1;

    int ret = driver_->Connect(params_);
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
    if (!driver_) {
        printf("DatabaseChannel::CreateReader: driver is null (%s.%s)\n", type_.c_str(), name_.c_str());
        return -1;
    }

    // 能力检测：检查驱动是否支持批量读取
    auto* batch_readable = dynamic_cast<IBatchReadable*>(driver_.get());
    if (!batch_readable) {
        printf("DatabaseChannel: driver does not support batch reading\n");
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
    if (!opened_ || !driver_) return -1;

    // 能力检测：检查驱动是否支持批量写入
    auto* batch_writable = dynamic_cast<IBatchWritable*>(driver_.get());
    if (!batch_writable) {
        printf("DatabaseChannel: driver does not support batch writing\n");
        return -1;
    }

    return batch_writable->CreateWriter(table, writer);
}

bool DatabaseChannel::IsConnected() {
    if (!driver_) return false;
    return driver_->IsConnected();
}

}  // namespace database
}  // namespace flowsql
