// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <framework/interfaces/idatabase_arrow_write.h>
#include <services/database/drivers/clickhouse_driver.h>

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <httplib.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <limits>
#include <mutex>
#include <thread>

namespace {

class LimitedPool final : public arrow::MemoryPool {
 public:
    explicit LimitedPool(int64_t limit) : limit_(limit), backing_(arrow::default_memory_pool()) {}
    arrow::Status Allocate(int64_t size, int64_t alignment, uint8_t** out) override {
        if (size > limit_ - bytes_allocated()) return arrow::Status::OutOfMemory("test pool exhausted");
        return backing_.Allocate(size, alignment, out);
    }
    arrow::Status Reallocate(int64_t old_size, int64_t size, int64_t alignment, uint8_t** out) override {
        if (size - old_size > limit_ - bytes_allocated()) return arrow::Status::OutOfMemory("test pool exhausted");
        return backing_.Reallocate(old_size, size, alignment, out);
    }
    void Free(uint8_t* data, int64_t size, int64_t alignment) override { backing_.Free(data, size, alignment); }
    int64_t bytes_allocated() const override { return backing_.bytes_allocated(); }
    int64_t max_memory() const override { return backing_.max_memory(); }
    int64_t total_bytes_allocated() const override { return backing_.total_bytes_allocated(); }
    int64_t num_allocations() const override { return backing_.num_allocations(); }
    std::string backend_name() const override { return "test_limited"; }

 private:
    int64_t limit_;
    arrow::ProxyMemoryPool backing_;
};

std::shared_ptr<arrow::RecordBatch> MakeRows() {
    arrow::UInt64Builder ids;
    arrow::StringBuilder text;
    arrow::FixedSizeBinaryBuilder addresses(arrow::fixed_size_binary(16));
    arrow::Int64Builder nullable;
    const std::string special("quote'&%\\\0tail", 14);
    for (int row = 0; row < 2; ++row) {
        assert(ids.Append(row ? std::numeric_limits<uint64_t>::max() : 0).ok());
        assert(text.Append(row ? special : "").ok());
        uint8_t bytes[16]{};
        bytes[0] = 0x20;
        bytes[1] = 0x01;
        bytes[15] = row ? 0xff : 0;
        assert(addresses.Append(bytes).ok());
        assert((row ? nullable.Append(-7) : nullable.AppendNull()).ok());
    }
    std::vector<std::shared_ptr<arrow::Array>> columns;
    for (arrow::ArrayBuilder* builder : std::vector<arrow::ArrayBuilder*>{&ids, &text, &addresses, &nullable}) {
        auto result = builder->Finish();
        assert(result.ok());
        columns.push_back(*result);
    }
    return arrow::RecordBatch::Make(
        arrow::schema({arrow::field("id", arrow::uint64(), false), arrow::field("text", arrow::utf8(), false),
                       arrow::field("ip", arrow::fixed_size_binary(16), false),
                       arrow::field("nullable", arrow::int64(), true)}),
        2, std::move(columns));
}

class Server final {
 public:
    explicit Server(std::shared_ptr<arrow::RecordBatch> expected) : expected_(std::move(expected)) {
        server_.Get(
            "/", [](const httplib::Request&, httplib::Response& response) { response.set_content("1", "text/plain"); });
        server_.Post("/", [this](const httplib::Request& request, httplib::Response& response) {
            assert(request.get_param_value("query") == "INSERT INTO `probe``table` FORMAT ArrowStream");
            assert(request.get_header_value("Content-Type") == "application/octet-stream");
            for (const auto& parameter : request.params) assert(parameter.first.rfind("param_", 0) != 0);
            auto buffer = arrow::Buffer::FromString(request.body);
            auto reader = arrow::ipc::RecordBatchStreamReader::Open(std::make_shared<arrow::io::BufferReader>(buffer));
            assert(reader.ok());
            std::shared_ptr<arrow::RecordBatch> batch;
            assert((*reader)->ReadNext(&batch).ok() && batch && batch->Equals(*expected_));
            assert((*reader)->ReadNext(&batch).ok() && !batch);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ports_.push_back(request.remote_port);
            }
            response.status = fail_next.exchange(false) ? 500 : 200;
            response.set_content(response.status == 200 ? "" : "expected write failure", "text/plain");
        });
        port = server_.bind_to_any_port("127.0.0.1");
        assert(port > 0);
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!server_.is_running() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        assert(server_.is_running());
    }
    ~Server() { Stop(); }
    std::vector<int> Ports() {
        std::lock_guard<std::mutex> lock(mutex_);
        return ports_;
    }
    void Stop() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }
    int port = 0;
    std::atomic<bool> fail_next{false};

 private:
    std::shared_ptr<arrow::RecordBatch> expected_;
    httplib::Server server_;
    std::thread thread_;
    std::mutex mutex_;
    std::vector<int> ports_;
};

}  // namespace

int main() {
    auto rows = MakeRows();
    Server server(rows);
    flowsql::database::ClickHouseDriver driver;
    assert(driver.Connect(
               {{"host", "127.0.0.1"}, {"port", std::to_string(server.port)}, {"database", "columnar_test"}}) == 0);
    auto first = driver.CreateSession();
    auto second = driver.CreateSession();
    auto* writer = dynamic_cast<flowsql::IDatabaseArrowWriteV1*>(first.get());
    auto* next_writer = dynamic_cast<flowsql::IDatabaseArrowWriteV1*>(second.get());
    assert(writer && next_writer && "ClickHouse session must expose budgeted Arrow write capability");
    LimitedPool pool(4 * 1024 * 1024);
    assert(writer->WriteArrowBatchesWithPool("probe`table", {rows}, &pool) == 0);
    assert(pool.max_memory() > 0 && pool.bytes_allocated() == 0);
    assert(next_writer->WriteArrowBatchesWithPool("probe`table", {rows}, &pool) == 0);
    auto ports = server.Ports();
    assert(ports.size() == 2 && ports[0] == ports[1]);

    LimitedPool exhausted(0);
    assert(writer->WriteArrowBatchesWithPool("probe`table", {rows}, &exhausted) != 0);
    assert(exhausted.bytes_allocated() == 0 && server.Ports().size() == 2);
    assert(writer->WriteArrowBatchesWithPool(nullptr, {rows}, &pool) != 0);
    assert(writer->WriteArrowBatchesWithPool("probe`table", {rows}, nullptr) != 0);
    assert(pool.bytes_allocated() == 0 && server.Ports().size() == 2);

    server.fail_next = true;
    assert(writer->WriteArrowBatchesWithPool("probe`table", {rows}, &pool) != 0);
    assert(std::string(first->GetLastError()).find("expected write failure") != std::string::npos);
    assert(pool.bytes_allocated() == 0 && server.Ports().size() == 3);

    std::vector<std::shared_ptr<flowsql::database::IDbSession>> sessions;
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        sessions.push_back(driver.CreateSession());
        threads.emplace_back([session = sessions.back(), rows] {
            LimitedPool concurrent_pool(4 * 1024 * 1024);
            auto* columnar = dynamic_cast<flowsql::IDatabaseArrowWriteV1*>(session.get());
            assert(columnar && columnar->WriteArrowBatchesWithPool("probe`table", {rows}, &concurrent_pool) == 0);
            assert(concurrent_pool.bytes_allocated() == 0);
        });
    }
    for (auto& thread : threads) thread.join();
    ports = server.Ports();
    assert(ports.size() == 7);
    for (int port : ports) assert(port == ports.front());
    assert(driver.Disconnect() == 0 && !driver.CreateSession());
    assert(next_writer->WriteArrowBatchesWithPool("probe`table", {rows}, &pool) == 0);
    assert(pool.bytes_allocated() == 0 && server.Ports().size() == 8);
    server.Stop();
    assert(next_writer->WriteArrowBatchesWithPool("probe`table", {rows}, &pool) != 0);
    assert(pool.bytes_allocated() == 0 && server.Ports().size() == 8);
    std::puts("[PASS] Arrow body, exact values, budget, connection reuse, concurrency, no retry, ownership");
}
