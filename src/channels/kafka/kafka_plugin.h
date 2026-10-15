// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <channels/pcapfile/packet_filter_domain.h>
#include <common/iplugin.h>
#include <framework/interfaces/iblock_stream_channel_descriptor.h>
#include <framework/interfaces/iblock_stream_factory.h>
#include <framework/interfaces/iblock_stream_reader.h>
#include <framework/interfaces/iresult_sink.h>
#include <framework/interfaces/irouter_handle.h>
#include <memory>
#include <set>
#include "kafka_client.h"

namespace flowsql::channels::kafka {
class Plugin final : public IPlugin,
                     public IKafkaConnectionServiceV1,
                     public IRouterHandle,
                     public IKafkaChannelCatalogV1,
                     public IBlockStreamFactory,
                     public IBlockStreamReaderFactoryV1,
                     public IBlockStreamChannelDescriptorV1,
                     public IResultSinkFactoryV1,
                     public IFilterDomainResolverV1 {
 public:
    int Option(const char* options) override;
    int Load(IQuerier*) override;
    int Start() override;
    int Stop() override;
    int Unload() override;
    int OpenSession(const char* name, const char* usage, IKafkaClientSessionV1** session, std::string* error) override;
    void ReleaseSession(IKafkaClientSessionV1* session) override;
    void EnumRoutes(std::function<void(const RouteItem&)> callback) override;
    ~Plugin() override { Stop(); }
    int CreateWriter(const ResultSinkContextV1&, std::unique_ptr<IResultSinkWriterV1>*) override;
    int DescribeChannel(const char* name, KafkaChannelDescriptionV1* description) override;
    Store& Definitions() { return store_; }
    const Deployment& Environment() const { return deployment_; }
    IBlockStreamChannel* Get(const char* type, const char* name) override;
    void List(std::function<void(const char*, const char*, IBlockStreamChannel*)> callback) override;
    int CreateReader(const BlockStreamReaderConfigV1& config, IBlockStreamChannel** output) override;
    void ReleaseReader(IBlockStreamChannel* reader) override;
    void DescribeChannelTypes(std::function<void(const BlockStreamChannelTypeDescriptorV1&)> callback) const override;
    int Resolve(const FilterDomainResolveRequestV1&, FilterDomainResolveResultV1*) const override;

 private:
    struct Verification {
        std::mutex mutex;
        std::thread worker;
        std::shared_ptr<Client> client;
        bool done = false, cancelled = false, draft = false;
        Json result;
        KafkaBindingSnapshotV1 snapshot;
        std::string target, name, files_stamp;
        std::chrono::steady_clock::time_point completed;
    };
    int32_t Handle(const std::string& operation, const std::string& request, std::string& response);
    Json DefinitionsJson();
    Json StartTest(const Json& request);
    Json Test(const Json& request, bool cancel);
    void StopTests();
    void CheckConnection(const rapidjson::Value& connection);
    void PruneTests();
    void AttachVerification(Json& result, const std::string& kind);
    std::string FilesStamp() const;
    std::mutex tests_mutex_;
    std::condition_variable tests_changed_;
    std::thread tests_cleanup_;
    std::map<std::string, std::shared_ptr<Verification>> tests_;
    uint64_t next_test_ = 0;
    bool tests_stopping_ = false;
    Json capabilities_;
    Json options_;
    Store store_;
    Deployment deployment_;
    IQuerier* querier_ = nullptr;
    std::mutex sessions_mutex_;
    std::condition_variable sessions_drained_;
    std::set<Client*> sessions_;
    bool stopping_ = false;
    size_t pending_ = 0;
    std::mutex readers_mutex_;
    std::set<std::string> reader_names_;
    std::map<std::string, std::unique_ptr<IBlockStreamChannel>> definitions_;
    std::map<IBlockStreamChannel*, std::unique_ptr<IBlockStreamChannel>> readers_;
};
}  // namespace flowsql::channels::kafka
