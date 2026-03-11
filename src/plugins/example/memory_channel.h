#ifndef _FLOWSQL_PLUGINS_EXAMPLE_MEMORY_CHANNEL_H_
#define _FLOWSQL_PLUGINS_EXAMPLE_MEMORY_CHANNEL_H_

#include <framework/core/dataframe.h>
#include <framework/interfaces/idataframe_channel.h>
#include <common/iplugin.h>

#include <string>

namespace flowsql {

// MemoryChannel — IDataFrameChannel 的简单内存实现（示例插件）
// Read() 快照语义，Write() 替换语义
class MemoryChannel : public IDataFrameChannel, public IPlugin {
 public:
    MemoryChannel() = default;
    ~MemoryChannel() override = default;

    // IPlugin
    int Load(IQuerier* /* querier */) override { return 0; }
    int Unload() override { return 0; }

    // IChannel — 身份
    const char* Catelog() override { return "example"; }
    const char* Name() override { return "memory"; }
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

 private:
    bool opened_ = false;
    DataFrame data_;
};

}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_EXAMPLE_MEMORY_CHANNEL_H_
