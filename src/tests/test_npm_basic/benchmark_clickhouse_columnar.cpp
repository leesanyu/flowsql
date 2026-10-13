// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <arrow/api.h>
#include <arrow/util/byte_size.h>
#include <framework/interfaces/idatabase_arrow_write.h>
#include <framework/interfaces/idatabase_factory.h>
#include <services/database/database_plugin.h>
#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
uint64_t Digest(const arrow::RecordBatch& batch) {
    uint64_t digest = 14695981039346656037ULL;
    for (const auto& column : batch.columns()) {
        auto string = column->ToString();
        for (unsigned char byte : string) digest = (digest ^ byte) * 1099511628211ULL;
    }
    return digest;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        Check(argc == 6, "usage: probe options-file channel values|arrow rows iterations");
        std::ifstream file(argv[1]);
        Check(file.good(), "options unavailable");
        const std::string options{std::istreambuf_iterator<char>(file), {}};
        const std::string mode(argv[3]);
        const int rows = std::stoi(argv[4]), iterations = std::stoi(argv[5]);
        Check((mode == "values" || mode == "arrow") && rows > 0 && rows <= 16384 && iterations > 0, "invalid workload");
        flowsql::database::DatabasePlugin plugin;
        Check(plugin.Option(options.c_str()) == 0 && plugin.Load(nullptr) == 0 && plugin.Start() == 0,
              "database load failed");
        auto channel = plugin.AcquireChannel("clickhouse", argv[2]);
        Check(channel && channel->IsOpened(), "channel unavailable");
        const std::string table = "npm_columnar_probe_" + std::to_string(getpid());
        Check(channel->ExecuteSql(("CREATE TABLE " + table +
                                   "(id UInt64,text String,nullable Nullable(Int64),ip FixedString(16)) "
                                   "ENGINE=MergeTree ORDER BY id")
                                      .c_str()) >= 0,
              channel->GetLastError());
        struct TableOwner {
            flowsql::IDatabaseChannel* channel;
            std::string table;
            ~TableOwner() { channel->ExecuteSql(("DROP TABLE IF EXISTS " + table).c_str()); }
        } owner{channel.get(), table};
        arrow::UInt64Builder ids;
        arrow::StringBuilder strings;
        arrow::Int64Builder nulls;
        arrow::FixedSizeBinaryBuilder addresses(arrow::fixed_size_binary(16));
        const std::string special = std::string("quote'&%=\\") + '\0' + "中文";
        for (int row = 0; row < rows; ++row) {
            Check(ids.Append(std::numeric_limits<uint64_t>::max() - row).ok(), "id allocation");
            Check(strings.Append(special).ok(), "text allocation");
            Check((row % 2 ? nulls.Append(-row) : nulls.AppendNull()).ok(), "nullable allocation");
            uint8_t address[16]{};
            address[0] = 0x20;
            address[1] = 0x01;
            address[14] = row >> 8;
            address[15] = row;
            Check(addresses.Append(address).ok(), "address allocation");
        }
        std::vector<std::shared_ptr<arrow::Array>> columns;
        for (arrow::ArrayBuilder* builder : std::vector<arrow::ArrayBuilder*>{&ids, &strings, &nulls, &addresses}) {
            auto array = builder->Finish();
            Check(array.ok(), "array finish");
            columns.push_back(*array);
        }
        auto batch = arrow::RecordBatch::Make(
            arrow::schema({arrow::field("id", arrow::uint64(), false), arrow::field("text", arrow::utf8(), false),
                           arrow::field("nullable", arrow::int64(), true),
                           arrow::field("ip", arrow::fixed_size_binary(16), false)}),
            rows, std::move(columns));
        auto* prepared = dynamic_cast<flowsql::IDatabasePreparedCommandV1*>(channel.get());
        auto* columnar = dynamic_cast<flowsql::IDatabaseArrowWriteV1*>(channel.get());
        Check(prepared && columnar, "write capabilities unavailable");
        arrow::ProxyMemoryPool pool(arrow::default_memory_pool());
        const auto invoke = [&] {
            if (mode == "arrow") return columnar->WriteArrowBatchesWithPool(table.c_str(), {batch}, &pool);
            std::vector<flowsql::DatabaseParameterV1> parameters;
            parameters.reserve(static_cast<size_t>(rows) * 4);
            for (int row = 0; row < rows; ++row) {
                flowsql::DatabaseParameterV1 id, text, nullable, ip;
                id.kind = flowsql::DatabaseParameterKindV1::kUInt64;
                id.uint64_value = std::numeric_limits<uint64_t>::max() - row;
                text.kind = flowsql::DatabaseParameterKindV1::kString;
                text.data = special.data();
                text.size = special.size();
                if (row % 2) {
                    nullable.kind = flowsql::DatabaseParameterKindV1::kInt64;
                    nullable.int64_value = -row;
                }
                ip.kind = flowsql::DatabaseParameterKindV1::kBlob;
                const auto address =
                    std::static_pointer_cast<arrow::FixedSizeBinaryArray>(batch->column(3))->GetView(row);
                ip.data = address.data();
                ip.size = address.size();
                parameters.insert(parameters.end(), {id, text, nullable, ip});
            }
            return prepared->ExecutePreparedBatch(("INSERT INTO " + table + " VALUES(?,?,?,?)").c_str(),
                                                  parameters.data(), 4, rows) < 0
                       ? -1
                       : 0;
        };
        const int warmup_status = invoke();
        Check(warmup_status == 0, channel->GetLastError());
        rusage before{}, after{};
        getrusage(RUSAGE_SELF, &before);
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) {
            const int status = invoke();
            Check(status == 0, channel->GetLastError());
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        getrusage(RUSAGE_SELF, &after);
        Check(pool.bytes_allocated() == 0, "IPC buffer retained");
        std::vector<std::shared_ptr<arrow::RecordBatch>> actual;
        Check(channel->ExecuteQueryArrow(("SELECT count(*) AS n FROM " + table).c_str(), &actual) == 0 &&
                  actual.size() == 1 &&
                  std::static_pointer_cast<arrow::UInt64Array>(actual[0]->column(0))->Value(0) ==
                      static_cast<uint64_t>(rows) * (iterations + 1),
              "stored row count mismatch");
        actual.clear();
        Check(
            channel->ExecuteQueryArrow(("SELECT DISTINCT * FROM " + table + " ORDER BY id DESC").c_str(), &actual) == 0,
            channel->GetLastError());
        int64_t offset = 0;
        for (const auto& read : actual) {
            Check(read->num_columns() == batch->num_columns(), "readback columns");
            for (int col = 0; col < read->num_columns(); ++col) {
                if (col == 3 && read->column(col)->type_id() == arrow::Type::BINARY) {
                    auto address = std::static_pointer_cast<arrow::BinaryArray>(read->column(col));
                    auto expected = std::static_pointer_cast<arrow::FixedSizeBinaryArray>(batch->column(col));
                    for (int64_t row = 0; row < read->num_rows(); ++row) {
                        Check(address->GetView(row) == expected->GetView(offset + row), "address readback value");
                    }
                } else {
                    Check(read->column(col)->Equals(batch->column(col)->Slice(offset, read->num_rows())),
                          "readback values");
                }
            }
            offset += read->num_rows();
        }
        Check(offset == rows, "readback count");
        auto bytes = arrow::util::ReferencedBufferSize(*batch);
        Check(bytes.ok(), "logical byte count");
        const auto cpu = [](const rusage& usage) {
            return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0 +
                   (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
        };
        std::cout << std::fixed << std::setprecision(6) << "columnar_probe,{\"mode\":\"" << mode
                  << "\",\"batch_rows\":" << rows << ",\"iterations\":" << iterations
                  << ",\"rows\":" << static_cast<uint64_t>(rows) * iterations << ",\"write_ms\":" << ms
                  << ",\"rows_per_second\":" << rows * iterations * 1000.0 / ms
                  << ",\"logical_bytes\":" << *bytes * iterations
                  << ",\"logical_bytes_per_second\":" << *bytes * iterations * 1000.0 / ms
                  << ",\"cpu_ms\":" << cpu(after) - cpu(before) << ",\"ipc_pool_peak\":" << pool.max_memory()
                  << ",\"rss_bytes\":" << after.ru_maxrss * 1024 << ",\"requests\":" << iterations
                  << ",\"readback_equal\":true,\"digest\":" << Digest(*batch) << ",\"table\":\"" << table << "\"}\n";
    } catch (const std::exception& error) {
        std::cerr << "columnar probe failed: " << error.what() << '\n';
        return 1;
    }
}
