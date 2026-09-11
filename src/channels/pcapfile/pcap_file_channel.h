// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_CHANNELS_PCAPFILE_PCAP_FILE_CHANNEL_H_
#define _FLOWSQL_CHANNELS_PCAPFILE_PCAP_FILE_CHANNEL_H_

#include <common/iplugin.h>
#include <framework/interfaces/iblock_stream_channel.h>
#include <framework/interfaces/iblock_stream_factory.h>
#include <framework/interfaces/iblock_stream_manager.h>
#include <framework/interfaces/iblock_stream_reader.h>
#include <framework/interfaces/ifilter_pushdown.h>
#include <plugins/npi/iprotocol.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "packet_filter_domain.h"
#include "pcap_file_channel_store.h"

namespace flowsql::channels::pcapfile {

enum class PcapReplayMode { kFast, kTimestamp };

using PcapReplayWaiter = std::function<void(std::chrono::nanoseconds)>;

struct PcapFileSourceConfig {
    std::string path;
    std::string format = "auto";
    uint32_t batch_packets = 256;
    PcapReplayMode replay_mode = PcapReplayMode::kFast;
    uint32_t replay_speed_milli = 1000;
};

int ParsePcapFileSourceConfig(const std::string& option,
                              PcapFileSourceConfig* out,
                              std::string* normalized,
                              std::string* error);

class PcapFileReader;

class PcapFileChannel final : public IBlockStreamChannel {
 public:
    PcapFileChannel(std::string name, PcapFileSourceConfig config, IProtocol* protocol,
                    PcapReplayWaiter replay_waiter = {},
                    std::shared_ptr<const packet::PcapFilterPlan> filter_plan = {});
    ~PcapFileChannel() override;

    const char* Category() override { return "pcapfile"; }
    const char* Name() override { return name_.c_str(); }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return "packet"; }
    int Open() override;
    int Close() override;
    bool IsOpened() const override;
    int Flush() override { return 0; }
    BlockPollEvent PollBlock(int timeout_ms = 100) override;
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& block) override;
    void Cancel() override;
    bool IsFinished() const override;

    size_t OutstandingBatchCount() const;
    bool IsBusy() const;
    const std::string& Option() const { return normalized_option_; }
    void SetNormalizedOption(std::string option) { normalized_option_ = std::move(option); }
    PacketHeaderFilterResult EvaluateHeaderFilter(const packet::PacketMeta& meta) const;
    bool EvaluateDecodedFilter(const packet::PacketMeta& meta,
                               const packet::PacketLayerInfo& layer) const;

 private:
    std::string name_;
    PcapFileSourceConfig config_;
    IProtocol* protocol_ = nullptr;  // non-owning, provider lifetime owned by host
    std::string normalized_option_;
    std::unique_ptr<PcapFileReader> reader_;
    mutable std::mutex mutex_;
    std::condition_variable state_cv_;
    std::unordered_set<const arrow::RecordBatch*> outstanding_;
    bool opened_ = false;
    bool cancelled_ = false;
    bool finished_ = false;
    bool eof_sent_ = false;
    bool error_sent_ = false;
    int error_code_ = 0;
    std::string error_message_;
    int64_t previous_timestamp_ns_ = 0;
    bool have_previous_timestamp_ = false;
    uint32_t replay_remainder_ = 0;
    PcapReplayWaiter replay_waiter_;
    std::shared_ptr<const packet::PcapFilterPlan> filter_plan_;
    bool poll_in_progress_ = false;
};

class PcapFilePlugin final : public IPlugin,
                             public IBlockStreamFactory,
                             public IBlockStreamManager,
                             public IBlockStreamReaderFactoryV1,
                             public IFilterDomainResolverV1,
                             public IFilterPushdownV1 {
 public:
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

    IBlockStreamChannel* Get(const char* type, const char* name) override;
    void List(std::function<void(const char*, const char*, IBlockStreamChannel*)> callback) override;
    int AddChannel(const std::string& type, const std::string& name, const std::string& option) override;
    int ModifyChannel(const std::string& type, const std::string& name, const std::string& option) override;
    int RemoveChannel(const std::string& type, const std::string& name) override;
    void QueryChannels(std::function<void(const std::string&, const std::string&, const std::string&, const std::string&)> callback) override;
    int CreateReader(const BlockStreamReaderConfigV1& config,
                     IBlockStreamChannel** reader) override;
    void ReleaseReader(IBlockStreamChannel* reader) override;
    int Resolve(const FilterDomainResolveRequestV1& request,
                FilterDomainResolveResultV1* result) const override;
    int EvaluatePushdown(const FilterPushdownRequestV1& request,
                         FilterPushdownResultV1* result) const override;

 private:
    struct ReaderSession {
        std::string task_id;
        std::string source_category;
        std::string source_name;
        std::string pushed_filter_plan_json;
        std::shared_ptr<PcapFileChannel> channel;
    };

    static std::string MakeKey(const std::string& type, const std::string& name);
    int StopChannelsLocked();
    int BuildChannel(const std::string& name, const std::string& option,
                     std::shared_ptr<PcapFileChannel>* out, std::string* error,
                     std::shared_ptr<const packet::PcapFilterPlan> filter_plan = {});

    IQuerier* querier_ = nullptr;
    IProtocol* protocol_ = nullptr;
    PcapFilterDomainResolver filter_domain_resolver_;
    std::string plugin_option_;
    std::string db_path_;
    PcapFileChannelStore store_;
    bool store_open_ = false;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> options_;
    std::unordered_map<std::string, std::shared_ptr<PcapFileChannel>> channels_;
    std::unordered_map<IBlockStreamChannel*, ReaderSession> readers_;
};

}  // namespace flowsql::channels::pcapfile

#endif  // _FLOWSQL_CHANNELS_PCAPFILE_PCAP_FILE_CHANNEL_H_
