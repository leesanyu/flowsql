// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <arpa/inet.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <operators/npm_basic/core/npm_task_budget.h>
#include <operators/npm_basic/npm_basic_result_consumer.h>
#include <operators/npm_basic/npm_family_result_contract.h>
#include <services/database/database_plugin.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace flowsql;
using namespace flowsql::npm;
void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::vector<std::shared_ptr<arrow::RecordBatch>> Query(IDatabaseChannel* channel, const std::string& category,
                                                       const std::string& sql) {
    std::vector<std::shared_ptr<arrow::RecordBatch>> result;
    if (category == "clickhouse") {
        Require(channel->ExecuteQueryArrow(sql.c_str(), &result) == 0, channel->GetLastError());
    } else {
        IBatchReader* raw = nullptr;
        Require(channel->CreateReader(sql.c_str(), &raw) == 0 && raw, channel->GetLastError());
        const auto close = [](IBatchReader* reader) {
            reader->Close();
            reader->Release();
        };
        std::unique_ptr<IBatchReader, decltype(close)> reader(raw, close);
        const uint8_t* data = nullptr;
        size_t size = 0;
        for (;;) {
            const int rc = reader->Next(&data, &size);
            if (rc == 1) break;
            Require(rc == 0, "reader Next failed");
            auto buffer = arrow::Buffer::FromString(std::string(reinterpret_cast<const char*>(data), size));
            auto input = std::make_shared<arrow::io::BufferReader>(buffer);
            auto stream = arrow::ipc::RecordBatchStreamReader::Open(input).ValueOrDie();
            for (;;) {
                auto batch = stream->Next().ValueOrDie();
                if (!batch) break;
                result.push_back(batch);
            }
        }
    }
    return result;
}
uint64_t Scalar(IDatabaseChannel* channel, const std::string& category, const std::string& sql) {
    const auto batches = Query(channel, category, sql);
    Require(!batches.empty() && batches[0]->num_rows() == 1, "scalar query missing row");
    return std::stoull(batches[0]->column(0)->GetScalar(0).ValueOrDie()->ToString());
}
std::shared_ptr<arrow::RecordBatch> Rows(const NpmEntityDescriptorV1& entity, int begin, int count,
                                         const std::string& family) {
    std::vector<std::shared_ptr<arrow::Array>> columns;
    for (const auto& field : entity.schema->fields()) {
        auto builder = arrow::MakeBuilder(field->type()).ValueOrDie();
        for (int row = 0; row < count; ++row) {
            arrow::Status status;
            const auto ordinal = static_cast<uint64_t>(begin + row + 1);
            const bool family_entity = entity.entity_id.find("_ipv") != std::string::npos;
            const auto id =
                family == "mixed" && family_entity ? ordinal * 2 - (entity.entity_id.back() == '6' ? 1 : 0) : ordinal;
            if (field->nullable())
                status = builder->AppendNull();
            else
                switch (field->type()->id()) {
                    case arrow::Type::BOOL:
                        status = static_cast<arrow::BooleanBuilder*>(builder.get())->Append(true);
                        break;
                    case arrow::Type::UINT8:
                        status = static_cast<arrow::UInt8Builder*>(builder.get())->Append(17);
                        break;
                    case arrow::Type::UINT16:
                        status = static_cast<arrow::UInt16Builder*>(builder.get())->Append(53);
                        break;
                    case arrow::Type::UINT32:
                        status = static_cast<arrow::UInt32Builder*>(builder.get())->Append(0xc0000200U + id);
                        break;
                    case arrow::Type::UINT64:
                        status = static_cast<arrow::UInt64Builder*>(builder.get())
                                     ->Append(field->name() == entity.identity_column ? id : 1);
                        break;
                    case arrow::Type::INT64:
                        status = static_cast<arrow::Int64Builder*>(builder.get())
                                     ->Append(field->name() == "period_end_ns" ? 30000000000LL : 0);
                        break;
                    case arrow::Type::DOUBLE:
                        status = static_cast<arrow::DoubleBuilder*>(builder.get())->Append(1.25);
                        break;
                    case arrow::Type::STRING: {
                        std::string value = field->name() == "protocol_status" ? "unknown" : "cost";
                        if (field->name() == "a_ip" || field->name() == "b_ip" || field->name() == "src_ip" ||
                            field->name() == "dst_ip") {
                            const bool six = family == "ipv6" || (family == "mixed" && id % 2);
                            uint8_t bytes[16] = {0x20, 1, 0x0d, 0xb8};
                            bytes[12] = id >> 24;
                            bytes[13] = id >> 16;
                            bytes[14] = id >> 8;
                            bytes[15] = id;
                            const uint32_t numeric = 0xc0000200U + id;
                            const uint8_t four[4] = {static_cast<uint8_t>(numeric >> 24),
                                                     static_cast<uint8_t>(numeric >> 16),
                                                     static_cast<uint8_t>(numeric >> 8), static_cast<uint8_t>(numeric)};
                            char text[INET6_ADDRSTRLEN];
                            Require(inet_ntop(six ? AF_INET6 : AF_INET, six ? bytes : four, text, sizeof(text)),
                                    "measurement address formatting");
                            value = text;
                        }
                        status = static_cast<arrow::StringBuilder*>(builder.get())->Append(value);
                        break;
                    }
                    case arrow::Type::FIXED_SIZE_BINARY: {
                        uint8_t bytes[16] = {0x20, 1, 0x0d, 0xb8};
                        bytes[12] = id >> 24;
                        bytes[13] = id >> 16;
                        bytes[14] = id >> 8;
                        bytes[15] = id;
                        status = static_cast<arrow::FixedSizeBinaryBuilder*>(builder.get())->Append(bytes);
                        break;
                    }
                    default:
                        throw std::runtime_error("unsupported measurement field");
                }
            Require(status.ok(), status.ToString());
        }
        columns.push_back(builder->Finish().ValueOrDie());
    }
    auto batch = arrow::RecordBatch::Make(entity.schema, count, std::move(columns));
    Require(batch->ValidateFull().ok(), "invalid measurement rows");
    return batch;
}
uint64_t Size(IDatabaseChannel* channel, const std::string& category, const std::string& table, bool index) {
    // The production SQLite build omits dbstat. The measurement runner inspects pages/indexes
    // with the system SQLite reader after Close; record live whole-file pages here.
    if (category == "sqlite")
        return index ? 0
                     : Scalar(channel, category, "PRAGMA page_count") * Scalar(channel, category, "PRAGMA page_size");
    if (category == "mysql")
        return Scalar(
            channel, category,
            "SELECT " + std::string(index ? "sum_of_other_index_sizes" : "clustered_index_size") +
                "*@@innodb_page_size FROM mysql.innodb_table_stats WHERE database_name=database() AND table_name='" +
                table + "'");
    if (category == "postgres")
        return Scalar(channel, category,
                      "SELECT " + std::string(index ? "pg_indexes_size" : "pg_table_size") + "('" + table + "')");
    return Scalar(channel, category,
                  "SELECT coalesce(sum(" + std::string(index ? "marks_bytes" : "bytes_on_disk") +
                      "),0) FROM system.parts WHERE active AND database=currentDatabase() AND table='" + table + "'");
}
}  // namespace
int main(int argc, char** argv) {
    using namespace flowsql::npm;
    try {
        Require(argc == 4, "usage: benchmark_npm_family_storage options-file backend ipv4|ipv6|mixed");
        const std::string category = argv[2], family = argv[3];
        Require(family == "ipv4" || family == "ipv6" || family == "mixed", "invalid family");
        std::ifstream input(argv[1]);
        std::string options((std::istreambuf_iterator<char>(input)), {});
        flowsql::database::DatabasePlugin plugin;
        const int saved = dup(STDOUT_FILENO);
        FILE* quiet = fopen("/dev/null", "w");
        dup2(fileno(quiet), STDOUT_FILENO);
        const int rc = plugin.Option(options.c_str());
        fflush(stdout);
        dup2(saved, STDOUT_FILENO);
        close(saved);
        fclose(quiet);
        Require(rc == 0 && plugin.Load(nullptr) == 0 && plugin.Start() == 0, "database plugin initialization");
        auto channel = plugin.AcquireChannel(category.c_str(), ("t2" + category).c_str());
        Require(channel && channel->IsConnected(), "measurement backend unavailable");
        std::vector<NpmEntityDescriptorV1> entities;
        for (auto kind : {NpmFamilyEntityKind::kBasic, NpmFamilyEntityKind::kSession, NpmFamilyEntityKind::kDns,
                          NpmFamilyEntityKind::kHttp1, NpmFamilyEntityKind::kTls, NpmFamilyEntityKind::kIcmp})
            for (auto af : {flowsql::packet::AddressFamily::kIPv4, flowsql::packet::AddressFamily::kIPv6})
                entities.push_back(NpmFamilyEntityDescriptor(kind, af));
        auto budget = std::make_shared<NpmTaskBudget>(DefaultNpmAnalysisConfig(NpmRunMode::kOffline));
        auto factory = MakeNpmDatabaseResultConsumerFactory(channel.get(), "cost.packet");
        const NpmResultContextV1 context{
            "cost-" + family,
            "t5-cost-" + family + '-' + std::to_string(std::chrono::system_clock::now().time_since_epoch().count())};
        std::unique_ptr<INpmManagedResultConsumerV1> consumer;
        Require(factory->Create(context, entities, budget, &consumer) == 0, "consumer Create failed");
        constexpr int total = 4096, batch_rows = 512;
        for (const auto& entity : entities) {
            const bool six = entity.entity_id.find("ipv6") != std::string::npos;
            if ((family == "ipv4" && six) || (family == "ipv6" && !six)) continue;
            const int rows = family == "mixed" ? total / 2 : total;
            const std::string table = "npm_" + entity.entity_id + "_v1_data";
            const auto data_before = Size(channel.get(), category, table, false),
                       index_before = Size(channel.get(), category, table, true);
            double elapsed = 0;
            uint64_t address_bytes = 0;
            for (int begin = 0; begin < rows; begin += batch_rows) {
                auto batch = Rows(entity, begin, std::min(batch_rows, rows - begin), family);
                for (int column = 0; column < batch->num_columns(); ++column) {
                    const auto& field = entity.schema->field(column);
                    if (field->metadata() && field->metadata()->Contains("flowsql.address_encoding"))
                        address_bytes += (batch->num_rows() - batch->column(column)->null_count()) * (six ? 16 : 4);
                    else if (field->type()->id() == arrow::Type::STRING &&
                             (field->name() == "a_ip" || field->name() == "b_ip" || field->name() == "src_ip" ||
                              field->name() == "dst_ip")) {
                        const auto& strings = static_cast<const arrow::StringArray&>(*batch->column(column));
                        for (int row = 0; row < batch->num_rows(); ++row) address_bytes += strings.value_length(row);
                    }
                }
                const auto started = std::chrono::steady_clock::now();
                Require(consumer->Consume(context, entity, *batch) == 0,
                        "Consume failed: " + std::string(channel->GetLastError()));
                elapsed +=
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                Require(budget->Usage().pending_output_bytes == 0, "prepared budget leaked");
            }
            if (category == "mysql")
                Require(!Query(channel.get(), category, "ANALYZE TABLE " + table).empty(), "ANALYZE failed");
            const auto count = Scalar(channel.get(), category,
                                      "SELECT COUNT(*) FROM " + table + " WHERE __npm_run_id='" + context.run_id + "'");
            Require(count == static_cast<uint64_t>(rows), "persisted rows mismatch");
            const auto data_after = Size(channel.get(), category, table, false),
                       index_after = Size(channel.get(), category, table, true);
            std::printf(
                "storage,%s,%s,%s,rows,%d,write_ms,%.3f,rows_per_second,%.3f,address_parameter_bytes,%lu,data_before,%"
                "lu,data_after,%lu,index_before,%lu,index_after,%lu,pending_peak,%lu\n",
                category.c_str(), family.c_str(), entity.entity_id.c_str(), rows, elapsed, rows * 1000.0 / elapsed,
                address_bytes, data_before, data_after, index_before, index_after,
                budget->HighWaterMarks().pending_output_bytes);
        }
        Require(consumer->Finish() == 0, "Finish failed");
        consumer.reset();
        factory.reset();
        channel.reset();
        plugin.Stop();
        plugin.Unload();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "benchmark_npm_family_storage: %s\n", error.what());
        return 1;
    }
}
