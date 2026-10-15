// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "kafka_reader.h"
#include <cerrno>
#include <cstring>
#include <map>
#include "kafka_plugin.h"

namespace flowsql::channels::kafka {
StructuredReader::StructuredReader(std::string name, StructuredConfig config,
                                   std::shared_ptr<IKafkaClientSessionV1> session)
    : name_(std::move(name)), config_(std::move(config)), session_(std::move(session)) {
    arrow_schema_ = StructuredSchema(config_);
    schema_ = arrow_schema_->ToString(true);
}
int StructuredReader::Open() {
    if (opened_) return EALREADY;
    if (cancelled_ || !session_) return ECANCELED;
    void* capability = nullptr;
    int rc = session_->QueryInterface(IID_KAFKA_CONSUMER_SESSION_V1, &capability);
    if (rc || !capability) return rc ? rc : ENOTSUP;
    consumer_ = static_cast<IKafkaConsumerSessionV1*>(capability);
    rc = consumer_->StartConsumer(config_.offset_reset.c_str(), config_.max_message_bytes);
    if (!rc) opened_ = true;
    return rc;
}
int StructuredReader::Close() {
    Cancel();
    outstanding_.reset();
    consumer_ = nullptr;
    opened_ = false;
    session_.reset();
    return 0;
}
void StructuredReader::Cancel() {
    cancelled_ = true;
    if (session_) session_->Cancel();
}
BlockPollEvent StructuredReader::Error(int error) {
    terminal_error_ = error;
    terminal_ = true;
    return {BlockPollEvent::kError, nullptr, error};
}
BlockPollEvent StructuredReader::PollBlock(int timeout_ms) {
    if (cancelled_) return {BlockPollEvent::kCancelled, nullptr, ECANCELED};
    if (terminal_) return {BlockPollEvent::kError, nullptr, terminal_error_};
    if (!opened_ || timeout_ms < 0 || timeout_ms > 30000) return Error(EINVAL);
    if (outstanding_) return {BlockPollEvent::kError, nullptr, EBUSY};
    std::vector<KafkaMessageV1> messages;
    std::map<int32_t, int64_t> next;
    size_t bytes = 0;
    do {
        KafkaMessageV1 message;
        int rc = consumer_->PollMessage(messages.empty() ? timeout_ms : 0, &message);
        if (rc == EAGAIN) break;
        if (rc == ECANCELED) return {BlockPollEvent::kCancelled, nullptr, rc};
        if (rc) return Error(rc);
        if (message.offset < 0 || message.offset == INT64_MAX || message.partition < 0) return Error(EPROTO);
        next[message.partition] = std::max(next[message.partition], message.offset + 1);
        bytes += message.value.size() + message.key.size();
        messages.push_back(std::move(message));
    } while (messages.size() < config_.max_batch_rows && bytes < 16777216);
    if (messages.empty()) return {BlockPollEvent::kTimeout, nullptr, 0};
    auto decoded = DecodeStructured(config_, messages);
    if (!decoded.ok()) return Error(EILSEQ);
    outstanding_ = *decoded;
    offsets_.clear();
    for (const auto& [partition, offset] : next) offsets_.push_back({partition, offset});
    acknowledged_ = false;
    return {BlockPollEvent::kData, outstanding_, 0};
}
int StructuredReader::AcknowledgeBlock(const std::shared_ptr<arrow::RecordBatch>& block) {
    if (cancelled_) return ECANCELED;
    if (!block || block != outstanding_ || acknowledged_) return EINVAL;
    int rc = config_.commit_policy == "none" ? 0 : consumer_->CommitOffsets(offsets_, 3000);
    if (rc) {
        Error(rc);
        return rc;
    }
    acknowledged_ = true;
    return 0;
}
int StructuredReader::ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& block) {
    if (!block || block != outstanding_) return EINVAL;
    if (!acknowledged_) Error(ECANCELED);
    outstanding_.reset();
    offsets_.clear();
    return 0;
}
namespace {
class Definition final : public IBlockStreamChannel, public IBlockStreamSchemaV1 {
 public:
    Definition(std::string name, std::shared_ptr<arrow::Schema> schema)
        : name_(std::move(name)), schema_(schema->ToString(true)), arrow_schema_(std::move(schema)) {}
    const char* Category() override { return "kafka"; }
    const char* Name() override { return name_.c_str(); }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return schema_.c_str(); }
    std::shared_ptr<arrow::Schema> ArrowSchema() const override { return arrow_schema_; }
    int Open() override { return ENOTSUP; }
    int Close() override { return 0; }
    int Flush() override { return 0; }
    bool IsOpened() const override { return false; }
    BlockPollEvent PollBlock(int) override { return {BlockPollEvent::kError, nullptr, ENOTSUP}; }
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>&) override { return EINVAL; }
    void Cancel() override {}
    bool IsFinished() const override { return false; }

 private:
    std::string name_, schema_;
    std::shared_ptr<arrow::Schema> arrow_schema_;
};
}  // namespace
IBlockStreamChannel* Plugin::Get(const char* type, const char* name) {
    if (!type || std::strcmp(type, "kafka") || !name) return nullptr;
    try {
        auto rows = store_.Query("channels", name);
        if (rows["items"].Empty()) return nullptr;
        auto& row = rows["items"][0];
        if (Text(row, "payload_kind") != "structured" || Text(row, "role") == "sink" ||
            !row.HasMember("payload_options") || !row["payload_options"].HasMember("source") ||
            row["payload_options"]["source"].ObjectEmpty())
            return nullptr;
        auto config = ParseStructuredConfig(row["payload_options"]["source"]);
        const auto key = std::string(name) + ":" + std::to_string(row["revision"].GetUint64());
        std::lock_guard<std::mutex> lock(readers_mutex_);
        auto& definition = definitions_[key];
        if (!definition) definition = std::make_unique<Definition>(name, StructuredSchema(config));
        return definition.get();
    } catch (...) {
        return nullptr;
    }
}
void Plugin::List(std::function<void(const char*, const char*, IBlockStreamChannel*)> callback) {
    std::string after;
    while (true) {
        auto rows = store_.Query("channels", "", after, 100);
        for (const auto& row : rows["items"].GetArray()) {
            auto name = Text(row, "name");
            if (auto* channel = Get("kafka", name.c_str())) callback("kafka", name.c_str(), channel);
            after = name;
        }
        if (rows["items"].Size() < 100) break;
    }
}
void Plugin::DescribeChannelTypes(std::function<void(const BlockStreamChannelTypeDescriptorV1&)> callback) const {
    BlockStreamChannelTypeDescriptorV1 descriptor;
    descriptor.channel_type = "kafka";
    descriptor.display_name = "Kafka 结构化 JSON";
    descriptor.allowed_roles = {"source"};
    callback(descriptor);
}
int Plugin::CreateReader(const BlockStreamReaderConfigV1& config, IBlockStreamChannel** output) {
    if (!output) return EINVAL;
    *output = nullptr;
    if (!config.source_category || std::strcmp(config.source_category, "kafka")) return ENOTSUP;
    if (config.contract_version != 1 || !config.source_name || !config.task_id || !*config.task_id ||
        !config.pushed_filter_plan_json)
        return EINVAL;
    try {
        auto plan = Parse(config.pushed_filter_plan_json);
        if (!plan.HasMember("root") || !plan["root"].IsNull()) return ENOTSUP;
    } catch (...) {
        return EINVAL;
    }
    {
        std::lock_guard<std::mutex> lock(readers_mutex_);
        if (!reader_names_.insert(config.source_name).second) return EBUSY;
    }
    IKafkaClientSessionV1* raw = nullptr;
    std::string error;
    int rc = OpenSession(config.source_name, "source", &raw, &error);
    std::shared_ptr<IKafkaClientSessionV1> session(raw, [this](IKafkaClientSessionV1* value) {
        if (value) ReleaseSession(value);
    });
    try {
        if (!rc && raw->Snapshot().payload_kind != "structured") rc = ENOTSUP;
        if (!rc) {
            const auto& options = static_cast<Client*>(raw)->SourceOptions();
            if (options.empty())
                rc = ENOTSUP;
            else {
                auto reader = std::make_unique<StructuredReader>(config.source_name,
                                                                 ParseStructuredConfig(Parse(options)), session);
                rc = reader->Open();
                if (!rc) {
                    std::lock_guard<std::mutex> lock(readers_mutex_);
                    *output = reader.get();
                    readers_[reader.get()] = std::move(reader);
                    return 0;
                }
            }
        }
    } catch (...) {
        rc = EINVAL;
    }
    std::lock_guard<std::mutex> lock(readers_mutex_);
    reader_names_.erase(config.source_name);
    return rc;
}
void Plugin::ReleaseReader(IBlockStreamChannel* reader) {
    std::unique_ptr<IBlockStreamChannel> owner;
    {
        std::lock_guard<std::mutex> lock(readers_mutex_);
        auto i = readers_.find(reader);
        if (i == readers_.end()) return;
        reader_names_.erase(reader->Name());
        owner = std::move(i->second);
        readers_.erase(i);
    }
}
}  // namespace flowsql::channels::kafka
