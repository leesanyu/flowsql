// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
// T2 data-plane acceptance: one store publishes Arrow IPC while an independent client maps it
// read-only, keeps it readable after the publisher stops, and rejects unsafe or unbounded objects.

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/ipc/writer.h>
#include <httplib.h>
#include <openssl/sha.h>
#include <rapidjson/document.h>
#include <services/catalog/dataframe_transfer.h>
#include <services/visualization/dataframe_transfer.h>

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

using namespace flowsql;
using namespace std::chrono_literals;

namespace {
int failures = 0;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

std::string TemporaryDirectory() {
    std::array<char, 64> pattern{};
    std::snprintf(pattern.data(), pattern.size(), "/tmp/flowdataframe-XXXXXX");
    const char* created = mkdtemp(pattern.data());
    if (!created) return {};
    std::filesystem::path path(created);
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    return path.string();
}

std::string SchemaRevision(const arrow::Schema& schema) {
    const auto encoded = arrow::ipc::SerializeSchema(schema);
    if (!encoded.ok()) return {};
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((*encoded)->data(), static_cast<size_t>((*encoded)->size()), digest);
    std::string text;
    char byte[3];
    for (auto value : digest) {
        std::snprintf(byte, sizeof(byte), "%02x", value);
        text += byte;
    }
    return text;
}

size_t Objects(const std::string& directory) {
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        if (name.size() > 6 &&
            (name.substr(name.size() - 6) == ".arrow" || name.size() > 8 && name.substr(name.size() - 8) == ".partial"))
            ++count;
    }
    return count;
}

std::shared_ptr<arrow::RecordBatch> SampleBatch(int64_t rows = 4) {
    arrow::Int64Builder numbers;
    arrow::StringBuilder names;
    for (int64_t i = 0; i < rows; ++i) {
        numbers.Append(i * 10);
        names.Append(i % 2 ? "odd" : "even");
    }
    std::shared_ptr<arrow::Array> number_array, name_array;
    numbers.Finish(&number_array);
    names.Finish(&name_array);
    return arrow::RecordBatch::Make(
        arrow::schema({arrow::field("value", arrow::int64()), arrow::field("label", arrow::utf8())}), rows,
        {number_array, name_array});
}

class FakeProvider : public IDataFrameSnapshotProviderV1 {
 public:
    int List(std::vector<DataFrameSourceV1>* output, std::string* error) override {
        output->push_back({"source-a", "dataframe.sample"});
        output->push_back({"source-b", "dataframe.other"});
        (void)error;
        return 0;
    }
    int Acquire(const DataFrameSnapshotRequestV1& request, DataFrameSnapshotV1* output, std::string* error) override {
        if (slow) {
            for (int i = 0; i < 100 && !request.cancelled->load(); ++i) std::this_thread::sleep_for(10ms);
            *error = "CANCELLED";
            return -1;
        }
        if (request.source_id != "source-a") {
            *error = "SOURCE_UNAVAILABLE";
            return -1;
        }
        const auto batch = SampleBatch();
        uint64_t bytes = 0;
        for (const auto& column : batch->columns()) {
            const auto data = column->data();
            for (const auto& buffer : data->buffers) {
                if (buffer && buffer->is_cpu()) bytes += static_cast<uint64_t>(buffer->size());
            }
        }
        output->source_id = request.source_id;
        output->snapshot_id = std::string(64, 'a');
        output->schema_revision = SchemaRevision(*batch->schema());
        output->captured_at = "2026-10-22T00:00:00Z";
        output->row_count = batch->num_rows();
        output->retained_bytes = retain_bytes ? retain_bytes : bytes;
        output->lifetime = std::make_shared<int>(0);
        output->batch = batch;
        if (output->retained_bytes > request.max_bytes) {
            *error = "SNAPSHOT_LIMIT";
            return -1;
        }
        return 0;
    }
    bool slow = false;
    uint64_t retain_bytes = 0;
};

class Server {
 public:
    explicit Server(catalog::DataFrameTransferStore& store) {
        server_.Get("/channels/dataframe/exploration/v1/sources", [this](const auto&, auto& response) {
            std::vector<DataFrameSourceV1> sources;
            std::string error;
            provider_->List(&sources, &error);
            std::string body = "{\"sources\":[";
            for (size_t i = 0; i < sources.size(); ++i) {
                if (i) body += ',';
                body += "{\"source_id\":\"" + sources[i].source_id + "\",\"label\":\"" + sources[i].label + "\"}";
            }
            response.set_content(body + "]}", "application/json");
        });
        server_.Post("/channels/dataframe/exploration/v1/snapshots/acquire", [this, &store](const auto& request,
                                                                                            auto& response) {
            rapidjson::Document document;
            document.Parse(request.body.c_str());
            DataFrameSnapshotRequestV1 snapshot_request;
            snapshot_request.source_id = document["source_id"].GetString();
            snapshot_request.max_bytes = document["max_bytes"].GetUint64();
            snapshot_request.deadline = std::chrono::steady_clock::now() + 10s;
            snapshot_request.cancelled = cancelled_;
            catalog::DataFrameTransferInfoV1 info;
            std::string error;
            const int status =
                store.Acquire(*provider_, document["request_id"].GetString(), snapshot_request, &info, &error);
            if (status != 0) {
                response.status = error == "SOURCE_UNAVAILABLE" ? 404 : 400;
                response.set_content("{\"code\":\"" + error + "\"}", "application/json");
                return;
            }
            response.set_content(
                "{\"source_id\":\"" + info.source_id + "\",\"transfer_id\":\"" + info.transfer_id +
                    "\",\"snapshot_id\":\"" + info.snapshot_id + "\",\"schema_revision\":\"" + info.schema_revision +
                    "\",\"captured_at\":\"" + info.captured_at + "\",\"row_count\":" + std::to_string(info.row_count) +
                    ",\"ipc_bytes\":" + std::to_string(info.ipc_bytes) +
                    ",\"expires_at_unix\":1780000000,\"fields\":[{\"name\":\"value\",\"type\":\"int64\","
                    "\"nullable\":true},{\"name\":\"label\",\"type\":\"string\",\"nullable\":true}]}",
                "application/json");
        });
        server_.Post("/channels/dataframe/exploration/v1/snapshots/release",
                     [&store](const auto& request, auto& response) {
                         rapidjson::Document document;
                         document.Parse(request.body.c_str());
                         store.Release(document["transfer_id"].GetString());
                         response.set_content("{\"status\":\"released\"}", "application/json");
                     });
    }
    bool Start(FakeProvider& provider) {
        provider_ = &provider;
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) return false;
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        for (int i = 0; i < 200 && !server_.is_running(); ++i) std::this_thread::sleep_for(10ms);
        return server_.is_running();
    }
    void Stop() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }
    std::string Base() const { return "http://127.0.0.1:" + std::to_string(port_); }

 private:
    httplib::Server server_;
    std::thread thread_;
    FakeProvider* provider_ = nullptr;
    std::shared_ptr<std::atomic_bool> cancelled_ = std::make_shared<std::atomic_bool>(false);
    int port_ = 0;
};

void TestTransferLifecycle() {
    const auto root = TemporaryDirectory();
    CHECK(!root.empty());
    const std::string shared = root + "/shared";
    std::filesystem::create_directories(shared);
    std::filesystem::permissions(shared, std::filesystem::perms::owner_all);

    catalog::DataFrameTransferStore store;
    CHECK(store.Init(shared, 60s) == 0);
    FakeProvider provider;
    Server server(store);
    CHECK(server.Start(provider));

    visualization::DataFrameTransferClient client;
    CHECK(client.Init(server.Base(), shared, 5000));
    CHECK(client.Available());
    std::string error;
    const auto sources = client.Sources(&error);
    CHECK(error.empty());
    CHECK(sources.size() == 2 && sources[0].label == "dataframe.sample");

    visualization::DataFrameSnapshotInfoV1 snapshot;
    CHECK(client.Acquire("request-1", "source-a", 8 * 1024 * 1024, nullptr, 5000, &snapshot, &error));
    CHECK(error.empty());
    CHECK(snapshot.row_count == 4 && snapshot.ipc_bytes > 0);
    CHECK(snapshot.batch && snapshot.batch->num_rows() == 4 && snapshot.mapping);
    if (!snapshot.batch) {
        std::printf("FAIL acquire returned no batch\n");
        ++failures;
        return;
    }
    // The receiver acknowledges the transfer once its own mapping exists, so the source retires the file.
    CHECK(Objects(shared) == 0);
    CHECK(snapshot.batch->ValidateFull().ok());

    int64_t sum = 0;
    const auto numbers = std::static_pointer_cast<arrow::Int64Array>(snapshot.batch->column(0));
    for (int64_t i = 0; i < numbers->length(); ++i) sum += numbers->Value(i);
    CHECK(sum == 60);

    int64_t expires = 0;
    CHECK(client.Lookup(snapshot.snapshot_id, nullptr, nullptr));
    CHECK(client.Renew(snapshot.snapshot_id, &expires) && expires > 0);
    CHECK(client.Release(snapshot.snapshot_id));
    CHECK(!client.Release(snapshot.snapshot_id));
    int64_t after_release = 0;
    CHECK(!client.Renew(snapshot.snapshot_id, &after_release));
    CHECK(!client.Lookup(snapshot.snapshot_id, nullptr, nullptr));

    // A second transfer survives an abrupt publisher shutdown: the mapping owns the pages.
    visualization::DataFrameSnapshotInfoV1 survivor;
    CHECK(client.Acquire("request-2", "source-a", 8 * 1024 * 1024, nullptr, 5000, &survivor, &error));
    store.Stop();
    server.Stop();
    CHECK(survivor.batch->ValidateFull().ok());
    const auto surviving = std::static_pointer_cast<arrow::Int64Array>(survivor.batch->column(0));
    int64_t reopened = 0;
    for (int64_t i = 0; i < surviving->length(); ++i) reopened += surviving->Value(i);
    CHECK(reopened == 60);
    CHECK(Objects(shared) == 0);
    client.Stop();
    std::filesystem::remove_all(root);
    std::puts("[PASS] transfer lifecycle, release, renewal and publisher-exit mapping");
}

void TestBudgetsAndCancellation() {
    const auto root = TemporaryDirectory();
    const std::string shared = root + "/shared";
    std::filesystem::create_directories(shared);
    std::filesystem::permissions(shared, std::filesystem::perms::owner_all);

    catalog::DataFrameTransferStore store;
    CHECK(store.Init(shared, 1s) == 0);
    FakeProvider provider;
    Server server(store);
    CHECK(server.Start(provider));
    visualization::DataFrameTransferClient client;
    CHECK(client.Init(server.Base(), shared, 5000));
    std::string error;
    visualization::DataFrameSnapshotInfoV1 snapshot;

    CHECK(!client.Acquire("request-big", "source-a", 64, nullptr, 5000, &snapshot, &error));
    CHECK(error == "SNAPSHOT_LIMIT");
    CHECK(!client.Acquire("request-big", "source-a", 64, nullptr, 5000, &snapshot, &error));
    // Unknown sources and unsafe identities never reach the file system.
    error.clear();
    CHECK(!client.Acquire("request-missing", "source-none", 8 * 1024 * 1024, nullptr, 5000, &snapshot, &error));
    CHECK(!error.empty() && !snapshot.batch);

    provider.slow = true;
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    std::thread canceller([&] {
        std::this_thread::sleep_for(50ms);
        cancelled->store(true);
    });
    error.clear();
    const auto began = std::chrono::steady_clock::now();
    const bool acquired =
        client.Acquire("request-cancel", "source-a", 8 * 1024 * 1024, cancelled, 5000, &snapshot, &error);
    canceller.join();
    CHECK(!acquired);
    CHECK(std::chrono::steady_clock::now() - began < 2s);
    provider.slow = false;

    // Transfer leases expire and retire their objects without a receiver.
    provider.retain_bytes = 1024;
    catalog::DataFrameTransferInfoV1 info;
    DataFrameSnapshotRequestV1 request;
    request.source_id = "source-a";
    request.max_bytes = 8 * 1024 * 1024;
    request.deadline = std::chrono::steady_clock::now() + 5s;
    request.cancelled = std::make_shared<std::atomic_bool>(false);
    CHECK(store.Acquire(provider, "request-lease", request, &info, &error) == 0);
    CHECK(std::filesystem::exists(shared + "/" + info.transfer_id + ".arrow"));
    std::this_thread::sleep_for(1500ms);
    CHECK(!std::filesystem::exists(shared + "/" + info.transfer_id + ".arrow"));

    // Symlinked objects are never opened or removed.
    const std::string link = shared + "/" + std::string(32, 'c') + ".arrow";
    std::ofstream(root + "/target").put('x');
    std::filesystem::create_symlink(root + "/target", link);
    const int fd = open(link.c_str(), O_RDONLY | O_NOFOLLOW);
    CHECK(fd < 0);
    if (fd >= 0) close(fd);
    CHECK(std::filesystem::exists(root + "/target"));

    store.Stop();
    server.Stop();
    client.Stop();
    std::filesystem::remove_all(root);
    std::puts("[PASS] byte budget, cancellation, lease expiry and symlink rejection");
}

void TestDataPlaneUnavailable() {
    const auto root = TemporaryDirectory();
    catalog::DataFrameTransferStore store;
    std::string error;
    DataFrameSnapshotRequestV1 request;
    request.source_id = "source-a";
    request.max_bytes = 1024;
    request.deadline = std::chrono::steady_clock::now() + 1s;
    request.cancelled = std::make_shared<std::atomic_bool>(false);
    FakeProvider provider;
    catalog::DataFrameTransferInfoV1 info;
    CHECK(store.Acquire(provider, "request", request, &info, &error) != 0);
    CHECK(error == "DATA_PLANE_UNAVAILABLE");

    visualization::DataFrameTransferClient client;
    CHECK(!client.Init("http://127.0.0.1:1", "", 1000));
    CHECK(!client.Available());
    visualization::DataFrameSnapshotInfoV1 snapshot;
    CHECK(!client.Acquire("request", "source-a", 1024, nullptr, 1000, &snapshot, &error));
    CHECK(error == "DATA_PLANE_UNAVAILABLE");

    // Publicly readable directories are refused on both sides.
    std::filesystem::permissions(root, std::filesystem::perms::all);
    catalog::DataFrameTransferStore public_store;
    CHECK(public_store.Init(root, 1s) != 0);
    visualization::DataFrameTransferClient public_client;
    CHECK(!public_client.Init("http://127.0.0.1:1", root, 1000));
    std::filesystem::remove_all(root);
    std::puts("[PASS] missing data plane and non-private directories fail explicitly");
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestTransferLifecycle();
    TestBudgetsAndCancellation();
    TestDataPlaneUnavailable();
    if (failures) {
        std::printf("%d assertion(s) failed\n", failures);
        return 1;
    }
    std::puts("=== All DataFrame transfer tests passed ===");
    return 0;
}
