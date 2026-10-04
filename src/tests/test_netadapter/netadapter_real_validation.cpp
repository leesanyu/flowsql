// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arpa/inet.h>
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <framework/core/capture_progress_tracker.h>
#include <framework/interfaces/ibinaddon_host.h>
#include <framework/interfaces/iblock_stream_manager.h>
#include <framework/interfaces/iblock_stream_reader.h>
#include <framework/interfaces/icapture_block_stream_reader.h>
#include <framework/interfaces/idatabase_factory.h>
#include <framework/interfaces/irouter_handle.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <common/loader.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <thread>
using namespace flowsql;
namespace {
using Clock = std::chrono::steady_clock;
struct PluginOwner {
    PluginLoader* loader;
    ~PluginOwner() {
        loader->StopAll();
        loader->Unload();
    }
};
int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
fnRouterHandler Route(PluginLoader* loader, const char* uri) {
    fnRouterHandler result;
    loader->Traverse(IID_ROUTER_HANDLE, [&](void* value) {
        static_cast<IRouterHandle*>(value)->EnumRoutes([&](const RouteItem& item) {
            if (item.method == "POST" && item.uri == uri) result = item.handler;
        });
        return result ? -1 : 0;
    });
    return result;
}
std::string Json(const rapidjson::Document& doc) {
    rapidjson::StringBuffer out;
    rapidjson::Writer<rapidjson::StringBuffer> writer(out);
    doc.Accept(writer);
    return out.GetString();
}
std::vector<std::shared_ptr<arrow::RecordBatch>> Query(IDatabaseChannel* db, const std::string& sql) {
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    IBatchReader* raw = nullptr;
    const int rc = db->CreateReader(sql.c_str(), &raw);
    if (rc) std::cerr << "SQLite query failed: " << db->GetLastError() << "\nSQL: " << sql << "\n";
    assert(rc == 0 && raw);
    const auto release = [](IBatchReader* reader) {
        reader->Close();
        reader->Release();
    };
    std::unique_ptr<IBatchReader, decltype(release)> reader(raw, release);
    for (;;) {
        const uint8_t* bytes = nullptr;
        size_t size = 0;
        const int next = reader->Next(&bytes, &size);
        if (next == 1) break;
        assert(next == 0 && bytes && size);
        auto input = std::make_shared<arrow::io::BufferReader>(
            arrow::Buffer::FromString(std::string(reinterpret_cast<const char*>(bytes), size)));
        auto stream = arrow::ipc::RecordBatchStreamReader::Open(input).ValueOrDie();
        for (;;) {
            std::shared_ptr<arrow::RecordBatch> batch;
            assert(stream->ReadNext(&batch).ok());
            if (!batch) break;
            batches.push_back(std::move(batch));
        }
    }
    return batches;
}
uint64_t Count(IDatabaseChannel* db, const std::string& sql) {
    auto batches = Query(db, sql);
    assert(!batches.empty());
    return std::stoull(batches[0]->column(0)->GetScalar(0).ValueOrDie()->ToString());
}
bool CheckPeriodicResults(IDatabaseChannel* db, const std::string& relation, unsigned interval, bool single,
                          bool require_three_periods, rapidjson::Value* evidence,
                          rapidjson::Document::AllocatorType& allocator) {
    struct Prefix {
        uint64_t revision = 0, complete = 0, packets_ab = 0, packets_ba = 0, bytes_ab = 0, bytes_ba = 0;
        int64_t last_end = 0;
    };
    std::map<std::string, Prefix> sessions;
    uint64_t failures = 0, rows = 0, partial_final = 0;
    auto batches = Query(db,
                         "SELECT session_id,CAST(revision AS INTEGER),period_start_ns,period_end_ns,"
                         "period_complete,is_final,CAST(interval_packets_ab AS INTEGER),"
                         "CAST(interval_packets_ba AS INTEGER),CAST(interval_wire_bytes_ab AS INTEGER),"
                         "CAST(interval_wire_bytes_ba AS INTEGER),CAST(interval_wire_bytes_total AS INTEGER),"
                         "CAST(packets_ab AS INTEGER),CAST(packets_ba AS INTEGER),CAST(wire_bytes_ab AS INTEGER),"
                         "CAST(wire_bytes_ba AS INTEGER),CAST(wire_bytes_total AS INTEGER) FROM " +
                             relation + " ORDER BY session_id,CAST(revision AS INTEGER)");
    const int64_t period_ns = int64_t(interval) * 1'000'000'000;
    for (const auto& batch : batches) {
        for (int64_t row = 0; row < batch->num_rows(); ++row) {
            auto value = [&](int column) { return batch->column(column)->GetScalar(row).ValueOrDie()->ToString(); };
            auto number = [&](int column) { return std::stoull(value(column)); };
            auto& prefix = sessions[value(0)];
            const auto revision = number(1);
            const auto start = std::stoll(value(2)), end = std::stoll(value(3));
            const bool complete = value(4) == "1" || value(4) == "true";
            const bool final = value(5) == "1" || value(5) == "true";
            if (revision != prefix.revision + 1 || start % period_ns || end - start != period_ns ||
                (prefix.revision && start != prefix.last_end && !(final && end == prefix.last_end)))
                ++failures;
            prefix.revision = revision;
            prefix.last_end = end;
            prefix.complete += complete && !final;
            prefix.packets_ab += number(6);
            prefix.packets_ba += number(7);
            prefix.bytes_ab += number(8);
            prefix.bytes_ba += number(9);
            if (number(10) != number(8) + number(9) || prefix.packets_ab != number(11) ||
                prefix.packets_ba != number(12) || prefix.bytes_ab != number(13) || prefix.bytes_ba != number(14) ||
                number(15) != number(13) + number(14))
                ++failures;
            partial_final += final && !complete;
            ++rows;
        }
    }
    evidence->SetObject();
    rapidjson::Value prefixes(rapidjson::kArrayType);
    for (const auto& entry : sessions) {
        const auto& prefix = entry.second;
        if (require_three_periods &&
            (prefix.complete < 3 || prefix.packets_ab == 0 || (!single && prefix.packets_ba == 0)))
            ++failures;
        rapidjson::Value item(rapidjson::kObjectType);
        item.AddMember("session_id", rapidjson::Value(entry.first.c_str(), allocator), allocator);
        item.AddMember("last_revision", prefix.revision, allocator);
        item.AddMember("complete_periods", prefix.complete, allocator);
        item.AddMember("packets_ab", prefix.packets_ab, allocator);
        item.AddMember("packets_ba", prefix.packets_ba, allocator);
        item.AddMember("wire_bytes_ab", prefix.bytes_ab, allocator);
        item.AddMember("wire_bytes_ba", prefix.bytes_ba, allocator);
        prefixes.PushBack(item, allocator);
    }
    evidence->AddMember("checked_rows", rows, allocator);
    evidence->AddMember("failures", failures, allocator);
    evidence->AddMember("partial_final_rows", partial_final, allocator);
    evidence->AddMember("sessions", prefixes, allocator);
    return failures == 0;
}
struct Sender {
    std::atomic<bool> stopped{false};
    std::atomic<uint64_t> sent{0}, errors{0}, selected_sent{0}, rejected_sent{0};
    std::thread worker;
    Sender(const char* first, const char* second, unsigned pps, unsigned size, const std::string& profile = "dual") {
        std::string names[2] = {first, second};
        worker = std::thread([this, names, pps, size, profile] {
            int sockets[2];
            sockaddr_ll destinations[2]{};
            for (int i = 0; i < 2; ++i) {
                sockets[i] = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_IP));
                assert(sockets[i] >= 0);
                destinations[i].sll_family = AF_PACKET;
                destinations[i].sll_ifindex = if_nametoindex(names[i].c_str());
                destinations[i].sll_protocol = htons(ETH_P_IP);
                destinations[i].sll_halen = 6;
                std::memset(destinations[i].sll_addr, 0xff, 6);
            }
            auto next = Clock::now();
            const auto started = next;
            uint32_t sequence = 0;
            uint64_t tick = 0;
            while (!stopped) {
                for (unsigned direction = 0; direction < 2; ++direction) {
                    if (direction && (profile == "single" || (profile == "busy_idle" && tick % 100 != 0))) continue;
                    for (bool tcp : {false, true}) {
                        std::vector<uint8_t> frame(size);
                        std::memset(frame.data(), 0xff, 6);
                        frame[6] = 2;
                        frame[12] = 8;
                        frame[13] = 0;
                        frame[14] = 0x45;
                        const uint16_t ip_size = size - 14;
                        frame[16] = ip_size >> 8;
                        frame[17] = ip_size;
                        frame[22] = 64;
                        frame[23] = tcp ? 6 : 17;
                        frame[26] = 10;
                        frame[29] = direction ? 2 : 1;
                        frame[30] = 10;
                        frame[33] = direction ? 1 : 2;
                        const bool selected = profile != "high_reject" || tick % 100 == 0;
                        const uint16_t source = selected ? (direction ? 41001 : 41000) : 41002;
                        const uint16_t destination = direction ? 41000 : 41001;
                        frame[34] = source >> 8;
                        frame[35] = source;
                        frame[36] = destination >> 8;
                        frame[37] = destination;
                        std::memcpy(frame.data() + 54, "flowsql", 7);
                        if (tcp) {
                            frame[46] = 0x50;
                            frame[47] = 0x10;
                            frame[41] = sequence++;
                        } else {
                            const uint16_t udp_size = size - 34;
                            frame[38] = udp_size >> 8;
                            frame[39] = udp_size;
                        }
                        uint32_t checksum = 0;
                        for (int n = 14; n < 34; n += 2) checksum += (uint16_t(frame[n]) << 8) | frame[n + 1];
                        while (checksum >> 16) checksum = (checksum & 65535) + (checksum >> 16);
                        checksum = ~checksum;
                        frame[24] = checksum >> 8;
                        frame[25] = checksum;
                        if (sendto(sockets[direction], frame.data(), frame.size(), 0,
                                   reinterpret_cast<sockaddr*>(&destinations[direction]),
                                   sizeof(sockaddr_ll)) == int(frame.size())) {
                            ++sent;
                            if (selected)
                                ++selected_sent;
                            else
                                ++rejected_sent;
                        } else
                            ++errors;
                    }
                }
                ++tick;
                const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
                const auto rate = profile == "overload_recovery" && elapsed >= 5 && elapsed < 10 ? 5000 : pps;
                next += std::chrono::nanoseconds(1000000000ULL / rate);
                std::this_thread::sleep_until(next);
            }
            for (int fd : sockets) close(fd);
        });
    }
    void Stop() {
        stopped = true;
        if (worker.joinable()) worker.join();
    }
    ~Sender() { Stop(); }
};
}  // namespace
int main(int argc, char** argv) {
    if (argc != 13 && argc != 14) {
        std::cerr << "usage: netadapter_real_validation capture|sql|sql_all backend interfaceA interfaceB peerA peerB "
                     "interval_s duration_s ticks_per_s frame_bytes stop|cancel artifact.json "
                     "[dual|single|busy_idle|reject]\n";
        return 2;
    }
    assert(std::string(argv[1]) == "capture" || std::string(argv[1]) == "sql" || std::string(argv[1]) == "sql_all" ||
           std::string(argv[1]) == "sql_probe");
    assert(std::string(argv[11]) == "stop" || std::string(argv[11]) == "cancel");
    const bool capture = std::string(argv[1]) == "capture", cancel = std::string(argv[11]) == "cancel";
    const std::string backend = argv[2];
    const std::string profile = argc == 14 ? argv[13] : "dual";
    assert(profile == "dual" || profile == "single" || profile == "busy_idle" || profile == "reject" ||
           profile == "slow_analysis" || profile == "slow_output" || profile == "overload_recovery" ||
           profile == "domain_isolation" || profile == "input_failure" || profile == "high_reject");
    const bool probe = std::string(argv[1]) == "sql_probe";
    assert(capture || profile == "dual" || probe);
    assert(setenv("FLOWSQL_NETADAPTER_TEST_PROFILE", profile.c_str(), 1) == 0);
    assert(setenv("FLOWSQL_NETADAPTER_TEST_BACKEND", backend.c_str(), 1) == 0);
    const unsigned interval = std::stoul(argv[7]), duration = std::stoul(argv[8]);
    const unsigned pps = std::stoul(argv[9]), frame_bytes = std::stoul(argv[10]);
    assert(pps && interval && duration >= interval * 3 + 2 && (frame_bytes == 64 || frame_bytes == 1500));
    const auto root = std::filesystem::path("/tmp") / ("flowsql-netadapter-validation-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    auto* loader = PluginLoader::Single();
    PluginOwner libraries{loader};
    const char* libs[] = {"libflowsql_database.so",       "libflowsql_builtin.so",
                          "libflowsql_catalog.so",        "libflowsql_npi.so",
                          "libflowsql_netadapter.so",     "libflowsql_capture_af_packet.so",
                          "libflowsql_capture_pfring.so", "libflowsql_capture_af_xdp.so",
                          FLOWSQL_VALIDATION_PROBE_PATH,  "libflowsql_scheduler.so",
                          "libflowsql_binaddon.so"};
    std::string db_option = "type=sqlite;name=results;path=" + (root / "results.db").string();
    std::string catalog_option =
        "data_dir=" + (root / "data").string() + ";operator_db_path=" + (root / "operators.db").string();
    std::string addon_option =
        "operator_db_path=" + (root / "operators.db").string() + ";upload_dir=" + (root / "uploads").string();
    const unsigned npi_concurrency = 1;
    std::string npi_option = std::string("{\"ldfile\":\"") + FLOWSQL_NPI_PROTOCOLS_PATH +
                             "\",\"concurrency\":" + std::to_string(npi_concurrency) + "}";
    std::string channel_option = "{\"db_path\":\"" + (root / "channels.db").string() + "\"}";
    const char* options[] = {db_option.c_str(),
                             nullptr,
                             catalog_option.c_str(),
                             npi_option.c_str(),
                             channel_option.c_str(),
                             nullptr,
                             nullptr,
                             nullptr,
                             nullptr,
                             nullptr,
                             addon_option.c_str()};
    assert(loader->Load(get_absolute_process_path(), libs, options, 11) == 0);
    assert(loader->StartAll() == 0);
    IBlockStreamManager* manager = nullptr;
    IBlockStreamReaderFactoryV1* reader_factory = nullptr;
    loader->Traverse(IID_BLOCK_STREAM_MANAGER, [&](void* value) {
        manager = static_cast<IBlockStreamManager*>(value);
        return -1;
    });
    loader->Traverse(IID_BLOCK_STREAM_READER_FACTORY_V1, [&](void* value) {
        reader_factory = static_cast<IBlockStreamReaderFactoryV1*>(value);
        return -1;
    });
    assert(manager && reader_factory);
    const std::string selected_interfaces =
        profile == "single" ? std::string(argv[3]) : std::string(argv[3]) + "\",\"" + argv[4];
    const std::string config = "{\"backend\":\"" + backend + "\",\"interfaces\":[\"" + selected_interfaces +
                               "\"],\"snaplen\":1500,\"buffer_mib\":16}";
    assert(manager->AddChannel("netadapter", "live", config) == 0);
    if (profile == "domain_isolation") assert(manager->AddChannel("netadapter", "other", config) == 0);
    rapidjson::Document artifact;
    artifact.SetObject();
    auto& allocator = artifact.GetAllocator();
    artifact.AddMember("mode", rapidjson::Value(argv[1], allocator), allocator);
    artifact.AddMember("backend", rapidjson::Value(backend.c_str(), allocator), allocator);
    artifact.AddMember("profile", rapidjson::Value(profile.c_str(), allocator), allocator);
    artifact.AddMember("npi_concurrency", npi_concurrency, allocator);
    artifact.AddMember("period_s", interval, allocator);
    artifact.AddMember("duration_s", duration, allocator);
    artifact.AddMember("selected_ticks_per_s", pps, allocator);
    if (profile == "busy_idle")
        artifact.AddMember("packets_per_tick", rapidjson::Value(rapidjson::kNullType), allocator);
    else
        artifact.AddMember("packets_per_tick", profile == "single" ? 2 : 4, allocator);
    artifact.AddMember("ethernet_frame_bytes", frame_bytes, allocator);
    artifact.AddMember("snaplen", 1500, allocator);
    artifact.AddMember("channel_name", "netadapter.live", allocator);
    artifact.AddMember("criterion_window_close_ms", 2000, allocator);
    artifact.AddMember("criterion_cold_service_ms", 100, allocator);
    artifact.AddMember("criterion_capture_watermark_lag_ms", 2000, allocator);
    artifact.AddMember("criterion_reader_dropped_packets", 0, allocator);
    artifact.AddMember("criterion_source_dropped_packets_when_available", 0, allocator);
    artifact.AddMember("channel_buffer_bytes", 16 * 1024 * 1024, allocator);
    artifact.AddMember("criterion_maintenance_lateness_ms", 100, allocator);
    artifact.AddMember("criterion_recovery_s", 5, allocator);
    artifact.AddMember("pressure_start_s", 5, allocator);
    artifact.AddMember("pressure_end_s", 10, allocator);
    artifact.AddMember("pressure_ticks_per_s", 5000, allocator);
    artifact.AddMember("analysis_delay_ms", profile == "slow_analysis" ? 20 : 50, allocator);
    artifact.AddMember("output_delay_ms", 10, allocator);
    utsname environment{};
    assert(uname(&environment) == 0);
    artifact.AddMember("kernel", rapidjson::Value(environment.release, allocator), allocator);
    artifact.AddMember("machine", rapidjson::Value(environment.machine, allocator), allocator);
    std::string cpu_line;
    std::ifstream cpu_info("/proc/cpuinfo");
    while (std::getline(cpu_info, cpu_line))
        if (cpu_line.rfind("model name", 0) == 0) break;
    artifact.AddMember("cpu", rapidjson::Value(cpu_line.c_str(), allocator), allocator);
    rapidjson::Value command(rapidjson::kArrayType);
    for (int i = 0; i < argc; ++i) command.PushBack(rapidjson::Value(argv[i], allocator), allocator);
    artifact.AddMember("command", command, allocator);
    std::vector<double> poll_ms, watermark_lag_ms, window_close_ms;
    std::vector<double> batch_packets, batch_bytes;
    rapidjson::Value samples(rapidjson::kArrayType);
    uint64_t received = 0, complete = 0;
    bool passed = true;
    std::string diagnostics;
    uint64_t peak_capture_bytes = 0;
    const auto observation_origin = Clock::now();
    std::map<std::string, uint64_t> recovered_drop_baseline;
    auto next_sample = observation_origin;
    auto sample_channel = [&] {
        if (Clock::now() < next_sample) return;
        next_sample = Clock::now() + std::chrono::milliseconds(200);
        manager->QueryChannels([&](const auto&, const auto&, const auto& json, const auto&) { diagnostics = json; });
        rapidjson::Document channel;
        channel.Parse(diagnostics.c_str());
        assert(channel.IsObject());
        for (const auto& reader : channel["readers"].GetArray()) {
            const uint64_t bytes = reader["backend_bytes"].GetUint64() + reader["arrow_peak_bytes"].GetUint64();
            peak_capture_bytes = std::max(peak_capture_bytes, bytes);
            if (bytes > 16 * 1024 * 1024) passed = false;
            for (const char* field : {"reader_dropped_packets", "source_dropped_packets"}) {
                if (reader["totals"][field].IsNull()) continue;
                const auto value = reader["totals"][field].GetUint64();
                if (profile != "overload_recovery" && value != 0) passed = false;
                if (profile == "overload_recovery" && Clock::now() - observation_origin >= std::chrono::seconds(15)) {
                    const auto baseline = recovered_drop_baseline.emplace(field, value);
                    if (!baseline.second && baseline.first->second != value) passed = false;
                }
            }
        }
        rapidjson::Value sample(rapidjson::kObjectType);
        sample.AddMember("elapsed_s", std::chrono::duration<double>(Clock::now() - observation_origin).count(),
                         allocator);
        sample.AddMember("channel", rapidjson::Value(channel, allocator), allocator);
        samples.PushBack(sample, allocator);
    };
    rusage usage_start{};
    assert(getrusage(RUSAGE_SELF, &usage_start) == 0);
    const auto measurement_start = Clock::now();
    if (capture) {
        BlockStreamReaderConfigV1 request;
        request.task_id = "capture-validation";
        request.source_category = "netadapter";
        request.source_name = "live";
        request.pushed_filter_plan_json =
            profile == "reject"
                ? R"({"version":1,"root":{"node_id":1,"kind":"literal","type":"bool","nullable":false,"value":"false"}})"
                : R"({"version":1,"root":null})";
        artifact.AddMember("pushed_filter_plan_json", rapidjson::Value(request.pushed_filter_plan_json, allocator),
                           allocator);
        IBlockStreamChannel* base = nullptr;
        const int rc = reader_factory->CreateReader(request, &base);
        if (rc) {
            std::cerr << "real capture Open failed: " << rc << "\n";
            return 1;
        }
        auto* reader = dynamic_cast<ICaptureBlockStreamReaderV2*>(base);
        assert(reader);
        CaptureSourceSetV2 sources;
        assert(reader->DescribeSources(&sources) == 0);
        CaptureProgressTrackerV2 progress(sources);
        Sender sender(argv[5], argv[6], pps, frame_bytes, profile);
        const auto until = Clock::now() + std::chrono::seconds(duration);
        uint64_t sequence = 0;
        std::vector<uint64_t> per_input_packets(sources.inputs.size());
        std::vector<uint64_t> per_input_known_packets(sources.inputs.size());
        std::vector<Clock::time_point> last_service(sources.inputs.size(), Clock::now());
        std::vector<double> service_max(sources.inputs.size());
        uint64_t common_advances = 0, common_unavailable_polls = 0;
        std::optional<int64_t> last_common;
        while (Clock::now() < until) {
            const auto start = Clock::now();
            auto event = reader->PollCapture(10);
            poll_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
            if (event.block.kind == BlockPollEvent::kError) {
                passed = false;
                break;
            }
            if (event.block.batch) {
                assert(profile != "reject");
                received += event.block.batch->num_rows();
                batch_packets.push_back(event.block.batch->num_rows());
                auto lengths = std::static_pointer_cast<arrow::UInt32Array>(event.block.batch->column(1));
                uint64_t captured = 0;
                for (int64_t row = 0; row < lengths->length(); ++row) captured += lengths->Value(row);
                batch_bytes.push_back(captured);
                auto values = std::static_pointer_cast<arrow::UInt64Array>(event.block.batch->column(5));
                auto ids = std::static_pointer_cast<arrow::UInt32Array>(event.block.batch->column(4));
                auto raw = std::static_pointer_cast<arrow::BinaryArray>(event.block.batch->column(6));
                for (int64_t row = 0; row < values->length(); ++row) {
                    assert(values->Value(row) > sequence && (sequence = values->Value(row)));
                    ++per_input_packets.at(ids->Value(row));
                    const auto bytes = raw->GetView(row);
                    if (bytes.size() == frame_bytes && std::memcmp(bytes.data() + 54, "flowsql", 7) == 0)
                        ++per_input_known_packets.at(ids->Value(row));
                }
                assert(reader->ReleaseBlock(event.block.batch) == 0);
                event.block.batch.reset();
            }
            for (const auto& fact : event.progress) {
                service_max[fact.source_id] = std::max(
                    service_max[fact.source_id],
                    std::chrono::duration<double, std::milli>(Clock::now() - last_service[fact.source_id]).count());
                last_service[fact.source_id] = Clock::now();
            }
            assert(progress.Observe(event.progress) == CaptureProgressErrorV1::kNone);
            if (auto watermark = progress.CommonCandidateNs()) {
                if (!last_common || *watermark > *last_common) ++common_advances;
                last_common = watermark;
                watermark_lag_ms.push_back(double(NowNs() - *watermark) / 1000000);
                if (watermark_lag_ms.back() > 2000) passed = false;
            } else
                ++common_unavailable_polls;
            sample_channel();
        }
        sender.Stop();
        artifact.AddMember("sent_packets", sender.sent.load(), allocator);
        artifact.AddMember("send_errors", sender.errors.load(), allocator);
        if (sender.errors || (profile != "reject" && !received) || watermark_lag_ms.empty() || common_advances < 3)
            passed = false;
        artifact.AddMember("common_candidate_advances", common_advances, allocator);
        artifact.AddMember("common_candidate_unavailable_polls", common_unavailable_polls, allocator);
        artifact.AddMember("all_filtered_no_data", profile == "reject" && received == 0, allocator);
        artifact.AddMember("delivered_packets", received, allocator);
        rapidjson::Value services(rapidjson::kArrayType);
        for (double value : service_max) {
            services.PushBack(value, allocator);
            if (value > 100) passed = false;
        }
        artifact.AddMember("per_input_service_max_ms", services, allocator);
        rapidjson::Value input_packets(rapidjson::kArrayType);
        for (auto count : per_input_packets) input_packets.PushBack(count, allocator);
        artifact.AddMember("per_input_delivered_packets", input_packets, allocator);
        rapidjson::Value known_packets(rapidjson::kArrayType);
        uint64_t per_interface_known[2]{};
        for (size_t i = 0; i < per_input_known_packets.size(); ++i) {
            known_packets.PushBack(per_input_known_packets[i], allocator);
            const auto name = std::string(sources.inputs[i].source_name);
            if (name.rfind(std::string(argv[3]) + ":", 0) == 0) per_interface_known[0] += per_input_known_packets[i];
            if (name.rfind(std::string(argv[4]) + ":", 0) == 0) per_interface_known[1] += per_input_known_packets[i];
        }
        if (profile != "reject" && (!per_interface_known[0] || (profile != "single" && !per_interface_known[1])))
            passed = false;
        artifact.AddMember("per_input_known_packets", known_packets, allocator);
        artifact.AddMember("all_inputs_received_known_packets",
                           std::all_of(per_input_known_packets.begin(), per_input_known_packets.end(),
                                       [](uint64_t count) { return count > 0; }),
                           allocator);
        next_sample = Clock::now();
        sample_channel();
        if (cancel)
            reader->Cancel();
        else
            assert(reader->Flush() == 0);
        assert(reader->PollCapture(0).block.kind == (cancel ? BlockPollEvent::kCancelled : BlockPollEvent::kEof));
        reader_factory->ReleaseReader(base);
    } else {
        auto* addon = static_cast<IBinAddonHost*>(loader->First(IID_BINADDON_HOST));
        assert(addon);
        std::string response;
        const auto upload = root / "npm_basic.so";
        std::filesystem::copy_file(probe ? FLOWSQL_VALIDATION_PROBE_PATH : FLOWSQL_NPM_BASIC_PLUGIN_PATH, upload);
        assert(addon->UploadCppPlugin("npm_basic.so", upload.string(), response) == 0);
        rapidjson::Document uploaded;
        uploaded.Parse(response.c_str());
        assert(uploaded.HasMember("plugin_id"));
        const std::string plugin_id = uploaded["plugin_id"].GetString();
        if (addon->ActivateCppPlugin(plugin_id, response) != 0) {
            std::cerr << "validation operator activation failed: " << response << "\n";
            return 1;
        }
        auto submit = Route(loader, "/scheduler/batch/submit"), status = Route(loader, "/scheduler/batch/status"),
             stop = Route(loader, "/scheduler/batch/stop");
        assert(submit && status && stop);
        const std::string sql =
            "SELECT * FROM netadapter.live " +
            (std::string(argv[1]) != "sql"
                 ? (profile == "reject"        ? std::string("WHERE wire_len < 0 ")
                    : profile == "high_reject" ? std::string("WHERE src_port < 41002 ")
                                               : std::string())
                 : std::string("WHERE wire_len >= 64 AND (transport_protocol = 6 OR transport_protocol = 17) ")) +
            "USING npm.basic WITH output_interval_ns=" + std::to_string(uint64_t(interval) * 1000000000) +
            ",out_of_order_tolerance_ns=1000000000,tcp_idle_timeout_ns=600000000000,udp_idle_timeout_ns=600000000000" +
            (std::string(argv[1]) != "sql" ? std::string(",features='basic,icmp' ") : std::string(" ")) +
            (probe ? std::string(",max_tracked_bytes=16777216,max_pending_output_bytes=4194304 ") : std::string()) +
            "INTO "
            "sqlite.results";
        rapidjson::Document request;
        request.SetObject();
        request.AddMember("runtime_task_id", "netadapter-real", request.GetAllocator());
        request.AddMember("sql_text", rapidjson::Value(sql.c_str(), request.GetAllocator()), request.GetAllocator());
        const auto submitted = submit("/scheduler/batch/submit", Json(request), response);
        if (submitted) {
            std::cerr << response << "\n";
            return 1;
        }
        const std::string identity = R"({"runtime_task_id":"netadapter-real"})";
        std::string run_id;
        for (int attempt = 0; attempt < 500; ++attempt) {
            assert(status("/scheduler/batch/status", identity, response) == 0);
            rapidjson::Document state;
            state.Parse(response.c_str());
            if (state.HasMember("managed_result") && state["managed_result"].HasMember("run_id")) {
                run_id = state["managed_result"]["run_id"].GetString();
                break;
            }
            if (state.HasMember("status") && std::string(state["status"].GetString()) == "failed") {
                std::cerr << response << "\n";
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(!run_id.empty());
        std::string other_run_id;
        auto* db = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY))->Get("sqlite", "results");
        assert(db);
        Sender sender(argv[5], argv[6], pps, frame_bytes, profile);
        const auto sql_started = Clock::now();
        const auto sql_started_ns = NowNs();
        artifact.AddMember("sql_start_wall_ns", sql_started_ns, allocator);
        const auto until = Clock::now() + std::chrono::seconds(duration);
        const std::string relation = "npm_basic_history_v1 WHERE __npm_run_id='" + run_id + "'";
        std::set<std::pair<std::string, std::string>> observed_rows;
        std::optional<int64_t> last_common;
        uint64_t common_advances = 0, recovery_advances = 0;
        bool pressure_observed = false;
        bool expected_input_failure = false;
        double recovery_s = -1;
        rapidjson::Value sql_samples(rapidjson::kArrayType);
        rapidjson::Value window_samples(rapidjson::kArrayType);
        while (Clock::now() < until) {
            const auto start = Clock::now();
            assert(status("/scheduler/batch/status", identity, response) == 0);
            poll_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
            rapidjson::Document state;
            state.Parse(response.c_str());
            if (std::string(state["status"].GetString()) == "failed") {
                std::cerr << response << "\n";
                expected_input_failure =
                    profile == "input_failure" && Clock::now() - sql_started >= std::chrono::seconds(1);
                if (!expected_input_failure) passed = false;
                break;
            }
            const auto elapsed_s = std::chrono::duration<double>(Clock::now() - sql_started).count();
            if (probe && state.HasMember("managed_result") && state["managed_result"].HasMember("diagnostics")) {
                const auto& measured = state["managed_result"]["diagnostics"];
                if (measured["tracked_peak_bytes"].GetUint64() > 16777216 ||
                    measured["pending_output_peak_bytes"].GetUint64() > 4194304)
                    passed = false;
                const auto& common = measured["common_candidate_ns"];
                if (common.IsInt64()) {
                    if (!last_common || common.GetInt64() > *last_common) {
                        ++common_advances;
                        if (elapsed_s >= 10) {
                            ++recovery_advances;
                            if (recovery_s < 0) recovery_s = elapsed_s - 10;
                        }
                    }
                    last_common = common.GetInt64();
                    const double lag = double(NowNs() - *last_common) / 1e6;
                    watermark_lag_ms.push_back(lag);
                    if (elapsed_s >= 5 && elapsed_s < 10 && lag > 1000) pressure_observed = true;
                    if (elapsed_s >= (profile == "overload_recovery" ? 15 : 2) && lag > 2000) passed = false;
                } else {
                    if (elapsed_s >= 5 && elapsed_s < 10) pressure_observed = true;
                    if (profile != "overload_recovery" && elapsed_s >= 2) {
                        // A blocked fact pauses progress. Measure the last confirmed candidate's age without
                        // treating this sample as a new fact or advancing the production watermark.
                        if (!last_common)
                            passed = false;
                        else {
                            const double lag = double(NowNs() - *last_common) / 1e6;
                            watermark_lag_ms.push_back(lag);
                            if (lag > 2000) passed = false;
                        }
                    }
                }
                rapidjson::Value sample(rapidjson::kObjectType);
                sample.AddMember("elapsed_s", elapsed_s, allocator);
                sample.AddMember("sent_packets", sender.sent.load(), allocator);
                sample.AddMember("managed_result", rapidjson::Value(state["managed_result"], allocator), allocator);
                sql_samples.PushBack(sample, allocator);
            }
            complete = Count(db, "SELECT count(*) FROM " + relation + " AND period_complete=1");
            auto rows =
                Query(db, "SELECT session_id,revision,period_end_ns FROM " + relation + " AND period_complete=1");
            const auto observed_ns = NowNs();
            for (const auto& batch : rows)
                for (int64_t row = 0; row < batch->num_rows(); ++row) {
                    const auto id = batch->column(0)->GetScalar(row).ValueOrDie()->ToString();
                    const auto revision = batch->column(1)->GetScalar(row).ValueOrDie()->ToString();
                    if (!observed_rows.emplace(id, revision).second) continue;
                    const int64_t end_ns = std::stoll(batch->column(2)->GetScalar(row).ValueOrDie()->ToString());
                    const double delay = double(observed_ns - end_ns) / 1000000;
                    window_close_ms.push_back(delay);
                    const bool pressure_window = profile == "overload_recovery" &&
                                                 end_ns + 2'000'000'000LL >= sql_started_ns + 5'000'000'000LL &&
                                                 end_ns < sql_started_ns + 15'000'000'000LL;
                    rapidjson::Value sample(rapidjson::kObjectType);
                    sample.AddMember("period_end_ns", end_ns, allocator);
                    sample.AddMember("first_observed_ns", observed_ns, allocator);
                    sample.AddMember("delay_ms", delay, allocator);
                    sample.AddMember("pressure_affected", pressure_window, allocator);
                    window_samples.PushBack(sample, allocator);
                    if (delay < 0 || (!pressure_window && delay > 2000)) passed = false;
                }
            sample_channel();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        sender.Stop();
        artifact.AddMember("sent_packets", sender.sent.load(), allocator);
        artifact.AddMember("send_errors", sender.errors.load(), allocator);
        artifact.AddMember("selected_sent_packets", sender.selected_sent.load(), allocator);
        artifact.AddMember("rejected_sent_packets", sender.rejected_sent.load(), allocator);
        if (profile == "high_reject" && (sender.selected_sent == 0 || sender.rejected_sent == 0)) passed = false;
        if (sender.errors) passed = false;
        artifact.AddMember("complete_rows_before_stop", complete, allocator);
        if (profile != "reject" && profile != "input_failure" && complete < 6) passed = false;
        const std::string stop_body = "{\"runtime_task_id\":\"netadapter-real\",\"mode\":\"" +
                                      (cancel ? std::string("cancel") : std::string("stop")) + "\"}";
        if (!expected_input_failure) assert(stop("/scheduler/batch/stop", stop_body, response) == 0);
        rapidjson::Document final;
        for (int attempt = 0; attempt < 500; ++attempt) {
            assert(status("/scheduler/batch/status", identity, response) == 0);
            final.Parse(response.c_str());
            const std::string state = final["status"].GetString();
            if (state != "running" && state != "stopping") break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (std::string(final["status"].GetString()) != (profile == "input_failure" ? "failed"
                                                         : cancel                   ? "cancelled"
                                                                                    : "stopped"))
            passed = false;
        const auto sessions = Count(db, "SELECT count(DISTINCT session_id) FROM " + relation);
        const auto final_rows = Count(db, "SELECT count(*) FROM " + relation + " AND is_final=1");
        artifact.AddMember("session_count", sessions, allocator);
        artifact.AddMember("final_rows", final_rows, allocator);
        artifact.AddMember("window_samples", window_samples, allocator);
        artifact.AddMember("run_id", rapidjson::Value(run_id.c_str(), allocator), allocator);
        artifact.AddMember("sql", rapidjson::Value(sql.c_str(), allocator), allocator);
        artifact.AddMember("result_db", rapidjson::Value((root / "results.db").c_str(), allocator), allocator);
        const auto expected_sessions = profile == "reject" ? 0 : 2;
        if (sessions != expected_sessions || final_rows != (cancel || expected_input_failure ? 0 : expected_sessions))
            passed = false;
        rapidjson::Value consistency;
        if (!CheckPeriodicResults(db, relation, interval, profile == "single",
                                  profile != "reject" && profile != "input_failure", &consistency, allocator))
            passed = false;
        artifact.AddMember("result_consistency", consistency, allocator);
        if (profile == "high_reject") {
            const auto rejected_rows =
                Count(db, "SELECT count(*) FROM " + relation + " AND (a_port!=41000 OR b_port!=41001)");
            const auto wrong_bytes =
                Count(db, "SELECT count(*) FROM " + relation +
                              " AND (CAST(wire_bytes_ab AS INTEGER)!=CAST(packets_ab AS INTEGER)*" +
                              std::to_string(frame_bytes) +
                              " OR CAST(wire_bytes_ba AS INTEGER)!=CAST(packets_ba AS INTEGER)*" +
                              std::to_string(frame_bytes) + ")");
            uint64_t selected_prefix_packets = 0;
            for (const auto& prefix : artifact["result_consistency"]["sessions"].GetArray())
                selected_prefix_packets += prefix["packets_ab"].GetUint64() + prefix["packets_ba"].GetUint64();
            if (rejected_rows || wrong_bytes || !selected_prefix_packets ||
                selected_prefix_packets > sender.selected_sent)
                passed = false;
            artifact.AddMember("rejected_result_rows", rejected_rows, allocator);
            artifact.AddMember("wrong_frame_byte_rows", wrong_bytes, allocator);
            artifact.AddMember("selected_prefix_packets", selected_prefix_packets, allocator);
        }
        if (profile == "input_failure") {
            if (!expected_input_failure) passed = false;
            artifact.AddMember("input_failure_observed", expected_input_failure, allocator);
            manager->QueryChannels([&](const auto&, const auto&, const auto& json, const auto&) {
                rapidjson::Document channel;
                channel.Parse(json.c_str());
                if (!channel["readers"].Empty()) passed = false;
            });
        }
        if (profile == "domain_isolation") {
            std::string other_sql = sql;
            other_sql.replace(other_sql.find("netadapter.live"), 15, "netadapter.other");
            const std::string other_request =
                "{\"runtime_task_id\":\"netadapter-other\",\"sql_text\":\"" + other_sql + "\"}";
            assert(submit("/scheduler/batch/submit", other_request, response) == 0);
            for (int attempt = 0; attempt < 500; ++attempt) {
                assert(status("/scheduler/batch/status", R"({"runtime_task_id":"netadapter-other"})", response) == 0);
                rapidjson::Document state;
                state.Parse(response.c_str());
                if (state.HasMember("managed_result") && state["managed_result"].HasMember("run_id")) {
                    other_run_id = state["managed_result"]["run_id"].GetString();
                    break;
                }
                if (state.HasMember("status") && std::string(state["status"].GetString()) == "failed") {
                    std::cerr << "second domain task failed: " << response << "\n";
                    return 1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            assert(!other_run_id.empty() && other_run_id != run_id);
            artifact.AddMember("domain_execution", "sequential", allocator);
            Sender other_sender(argv[5], argv[6], pps, frame_bytes, profile);
            const auto other_until = Clock::now() + std::chrono::seconds(duration);
            while (Clock::now() < other_until) {
                assert(status("/scheduler/batch/status", R"({"runtime_task_id":"netadapter-other"})", response) == 0);
                rapidjson::Document state;
                state.Parse(response.c_str());
                if (std::string(state["status"].GetString()) == "failed") {
                    std::cerr << "second domain task failed: " << response << "\n";
                    passed = false;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            other_sender.Stop();
            artifact.AddMember("other_sent_packets", other_sender.sent.load(), allocator);
            artifact.AddMember("other_send_errors", other_sender.errors.load(), allocator);
            artifact.AddMember("other_run_id", rapidjson::Value(other_run_id.c_str(), allocator), allocator);
            if (other_sender.errors) passed = false;
            assert(stop("/scheduler/batch/stop", R"({"runtime_task_id":"netadapter-other","mode":"stop"})", response) ==
                   0);
            bool other_stopped = false;
            for (int attempt = 0; attempt < 500; ++attempt) {
                assert(status("/scheduler/batch/status", R"({"runtime_task_id":"netadapter-other"})", response) == 0);
                rapidjson::Document state;
                state.Parse(response.c_str());
                if (std::string(state["status"].GetString()) == "stopped") {
                    other_stopped = true;
                    break;
                }
                if (std::string(state["status"].GetString()) == "failed") break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const auto domains = Count(db, "SELECT count(DISTINCT observation_domain_id) FROM npm_basic_history_v1");
            const auto other_sessions =
                Count(db, "SELECT count(DISTINCT session_id) FROM npm_basic_history_v1 WHERE __npm_run_id='" +
                              other_run_id + "'");
            const std::string other_relation = "npm_basic_history_v1 WHERE __npm_run_id='" + other_run_id + "'";
            const auto other_final_rows = Count(db, "SELECT count(*) FROM " + other_relation + " AND is_final=1");
            const auto own_domains = Count(db, "SELECT count(DISTINCT observation_domain_id) FROM " + relation);
            const auto other_domains = Count(db, "SELECT count(DISTINCT observation_domain_id) FROM " + other_relation);
            if (!other_stopped || domains != 2 || own_domains != 1 || other_domains != 1 || other_sessions != 2 ||
                other_final_rows != 2)
                passed = false;
            artifact.AddMember("distinct_observation_domains", domains, allocator);
            artifact.AddMember("other_session_count", other_sessions, allocator);
            artifact.AddMember("other_final_rows", other_final_rows, allocator);
            artifact.AddMember("own_observation_domain_count", own_domains, allocator);
            artifact.AddMember("other_observation_domain_count", other_domains, allocator);
            assert(manager->RemoveChannel("netadapter", "other") == 0);
        }
        if (probe) {
            const auto& result = final["managed_result"];
            const auto& observed = result["validation_probe"];
            if (observed["maintenance_lateness_ms"]["count"].GetUint64() == 0 ||
                observed["maintenance_lateness_ms"]["max"].GetDouble() > 100 || common_advances < 3)
                passed = false;
            for (const auto& value : observed["per_input_service_max_ms"].GetArray())
                if (value.GetDouble() > 100) passed = false;
            artifact.AddMember("per_input_service_max_ms",
                               rapidjson::Value(observed["per_input_service_max_ms"], allocator), allocator);
            if (profile == "slow_analysis" && !observed["analysis_sleeps"].GetUint64()) passed = false;
            if (profile == "slow_output" && !observed["output_sleeps"].GetUint64()) passed = false;
            if (profile == "overload_recovery" &&
                (!pressure_observed || recovery_s < 0 || recovery_s > 5 || recovery_advances < 3))
                passed = false;
            if (profile == "input_failure" && (result["diagnostics"]["tracked_bytes"].GetUint64() != 0 ||
                                               result["diagnostics"]["pending_output_bytes"].GetUint64() != 0))
                passed = false;
            artifact.AddMember("analysis_budget_usage", rapidjson::Value(result["diagnostics"], allocator), allocator);
            artifact.AddMember("maintenance_latency_ms",
                               rapidjson::Value(observed["maintenance_lateness_ms"], allocator), allocator);
            artifact.AddMember("maintenance_service_gap_ms",
                               rapidjson::Value(observed["maintenance_service_gap_ms"], allocator), allocator);
            artifact.AddMember("common_candidate_advances", common_advances, allocator);
            artifact.AddMember("pressure_observed", pressure_observed, allocator);
            artifact.AddMember("recovery_first_progress_s", recovery_s, allocator);
            artifact.AddMember("recovery_advances", recovery_advances, allocator);
            artifact.AddMember("sql_samples", sql_samples, allocator);
        }
        artifact.AddMember("final_status", rapidjson::Value(final, allocator), allocator);
        submit = {};
        status = {};
        stop = {};
        assert(addon->DeactivateCppPlugin(plugin_id, response) == 0);
    }
    auto latency = [&](const char* key, std::vector<double> values) {
        rapidjson::Value result(rapidjson::kObjectType);
        if (!values.empty()) {
            std::sort(values.begin(), values.end());
            result.AddMember("p50", values[values.size() / 2], allocator);
            result.AddMember("p99", values[std::min(values.size() - 1, values.size() * 99 / 100)], allocator);
            result.AddMember("max", values.back(), allocator);
        }
        artifact.AddMember(rapidjson::Value(key, allocator), result, allocator);
    };
    latency(capture ? "poll_ms" : "status_request_ms", poll_ms);
    latency("batch_packets", batch_packets);
    latency("batch_captured_bytes", batch_bytes);
    latency("common_watermark_lag_ms", watermark_lag_ms);
    latency("window_close_observation_ms", window_close_ms);
    artifact.AddMember("capture_allocated_peak_bytes", peak_capture_bytes, allocator);
    artifact.AddMember("channel_samples", samples, allocator);
    if (!probe) {
        artifact.AddMember("analysis_budget_usage", rapidjson::Value(rapidjson::kNullType), allocator);
        artifact.AddMember("maintenance_latency_ms", rapidjson::Value(rapidjson::kNullType), allocator);
    }
    const auto elapsed = std::chrono::duration<double>(Clock::now() - measurement_start).count();
    rusage usage_end{};
    assert(getrusage(RUSAGE_SELF, &usage_end) == 0);
    auto cpu_seconds = [](const rusage& usage) {
        return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
               double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000000;
    };
    artifact.AddMember("process_cpu_seconds", cpu_seconds(usage_end) - cpu_seconds(usage_start), allocator);
    artifact.AddMember("measurement_elapsed_s", elapsed, allocator);
    artifact.AddMember("process_peak_rss_kib", uint64_t(usage_end.ru_maxrss), allocator);
    rapidjson::Document channel;
    channel.Parse(diagnostics.c_str());
    artifact.AddMember("channel", rapidjson::Value(channel, allocator), allocator);
    artifact.AddMember("passed_observed_checks", passed, allocator);
    std::ofstream output(argv[12]);
    output << Json(artifact) << "\n";
    output.close();
    assert(output.good());
    assert(manager->RemoveChannel("netadapter", "live") == 0);
    std::cout << "validation artifact: " << argv[12] << " passed=" << passed << "\n";
    return passed ? 0 : 1;
}
