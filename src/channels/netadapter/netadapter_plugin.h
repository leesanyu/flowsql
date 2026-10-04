// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#ifndef FLOWSQL_CHANNELS_NETADAPTER_PLUGIN_H_
#define FLOWSQL_CHANNELS_NETADAPTER_PLUGIN_H_
#include <channels/pcapfile/packet_filter_domain.h>
#include <common/iplugin.h>
#include <framework/interfaces/iblock_stream_factory.h>
#include <framework/interfaces/iblock_stream_manager.h>
#include <framework/interfaces/iblock_stream_reader.h>
#include <framework/interfaces/ifilter_pushdown.h>
#include <sqlite3.h>
#include "netadapter_reader.h"
namespace flowsql::channels::netadapter {
class NetAdapterPlugin final : public IPlugin,
                               public IBlockStreamFactory,
                               public IBlockStreamManager,
                               public IBlockStreamReaderFactoryV1,
                               public IFilterDomainResolverV1,
                               public IFilterPushdownV1 {
 public:
    int Option(const char* option) override;
    int Load(IQuerier* querier) override;
    int Start() override;
    int Stop() override;
    int Unload() override;
    IBlockStreamChannel* Get(const char* type, const char* name) override;
    void List(std::function<void(const char*, const char*, IBlockStreamChannel*)> callback) override;
    int AddChannel(const std::string& type, const std::string& name, const std::string& option) override;
    int ModifyChannel(const std::string& type, const std::string& name, const std::string& option) override;
    int RemoveChannel(const std::string& type, const std::string& name) override;
    void QueryChannels(
        std::function<void(const std::string&, const std::string&, const std::string&, const std::string&)> callback)
        override;
    int CreateReader(const BlockStreamReaderConfigV1& config, IBlockStreamChannel** reader) override;
    void ReleaseReader(IBlockStreamChannel* reader) override;
    int Resolve(const FilterDomainResolveRequestV1& request, FilterDomainResolveResultV1* result) const override;
    int EvaluatePushdown(const FilterPushdownRequestV1& request, FilterPushdownResultV1* result) const override;

 private:
    struct Channel;
    struct Lease;
    int Save(const std::string& name, const std::string& option, uint64_t domain, bool remove);
    ICaptureBackendProviderV1* Provider(const std::string& backend) const;
    std::string path_ = "./netadapter.db";
    sqlite3* db_ = nullptr;
    IQuerier* querier_ = nullptr;
    IProtocol* protocol_ = nullptr;
    bool started_ = false;
    uint64_t generation_ = 0;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Channel>> channels_;
    std::map<IBlockStreamChannel*, std::unique_ptr<NetAdapterReader>> readers_;
};
}  // namespace flowsql::channels::netadapter
#endif
