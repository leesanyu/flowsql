#ifndef _FLOWSQL_FRAMEWORK_CORE_MEMORY_CHANNEL_H_
#define _FLOWSQL_FRAMEWORK_CORE_MEMORY_CHANNEL_H_

#include <framework/core/dataframe.h>
#include <framework/interfaces/idataframe_channel.h>

#include <string>

namespace flowsql {

// MemoryChannel — IDataFrameChannel 的简单内存实现
// Read() 快照语义，Write() 替换语义
// 不继承 IPlugin，作为公共类直接构造使用
class MemoryChannel : public IDataFrameChannel {
 public:
    MemoryChannel() = default;
    ~MemoryChannel() override = default;

    // IChannel — 身份
    const char* Catelog() override { return catelog_.c_str(); }
    const char* Name() override { return name_.c_str(); }
    const char* Type() override { return ChannelType::kDataFrame; }
    const char* Schema() override { return "[]"; }

    // IChannel — 生命周期
    int Open() override;
    int Close() override;
    bool IsOpened() const override { return opened_; }
    int Flush() override { return 0; }

    // IDataFrameChannel — 数据读写
    int Write(IDataFrame* df) override;
    int Read(IDataFrame* df) override;

    // 设置通道身份（可选，默认为空字符串）
    void SetIdentity(const std::string& catelog, const std::string& name) {
        catelog_ = catelog;
        name_ = name;
    }

 private:
    bool opened_ = false;
    DataFrame data_;
    std::string catelog_;
    std::string name_;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_MEMORY_CHANNEL_H_
