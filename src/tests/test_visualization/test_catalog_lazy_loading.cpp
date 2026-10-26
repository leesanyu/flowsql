// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <rapidjson/document.h>
#include <services/catalog/catalog_plugin.h>
#include <services/catalog/lazy_dataframe_channel.h>
#include <sys/inotify.h>
#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>

using namespace flowsql;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {
std::string Route(catalog::CatalogPlugin& plugin, const std::string& uri) {
    std::string response;
    bool found = false;
    plugin.EnumRoutes([&](const RouteItem& item) {
        if (item.uri == uri) {
            assert(item.handler(uri, "", response) == 0);
            found = true;
        }
    });
    assert(found);
    return response;
}
void Sample(DataFrame& frame, int64_t value = 7) {
    frame.SetSchema({{"n", DataType::INT64, 0, ""}});
    assert(frame.AppendRow({value}) == 0);
}
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    std::atomic_int calls{0};
    void Wait() {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, 2s, [&] { return entered; }));
    }
    void Release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        changed.notify_all();
    }
    std::shared_ptr<IDataFrameChannel> Load(std::istream&, const std::string& name,
                                            const catalog::LazyDataFrameChannel::Check& interrupted) {
        ++calls;
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        while (!released) {
            if (interrupted()) return {};
            changed.wait_for(lock, 1ms);
        }
        auto channel = std::make_shared<DataFrameChannel>("dataframe", name);
        DataFrame frame;
        Sample(frame);
        channel->Open();
        channel->Write(&frame);
        return channel;
    }
};
auto MakeLazy(const fs::path& path, Gate& gate) {
    return std::make_shared<catalog::LazyDataFrameChannel>(
        path.string(), "test",
        [&](auto& input, const auto& name, const auto& check) { return gate.Load(input, name, check); });
}
auto Request() {
    DataFrameSnapshotRequestV1 request;
    request.max_bytes = 1024 * 1024;
    request.deadline = std::chrono::steady_clock::now() + 2s;
    request.cancelled = std::make_shared<std::atomic_bool>(false);
    return request;
}
void Concurrency(const fs::path& path) {
    Gate gate;
    auto channel = MakeLazy(path, gate);
    const auto read = [&] {
        DataFrame frame;
        return channel->Read(&frame) == 0 && frame.RowCount() == 1;
    };
    auto owner = std::async(std::launch::async, read);
    gate.Wait();
    assert(channel->Describe().state == "loading");
    assert(std::string(channel->Schema()) == "null");
    std::vector<std::future<bool>> readers;
    for (int i = 0; i < 8; ++i) readers.push_back(std::async(std::launch::async, read));
    auto request = Request();
    auto waiter = std::async(std::launch::async, [&] {
        std::shared_ptr<arrow::RecordBatch> batch;
        uint64_t bytes = 0;
        std::string error;
        assert(channel->CaptureSnapshot(request, &batch, &bytes, &error) != 0);
        assert(!batch && bytes == 0);
        return error;
    });
    assert(waiter.wait_for(20ms) == std::future_status::timeout);
    request.cancelled->store(true);
    assert(waiter.wait_for(1s) == std::future_status::ready && waiter.get() == "CANCELLED");
    auto timeout = Request();
    timeout.deadline = std::chrono::steady_clock::now() + 10ms;
    std::shared_ptr<arrow::RecordBatch> batch;
    uint64_t bytes = 0;
    std::string error;
    assert(channel->CaptureSnapshot(timeout, &batch, &bytes, &error) != 0 && error == "TIMEOUT");
    gate.Release();
    assert(owner.get());
    for (auto& reader : readers) assert(reader.get());
    assert(gate.calls == 1 && read() && gate.calls == 1);
    assert(channel->Describe().state == "loaded");
    DataFrame append;
    Sample(append, 8);
    assert(channel->Append(&append) == 0 && channel->Describe().rows == 2);
    DataFrame wrong;
    wrong.SetSchema({{"wrong", DataType::STRING, 0, ""}});
    assert(channel->Append(&wrong) != 0);
    auto snapshot_request = Request();
    assert(channel->CaptureSnapshot(snapshot_request, &batch, &bytes, &error) == 0 && batch->num_rows() == 2);
    assert(channel->Close() == 0 && batch->num_rows() == 2);
    assert(channel->Open() == 0 && read() && gate.calls == 2);
    channel->Retire();
    assert(channel->Open() != 0 && !channel->IsOpened() && !read());

    for (bool expire : {false, true}) {
        Gate parser;
        auto cancelled_channel = MakeLazy(path, parser);
        auto capture = Request();
        if (expire) capture.deadline = std::chrono::steady_clock::now() + 30ms;
        auto pending = std::async(std::launch::async, [&] {
            std::shared_ptr<arrow::RecordBatch> output;
            uint64_t size = 0;
            std::string message;
            assert(cancelled_channel->CaptureSnapshot(capture, &output, &size, &message) != 0);
            assert(!output && size == 0);
            return message;
        });
        parser.Wait();
        if (!expire) capture.cancelled->store(true);
        assert(pending.wait_for(1s) == std::future_status::ready);
        assert(pending.get() == (expire ? "TIMEOUT" : "CANCELLED"));
        assert(cancelled_channel->Describe().state == "unloaded");
        parser.Release();
        DataFrame frame;
        assert(cancelled_channel->Read(&frame) == 0 && parser.calls == 2);
    }
    for (bool replace : {false, true}) {
        Gate parser;
        auto changing = MakeLazy(path, parser);
        auto pending = std::async(std::launch::async, [&] {
            DataFrame frame;
            return changing->Read(&frame);
        });
        parser.Wait();
        DataFrame replacement;
        Sample(replacement, 9);
        if (replace)
            assert(changing->Write(&replacement) == 0);
        else
            assert(changing->Close() == 0);
        assert(pending.wait_for(1s) == std::future_status::ready && pending.get() != 0);
        DataFrame frame;
        if (replace) {
            assert(changing->Read(&frame) == 0 && std::get<int64_t>(frame.GetRow(0)[0]) == 9);
            assert(parser.calls == 1);
        } else {
            assert(!changing->IsOpened());
            parser.Release();
            assert(changing->Open() == 0 && changing->Read(&frame) == 0 && parser.calls == 2);
        }
    }
    std::cout
        << "PASS shared load, waiter/parser budgets, append/schema, close/reopen, replacement and snapshot ownership\n";
}
void Management(const fs::path& root) {
    const auto path = root / "manage";
    fs::create_directory(path);
    std::ofstream(path / "rename.csv") << "n\n5\n";
    std::ofstream(path / "remove.csv") << "n\n6\n";
    catalog::CatalogPlugin plugin;
    plugin.Option(("data_dir=" + path.string() + ";operator_db_path=" + (root / "manage.db").string()).c_str());
    assert(plugin.Start() == 0);
    std::vector<DataFrameSourceV1> sources;
    std::string error;
    plugin.List(&sources, &error);
    std::string source_id;
    for (const auto& source : sources)
        if (source.label == "dataframe.rename") source_id = source.source_id;
    const int watcher = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    assert(watcher >= 0 && inotify_add_watch(watcher, path.c_str(), IN_OPEN | IN_ACCESS) >= 0);
    auto removed = plugin.Get("remove");
    assert(plugin.Rename("rename", "renamed") == 0);
    assert(!plugin.Get("rename") && std::string(plugin.Get("renamed")->Name()) == "renamed");
    plugin.List(&sources, &error);
    for (const auto& source : sources)
        if (source.label == "dataframe.renamed") assert(source.source_id == source_id);
    assert(plugin.Unregister("remove") == 0 && !plugin.Get("remove"));
    assert(!removed->IsOpened() && removed->Open() != 0 && !fs::exists(path / "remove.csv"));
    char events[4096];
    assert(read(watcher, events, sizeof(events)) == -1 && errno == EAGAIN);
    DataFrame frame;
    auto renamed = std::dynamic_pointer_cast<IDataFrameChannel>(plugin.Get("renamed"));
    assert(renamed->Read(&frame) == 0 && std::get<int64_t>(frame.GetRow(0)[0]) == 5);
    plugin.Stop();
    assert(plugin.Start() == 0 && plugin.Get("renamed") && !plugin.Get("remove"));
    assert(std::string(plugin.Get("renamed")->Schema()) == "null");
    plugin.Stop();
    close(watcher);

    // Rename during parsing keeps the already-open file alive; a later reopen uses the new path.
    Gate gate;
    auto loading = MakeLazy(path / "renamed.csv", gate);
    auto pending = std::async(std::launch::async, [&] {
        DataFrame data;
        return loading->Read(&data);
    });
    gate.Wait();
    assert(loading->RenameFile((path / "moved.csv").string(), "moved") == 0);
    gate.Release();
    assert(pending.get() == 0 && std::string(loading->Name()) == "moved");
    loading->Close();
    loading->Open();
    assert(loading->Read(&frame) == 0 && gate.calls == 2);
    Gate retiring;
    auto deleted = MakeLazy(path / "moved.csv", retiring);
    auto active = std::async(std::launch::async, [&] {
        DataFrame data;
        return deleted->Read(&data);
    });
    retiring.Wait();
    assert(fs::remove(path / "moved.csv"));
    deleted->Retire();
    assert(active.wait_for(1s) == std::future_status::ready && active.get() != 0);
    assert(deleted->Open() != 0 && deleted->Describe().rows == -1);
    std::cout << "PASS unloaded rename/delete, source identity, restart and in-flight rename/retirement\n";
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--probe") {
        catalog::CatalogPlugin plugin;
        const fs::path directory(argv[2]);
        plugin.Option(
            ("data_dir=" + directory.string() + ";operator_db_path=" + (directory.parent_path() / "meta.db").string())
                .c_str());
        const auto start = std::chrono::steady_clock::now();
        assert(plugin.Start() == 0);
        const auto started = std::chrono::steady_clock::now();
        Route(plugin, "/channels/dataframe");
        const auto listed = std::chrono::steady_clock::now();
        rusage startup_usage{};
        getrusage(RUSAGE_SELF, &startup_usage);
        std::shared_ptr<IDataFrameChannel> first;
        plugin.List([&](const char*, std::shared_ptr<IChannel> channel) {
            if (!first) first = std::dynamic_pointer_cast<IDataFrameChannel>(channel);
        });
        DataFrame frame;
        if (first) assert(first->Read(&frame) == 0);
        const auto read = std::chrono::steady_clock::now();
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        std::cout << "{\"start_ms\":" << ms(start, started) << ",\"list_ms\":" << ms(started, listed)
                  << ",\"first_read_ms\":" << ms(listed, read)
                  << ",\"startup_peak_rss_kib\":" << startup_usage.ru_maxrss << ",\"peak_rss_kib\":" << usage.ru_maxrss
                  << ",\"rows\":" << frame.RowCount() << "}\n";
        plugin.Stop();
        return 0;
    }
    char pattern[] = "/tmp/flowsql-lazy-XXXXXX";
    const fs::path root(mkdtemp(pattern));
    const auto data = root / "data";
    fs::create_directory(data);
    std::ofstream(data / "numbers.csv") << "id,value,label\n1,2.5,hello\n2,3.5,world\n";
    std::ofstream(data / "broken.csv");
    const int watcher = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    assert(watcher >= 0);
    assert(inotify_add_watch(watcher, data.c_str(), IN_OPEN | IN_ACCESS) >= 0);
    catalog::CatalogPlugin plugin;
    plugin.Option(("data_dir=" + data.string() + ";operator_db_path=" + (root / "meta.db").string()).c_str());
    assert(plugin.Start() == 0);
    auto channel = std::dynamic_pointer_cast<IDataFrameChannel>(plugin.Get("numbers"));
    assert(channel && channel->IsOpened());
    assert(std::string(channel->Schema()) == "null");
    int count = 0;
    plugin.List([&](const char*, std::shared_ptr<IChannel> item) {
        ++count;
        assert(std::string(item->Schema()) == "null");
    });
    assert(count == 2);
    std::vector<DataFrameSourceV1> sources;
    std::string error;
    assert(plugin.List(&sources, &error) == 0 && sources.size() == 2);
    const auto listing = Route(plugin, "/channels/dataframe");
    rapidjson::Document document;
    document.Parse(listing.c_str());
    for (const auto& item : document["channels"].GetArray()) {
        assert(item["rows"].IsNull() && item["schema"].IsNull());
        assert(std::string(item["load_state"].GetString()) == "unloaded");
    }
    Route(plugin, "/channels/dataframe/exploration/v1/sources");
    alignas(inotify_event) char events[8192];
    const auto bytes = read(watcher, events, sizeof(events));
    for (ssize_t offset = 0; offset < bytes;) {
        const auto* event = reinterpret_cast<const inotify_event*>(events + offset);
        // Opening the directory to enumerate names is allowed; opening a CSV is not.
        assert(event->len == 0 || std::string(event->name).find(".csv") == std::string::npos);
        offset += sizeof(inotify_event) + event->len;
    }
    DataFrame frame;
    assert(channel->Read(&frame) == 0 && frame.RowCount() == 2);
    assert(frame.GetSchema()[0].type == DataType::INT64);
    assert(frame.GetSchema()[1].type == DataType::DOUBLE);
    assert(frame.GetSchema()[2].type == DataType::STRING);
    assert(plugin.Get("broken"));
    auto broken = std::dynamic_pointer_cast<IDataFrameChannel>(plugin.Get("broken"));
    assert(broken->Read(&frame) != 0);
    const auto failed = Route(plugin, "/channels/dataframe");
    assert(failed.find("CSV_LOAD_FAILED") != std::string::npos);
    std::ofstream(data / "broken.csv") << "label\n";
    assert(broken->Read(&frame) == 0 && frame.RowCount() == 0 && frame.GetSchema().size() == 1);
    Concurrency(data / "numbers.csv");
    Management(root);
    plugin.Stop();
    assert(!channel->IsOpened() && channel->Open() != 0);
    assert(plugin.Start() == 0);
    auto cold = std::dynamic_pointer_cast<IAppendableDataFrameChannel>(plugin.Get("numbers"));
    assert(std::string(cold->Schema()) == "null");
    DataFrame incoming;
    incoming.SetSchema(
        {{"id", DataType::INT64, 0, ""}, {"value", DataType::DOUBLE, 0, ""}, {"label", DataType::STRING, 0, ""}});
    assert(incoming.AppendRow({int64_t(3), 4.5, std::string("three")}) == 0);
    assert(cold->Append(&incoming) == 0 && cold->Read(&frame) == 0 && frame.RowCount() == 3);
    assert(std::get<int64_t>(frame.GetRow(0)[0]) == 1 && std::get<std::string>(frame.GetRow(2)[2]) == "three");
    auto overwrite = std::dynamic_pointer_cast<IDataFrameChannel>(plugin.Get("broken"));
    assert(std::string(overwrite->Schema()) == "null");
    assert(fs::remove(data / "broken.csv"));
    assert(overwrite->Write(&incoming) == 0 && overwrite->Read(&frame) == 0 && frame.RowCount() == 1);
    plugin.Stop();
    close(watcher);
    // Temporary evidence is retained for diagnosis, never touches business CSVs.
    std::cout << "PASS startup and directory queries never open CSV; target Read preserves schema\n";
}
