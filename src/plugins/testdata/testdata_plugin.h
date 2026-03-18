#ifndef _FLOWSQL_PLUGINS_TESTDATA_TESTDATA_PLUGIN_H_
#define _FLOWSQL_PLUGINS_TESTDATA_TESTDATA_PLUGIN_H_

#include <common/iplugin.h>
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>

#include <memory>

namespace flowsql {

// TestDataPlugin — 提供内置测试数据通道（test.data）
// 仅用于开发和演示，生产环境可不加载此插件
class TestDataPlugin : public IPlugin {
 public:
    TestDataPlugin() = default;
    ~TestDataPlugin() override = default;

    int Option(const char* /*arg*/) override { return 0; }
    int Load(IQuerier* /*querier*/) override { return 0; }
    int Unload() override { return 0; }

    int Start() override;
    int Stop() override;

    // pluginregist 时提前创建通道，以便注册到 IRegister
    DataFrameChannel* GetChannel() { return channel_.get(); }
    void InitChannel();

 private:
    std::shared_ptr<DataFrameChannel> channel_;
};

}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_TESTDATA_TESTDATA_PLUGIN_H_
