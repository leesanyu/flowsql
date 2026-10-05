// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "netadapter_plugin.h"
#include <net/if.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <random>
namespace flowsql::channels::netadapter {
struct NetAdapterPlugin::Channel final : IBlockStreamChannel {
    std::string name, option;
    CaptureBackendConfigV1 config;
    uint64_t domain = 0;
    std::atomic<uint32_t> leases{0};
    const char* Category() override { return "netadapter"; }
    const char* Name() override { return name.c_str(); }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return "packet"; }
    int Open() override { return ENOTSUP; }
    int Close() override { return 0; }
    int Flush() override { return 0; }
    bool IsOpened() const override { return false; }
    BlockPollEvent PollBlock(int) override { return {BlockPollEvent::kError, nullptr, ENOTSUP}; }
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>&) override { return EINVAL; }
    void Cancel() override {}
    bool IsFinished() const override { return false; }
};
struct NetAdapterPlugin::Lease {
    std::shared_ptr<Channel> channel;
    std::shared_ptr<ICaptureBackendSessionV1> backend;
    ~Lease() { --channel->leases; }
};
int NetAdapterPlugin::Option(const char* option) {
    if (!option || !*option) return 0;
    rapidjson::Document doc;
    doc.Parse(option);
    if (doc.HasParseError() || !doc.IsObject() || doc.MemberCount() != 1 || !doc.HasMember("db_path") ||
        !doc["db_path"].IsString() || !doc["db_path"].GetStringLength())
        return EINVAL;
    path_ = doc["db_path"].GetString();
    return 0;
}
int NetAdapterPlugin::Load(IQuerier* querier) {
    querier_ = querier;
    return querier ? 0 : EINVAL;
}
int NetAdapterPlugin::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) return EALREADY;
    const auto parent = std::filesystem::path(path_).parent_path();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) return EIO;
    }
    if (sqlite3_open_v2(path_.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK)
        return EIO;
    sqlite3_busy_timeout(db_, 5000);
    if (sqlite3_exec(db_,
                     "CREATE TABLE IF NOT EXISTS netadapter_channels(name TEXT PRIMARY KEY,options TEXT NOT "
                     "NULL,domain INTEGER NOT NULL UNIQUE)",
                     nullptr, nullptr, nullptr) != SQLITE_OK)
        return EIO;
    protocol_ = static_cast<IProtocol*>(querier_->First(IID_PROTOCOL));
    if (!protocol_) return ENODEV;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT name,options,domain FROM netadapter_channels", -1, &statement, nullptr) !=
        SQLITE_OK)
        return EIO;
    int step;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
        auto channel = std::make_shared<Channel>();
        channel->name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        std::string error;
        channel->domain = sqlite3_column_int64(statement, 2);
        if (!channel->domain || ParseNetAdapterConfig(reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
                                                      &channel->config, &channel->option, &error) != 0) {
            sqlite3_finalize(statement);
            return EINVAL;
        }
        channels_.emplace(channel->name, channel);
    }
    sqlite3_finalize(statement);
    if (step != SQLITE_DONE) return EIO;
    started_ = true;
    return 0;
}
int NetAdapterPlugin::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : channels_)
        if (entry.second->leases) return EBUSY;
    started_ = false;
    return 0;
}
int NetAdapterPlugin::Unload() {
    if (Stop() != 0) return EBUSY;
    std::lock_guard<std::mutex> lock(mutex_);
    channels_.clear();
    if (db_ && sqlite3_close(db_) != SQLITE_OK) return EBUSY;
    db_ = nullptr;
    querier_ = nullptr;
    protocol_ = nullptr;
    return 0;
}
ICaptureBackendProviderV1* NetAdapterPlugin::Provider(const std::string& backend) const {
    ICaptureBackendProviderV1* result = nullptr;
    if (querier_)
        querier_->Traverse(IID_CAPTURE_BACKEND_PROVIDER_V1, [&](void* value) {
            auto* provider = static_cast<ICaptureBackendProviderV1*>(value);
            if (provider && backend == provider->Backend()) {
                result = provider;
                return 1;
            }
            return 0;
        });
    return result;
}
int NetAdapterPlugin::Save(const std::string& name, const std::string& option, uint64_t domain, bool remove) {
    sqlite3_stmt* statement = nullptr;
    const char* sql =
        remove
            ? "DELETE FROM netadapter_channels WHERE name=?"
            : "INSERT INTO netadapter_channels VALUES(?,?,?) ON CONFLICT(name) DO UPDATE SET options=excluded.options";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) return EIO;
    sqlite3_bind_text(statement, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    if (!remove) {
        sqlite3_bind_text(statement, 2, option.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 3, domain);
    }
    const int rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    return rc == SQLITE_DONE ? 0 : EIO;
}
int NetAdapterPlugin::AddChannel(const std::string& type, const std::string& name, const std::string& option) {
    if (type != "netadapter") return ENOTSUP;
    if (name.empty() || name.find_first_of(". /\\\t\r\n") != std::string::npos) return EINVAL;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return EPIPE;
    if (channels_.count(name)) return EEXIST;
    auto channel = std::make_shared<Channel>();
    std::string error;
    if (ParseNetAdapterConfig(option, &channel->config, &channel->option, &error) != 0) return EINVAL;
    if (!Provider(channel->config.backend)) return ENODEV;
    channel->name = name;
    channel->domain = (uint64_t(std::random_device{}()) << 32 | std::random_device{}()) & INT64_MAX;
    if (!channel->domain) channel->domain = 1;
    const int rc = Save(name, channel->option, channel->domain, false);
    if (rc) return rc;
    channels_.emplace(name, channel);
    return 0;
}
int NetAdapterPlugin::ModifyChannel(const std::string& type, const std::string& name, const std::string& option) {
    if (type != "netadapter") return ENOTSUP;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(name);
    if (it == channels_.end()) return ENOENT;
    if (it->second->leases) return EBUSY;
    CaptureBackendConfigV1 config;
    std::string normalized, error;
    if (ParseNetAdapterConfig(option, &config, &normalized, &error) != 0) return EINVAL;
    if (!Provider(config.backend)) return ENODEV;
    const int rc = Save(name, normalized, it->second->domain, false);
    if (rc) return rc;
    it->second->config = std::move(config);
    it->second->option = std::move(normalized);
    return 0;
}
int NetAdapterPlugin::RemoveChannel(const std::string& type, const std::string& name) {
    if (type != "netadapter") return ENOTSUP;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(name);
    if (it == channels_.end()) return ENOENT;
    if (it->second->leases) return EBUSY;
    const int rc = Save(name, {}, 0, true);
    if (rc) return rc;
    channels_.erase(it);
    return 0;
}
IBlockStreamChannel* NetAdapterPlugin::Get(const char* type, const char* name) {
    if (!type || !name || std::string(type) != "netadapter") return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(name);
    return it == channels_.end() ? nullptr : it->second.get();
}
void NetAdapterPlugin::List(std::function<void(const char*, const char*, IBlockStreamChannel*)> callback) {
    std::vector<std::shared_ptr<Channel>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : channels_) snapshot.push_back(item.second);
    }
    for (const auto& channel : snapshot) callback("netadapter", channel->name.c_str(), channel.get());
}
void NetAdapterPlugin::QueryChannels(
    std::function<void(const std::string&, const std::string&, const std::string&, const std::string&)> callback) {
    struct Row {
        std::string name, option, status;
    };
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : channels_) {
            rapidjson::Document doc;
            doc.Parse(item.second->option.c_str());
            auto& a = doc.GetAllocator();
            doc.AddMember("observation_domain_id", item.second->domain, a);
            doc.AddMember("budget_bytes", item.second->config.buffer_bytes, a);
            const auto limits = DefaultSourceSet();
            doc.AddMember("max_packets_per_batch", limits.limits.max_packets_per_batch, a);
            doc.AddMember("max_bytes_per_batch", limits.limits.max_bytes_per_batch, a);
            doc.AddMember("max_wait_ms", limits.limits.max_wait_ms, a);
            doc.AddMember("max_outstanding_batches", limits.limits.max_outstanding_batches, a);
            doc.AddMember("max_inspected_packets_per_poll", limits.max_inspected_packets_per_poll, a);
            doc.AddMember("max_inspected_bytes_per_poll", limits.max_inspected_bytes_per_poll, a);
            doc.AddMember("max_poll_work_ms", limits.max_poll_work_ms, a);
            doc.AddMember("input_packet_quantum", kInputPacketQuantum, a);
            doc.AddMember("input_byte_quantum", kInputByteQuantum, a);
            doc.AddMember("shared_envelope_min_bytes", kSharedEnvelopeBytes, a);
            doc.AddMember("minimum_input_bytes", kMinimumInputBytes, a);
            doc.AddMember("execution", rapidjson::Value("single runner, fair nonblocking inputs", a), a);
            rapidjson::Value active(rapidjson::kArrayType);
            for (const auto& reader : readers_)
                if (reader.second->Name() == item.first) {
                    rapidjson::Document diagnostic;
                    diagnostic.Parse(reader.second->Diagnostics().c_str());
                    active.PushBack(rapidjson::Value(diagnostic, a), a);
                }
            doc.AddMember("readers", active, a);
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            doc.Accept(writer);
            rows.push_back({item.first, buffer.GetString(), item.second->leases ? "busy" : "ready"});
        }
    }
    for (const auto& row : rows) callback("netadapter", row.name, row.option, row.status);
}
void NetAdapterPlugin::DescribeChannelTypes(
    std::function<void(const BlockStreamChannelTypeDescriptorV1&)> callback) const {
    BlockStreamChannelTypeDescriptorV1 descriptor;
    descriptor.channel_type = "netadapter";
    descriptor.display_name = "NetAdapter 网卡采集";
    descriptor.allowed_roles = {"source"};
    descriptor.option_schema = {
        {"backend",
         "enum",
         true,
         "af_packet",
         {"af_packet", "pfring_classic", "af_xdp_copy_skb"},
         0,
         0,
         false,
         false,
         "采集后端"},
        {"interfaces", "array", true, "[]", {}, 0, 0, false, false, "采集网卡"},
        {"promiscuous", "bool", false, "true", {}, 0, 0, false, false, "混杂模式"},
        {"snaplen", "int", false, "65535", {}, 1, 65535, true, false, "截断长度（字节）"},
        {"buffer_mib", "int", false, "64", {}, 1, 4294967295LL, true, false, "采集缓冲（MiB）"},
    };
    callback(descriptor);
}
int NetAdapterPlugin::CreateReader(const BlockStreamReaderConfigV1& request, IBlockStreamChannel** output) {
    if (!output) return EINVAL;
    *output = nullptr;
    if (!request.source_category || std::string(request.source_category) != "netadapter") return ENOTSUP;
    if (request.contract_version != 1 || !request.source_name || !request.task_id || !*request.task_id ||
        !request.pushed_filter_plan_json)
        return EINVAL;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return EPIPE;
    auto it = channels_.find(request.source_name);
    if (it == channels_.end()) return ENOENT;
    if (it->second->leases) return EBUSY;
    auto* provider = Provider(it->second->config.backend);
    if (!provider) return ENODEV;
    auto filter = std::make_shared<packet::PcapFilterPlan>();
    std::string error;
    if (pcapfile::CompilePcapFilterPlanJson(request.pushed_filter_plan_json, filter.get(), &error) != 0) return EINVAL;
    std::shared_ptr<ICaptureBackendSessionV1> backend;
    const int rc = provider->Open(it->second->config, &backend, &error);
    if (rc) return rc;
    if (!backend) return EFAULT;
    std::set<std::string> covered;
    uint64_t bytes = 0;
    for (const auto& input : backend->Inputs()) {
        covered.insert(input.interface_name);
        bytes += input.buffer_bytes;
    }
    if (covered != std::set<std::string>(it->second->config.interfaces.begin(), it->second->config.interfaces.end()) ||
        bytes + kSharedEnvelopeBytes > it->second->config.buffer_bytes)
        return EINVAL;
    ++it->second->leases;
    auto lease = std::make_shared<Lease>();
    lease->channel = it->second;
    lease->backend = backend;
    auto reader = std::make_unique<NetAdapterReader>(it->first, it->second->config, it->second->domain, ++generation_,
                                                     backend, protocol_, lease, filter);
    const int open_rc = reader->Open();
    if (open_rc) return open_rc;
    *output = reader.get();
    readers_.emplace(*output, std::move(reader));
    return 0;
}
void NetAdapterPlugin::ReleaseReader(IBlockStreamChannel* reader) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = readers_.find(reader);
    if (it == readers_.end()) return;
    it->second->Cancel();
    readers_.erase(it);
}
int NetAdapterPlugin::Resolve(const FilterDomainResolveRequestV1& request, FilterDomainResolveResultV1* result) const {
    if (!request.target_category || std::string(request.target_category) != "netadapter") return ENOTSUP;
    auto next = request;
    next.target_category = "pcapfile";
    return pcapfile::PcapFilterDomainResolver().Resolve(next, result);
}
int NetAdapterPlugin::EvaluatePushdown(const FilterPushdownRequestV1& request, FilterPushdownResultV1* result) const {
    if (!request.target_category || std::string(request.target_category) != "netadapter") return ENOTSUP;
    if (!result || request.contract_version != 1 || !request.canonical_plan_json) return EINVAL;
    result->diagnostic = "exact typed packet plan; unsupported nodes stay residual";
    return pcapfile::SelectCompilablePcapFilterNodes(request.canonical_plan_json, request.candidate_node_ids,
                                                     &result->accepted_node_ids, &result->diagnostic);
}
}  // namespace flowsql::channels::netadapter
