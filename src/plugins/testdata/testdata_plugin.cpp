#include "testdata_plugin.h"

#include <common/log.h>

namespace flowsql {

void TestDataPlugin::InitChannel() {
    channel_ = std::make_shared<DataFrameChannel>("test", "data");
    channel_->Open();
}

int TestDataPlugin::Start() {
    if (!channel_) return -1;

    DataFrame df;
    df.SetSchema({
        {"src_ip",     DataType::STRING, 0, "源IP"},
        {"dst_ip",     DataType::STRING, 0, "目的IP"},
        {"src_port",   DataType::UINT32, 0, "源端口"},
        {"dst_port",   DataType::UINT32, 0, "目的端口"},
        {"protocol",   DataType::STRING, 0, "协议"},
        {"bytes_sent", DataType::UINT64, 0, "发送字节"},
        {"bytes_recv", DataType::UINT64, 0, "接收字节"},
    });

    df.AppendRow({std::string("192.168.1.10"), std::string("10.0.0.1"),
                  uint32_t(52341), uint32_t(80), std::string("HTTP"),
                  uint64_t(1024), uint64_t(4096)});
    df.AppendRow({std::string("192.168.1.10"), std::string("8.8.8.8"),
                  uint32_t(53421), uint32_t(53), std::string("DNS"),
                  uint64_t(64), uint64_t(128)});
    df.AppendRow({std::string("192.168.1.20"), std::string("172.16.0.5"),
                  uint32_t(44312), uint32_t(443), std::string("HTTPS"),
                  uint64_t(2048), uint64_t(8192)});

    channel_->Write(&df);
    LOG_INFO("TestDataPlugin::Start: test.data ready (%d rows)", df.RowCount());
    return 0;
}

int TestDataPlugin::Stop() {
    if (channel_) {
        channel_->Close();
        channel_.reset();
    }
    return 0;
}

}  // namespace flowsql
