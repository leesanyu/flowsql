// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arrow/util/byte_size.h>
#include <framework/interfaces/iblock_stream_channel.h>
#include <framework/interfaces/iblock_transform_dataframe_input.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/irouter_handle.h>
#include <sys/resource.h>
#include <tests/common/test_environment.h>
#include <unistd.h>
#include <common/loader.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include "baseliner_batch_fixture.h"

using namespace flowsql;
using namespace flowsql::baseliner;
using namespace flowsql::baseliner::batch_test;
namespace {
Batch Csv(size_t limit) {
    std::ifstream file(FLOWSQL_BASELINER_CSV);
    Check(file.good(), "repository CSV");
    std::string line;
    std::getline(file, line);
    arrow::StringBuilder labels, links;
    arrow::Int64Builder times;
    arrow::DoubleBuilder values;
    size_t rows = 0;
    while (rows < limit && std::getline(file, line)) {
        const auto comma = line.find(',');
        Check(comma != std::string::npos, "CSV fields");
        auto label = line.substr(0, comma);
        auto value = std::stod(line.substr(comma + 1));
        std::tm date{};
        std::istringstream text(label);
        text >> std::get_time(&date, "%Y/%m/%d %H:%M");
        Check(!text.fail(), "CSV time");
        const int64_t time = timegm(&date) - 8 * 3600;
        Check(time == 1624692180 + int64_t(rows) * 60, "CSV minute continuity");
        Check(labels.Append(label).ok() && values.Append(value).ok() && times.Append(time).ok() &&
                  links.Append("link_1").ok(),
              "CSV builders");
        ++rows;
    }
    Check(rows == limit, "CSV prefix size");
    return arrow::RecordBatch::Make(
        arrow::schema({arrow::field("time", arrow::utf8()), arrow::field("Mbps", arrow::float64()),
                       arrow::field("time_s", arrow::int64()), arrow::field("link", arrow::utf8())}),
        rows, {Unwrap(labels.Finish()), Unwrap(values.Finish()), Unwrap(times.Finish()), Unwrap(links.Finish())});
}
std::string Parameters() {
    return R"({"task_key":"link-data-2-month-mbps","clock":{"bucket_seconds":60,"timezone":"Asia/Shanghai"},"calendar":{"calendar_id":"cn-holiday","calendar_version":"2026.1"},"bootstrap":{"mode":"cold"},"forecast":{"horizon_buckets":0},"read_policy":{"page_rows":4096,"max_pending_bytes":268435456},"dataset":{"fields":{"time_s":"int64","link":"utf8","Mbps":"float64"},"series_keys":["link"],"deduplicate":{"keys":["time_s","link"],"on_duplicate":"require_equal"},"bucket":{"column":"time_s","unit":"s"},"metrics":[{"id":"mbps","kind":"value","column":"Mbps","aggregate":"mean","feature_type":"value_basic","profile":"default"}]}})";
}
struct Environment {
    PluginLoader* loader = PluginLoader::Single();
    explicit Environment(const std::string& library, const std::string& root) {
        const char* libs[] = {"libflowsql_builtin.so", "libflowsql_catalog.so", "libflowsql_scheduler.so",
                              "libflowsql_baseline.so", library.c_str()};
        const auto catalog = "data_dir=" + root + ";operator_db_path=" + root + "/operators.db";
        const char* options[] = {nullptr, catalog.c_str(), "batch_workers=1;stream_workers=1", nullptr, nullptr};
        Check(loader->Load(get_absolute_process_path(), libs, options, 5) == 0, "load isolated Scheduler plugins");
        Check(loader->StartAll() == 0, "start isolated Scheduler plugins");
    }
    ~Environment() {
        loader->StopAll();
        loader->Unload();
    }
};
std::string Execute(const std::string& sql) {
    fnRouterHandler handler;
    PluginLoader::Single()->Traverse(IID_ROUTER_HANDLE, [&](void* value) {
        static_cast<IRouterHandle*>(value)->EnumRoutes([&](const RouteItem& item) {
            if (item.uri == "/scheduler/batch/execute" && item.method == "POST") handler = item.handler;
        });
        return handler ? -1 : 0;
    });
    Check(bool(handler), "Scheduler batch route");
    rapidjson::Document request(rapidjson::kObjectType);
    request.AddMember("sql", rapidjson::Value(sql.c_str(), request.GetAllocator()), request.GetAllocator());
    std::string response;
    const int rc = handler("/scheduler/batch/execute", Json(request), response);
    Check(rc == 0, response);
    rapidjson::Document parsed;
    parsed.Parse(response.c_str());
    Check(parsed.IsObject() && parsed.HasMember("status") && std::string(parsed["status"].GetString()) == "completed",
          response);
    return response;
}
void Sql(size_t rows, const std::string& library, const std::string& output, bool compare) {
    const auto root = test::WorkspacePath("baseliner-output-sql-") +
                      std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" +
                      std::to_string(getpid());
    std::filesystem::create_directories(root);
    Environment environment(library, root);
    auto* registry = static_cast<IChannelRegistry*>(environment.loader->First(IID_CHANNEL_REGISTRY));
    Check(registry != nullptr, "registry");
    auto source = std::make_shared<DataFrameChannel>("dataframe", "link_data_2_month_baseliner");
    DataFrame frame;
    frame.FromArrow(Csv(rows));
    const auto original = frame.ToArrow();
    Check(source->Open() == 0 && source->Write(&frame) == 0 && registry->Register(source->Name(), source) == 0,
          "register private input");
    const std::string sql =
        "SELECT * FROM dataframe.link_data_2_month_baseliner USING explore.baseliner WITH parameters='" + Parameters() +
        "',model_output='dataframe.link_mbps_final_models' INTO dataframe.link_mbps_baseline";
    rusage before{}, measured{};
    getrusage(RUSAGE_SELF, &before);
    const auto start = Clock::now();
    const auto response = Execute(sql);
    const double elapsed = Seconds(start);
    getrusage(RUSAGE_SELF, &measured);
    auto read = [&](const char* name) {
        auto channel = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get(name));
        Check(bool(channel), std::string("published output: ") + name);
        DataFrame result;
        Check(channel->Read(&result) == 0, "read published output");
        return result.ToArrow();
    };
    const auto result = read("link_mbps_baseline"), model = read("link_mbps_final_models");
    Check(result->num_rows() == int64_t(rows) && result->num_columns() == 30 && model->num_rows() == 1,
          "SQL cardinality");
    Check(source->Read(&frame) == 0, "source preserved");
    Equal(frame.ToArrow(), original);
    if (!output.empty()) {
        std::filesystem::create_directories(output);
        if (compare) {
            Equal(result, Read(output + "/results.arrow"));
            Equal(model, Read(output + "/models.arrow"));
        } else {
            Write(output + "/results.arrow", result);
            Write(output + "/models.arrow", model);
        }
        std::ofstream(output + "/last-response.json") << response;
    }

    rusage compared{};
    getrusage(RUSAGE_SELF, &compared);
    std::cout << std::setprecision(12) << "COST {\"mode\":\"sql\",\"rows\":" << rows << ",\"total_s\":" << elapsed
              << ",\"result_bytes\":" << arrow::util::TotalBufferSize(*result)
              << ",\"model_rows\":" << model->num_rows() << ",\"max_rss_kib\":" << measured.ru_maxrss << ",\"user_s\":"
              << (measured.ru_utime.tv_sec - before.ru_utime.tv_sec) +
                     (measured.ru_utime.tv_usec - before.ru_utime.tv_usec) * 1e-6
              << ",\"sys_s\":"
              << (measured.ru_stime.tv_sec - before.ru_stime.tv_sec) +
                     (measured.ru_stime.tv_usec - before.ru_stime.tv_usec) * 1e-6
              << ",\"equal\":true}" << std::endl;
}
// Production reader and aggregation feed the real operator; only segment clocks are added here.
void Production(size_t rows, const char* library, const std::string& reference) {
    auto owner = std::make_shared<Library>(library);
    auto* normalizer = dynamic_cast<IBlockTransformSourceConfigProviderV1*>(owner->provider);
    Check(normalizer != nullptr, "source normalizer");
    std::string normalized, error;
    const auto with = With(Parameters(), false);
    rapidjson::Document config;
    config.Parse(with.c_str());
    config.AddMember("model_output", "dataframe.link_mbps_final_models", config.GetAllocator());
    const auto raw = Json(config);
    Check(normalizer->NormalizeSourceConfig(raw.c_str(), "dataframe.link_data_2_month_baseliner", &normalized,
                                            &error) == 0,
          error);
    BlockTransformTaskConfigV2 task_config{kBlockTransformTaskConfigV2Size, kBlockTransformContractVersionV2,
                                           "production-cost", normalized.c_str(), ""};
    IBlockTransformTaskV2* task_pointer = nullptr;
    Check(owner->provider->CreateTask(task_config, &task_pointer) == 0 && task_pointer, "production task");
    TaskOwner task(task_pointer, [owner](auto* value) { owner->provider->ReleaseTask(value); });
    auto* input_task = dynamic_cast<IBlockTransformDataFrameInputTaskV1*>(task.get());
    auto* model_task = dynamic_cast<IBlockTransformModelOutputTaskV1*>(task.get());
    auto* result_task = dynamic_cast<IBlockTransformResultOutputTaskV1*>(task.get());
    auto* source_task = dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get());
    Check(input_task && model_task && result_task && source_task, "production capabilities");
    Check(source_task->BindInputSource("dataframe.link_data_2_month_baseliner") == 0, task->LastError());
    BlockResultOutputBindingV1 result_binding;
    result_binding.target = "dataframe.link_mbps_baseline";
    Check(result_task->BindResultOutput(result_binding) == 0, task->LastError());
    auto models = std::make_shared<DataFrameChannel>("dataframe", "link_mbps_final_models");
    Check(models->Open() == 0, "model channel");
    BlockModelOutputBindingV1 model_binding;
    model_binding.primary_target = "dataframe.link_mbps_baseline";
    model_binding.dataframe = models;
    Check(model_task->BindModelOutput(model_binding) == 0, task->LastError());
    auto source = std::make_shared<DataFrameChannel>("dataframe", "link_data_2_month_baseliner");
    DataFrame data;
    data.FromArrow(Csv(rows));
    Check(source->Open() == 0 && source->Write(&data) == 0, "source channel");
    DataFrameChannel sink("dataframe", "link_mbps_baseline");
    Check(sink.Open() == 0, "result channel");
    BlockDataFrameInputBindingV1 input_binding;
    input_binding.source = source;
    input_binding.exact_source = "dataframe.link_data_2_month_baseliner";
    BlockDataFrameInputV1 input;
    rusage before{}, measured{}, compared{};
    getrusage(RUSAGE_SELF, &before);
    const auto begin = Clock::now();
    Check(input_task->CreateDataFrameInput(input_binding, &input) == 0, task->LastError());
    std::shared_ptr<arrow::Schema> output_schema;
    Check(task->Open(input.schema, &output_schema) == 0, task->LastError());
    uint64_t batches = 0, result_rows = 0, bytes = 0, max_rows = 0;
    double append_seconds = 0;
    auto drain = [&](const std::vector<BlockTransformOutputV1>& outputs) {
        for (const auto& out : outputs) {
            if (!out.batch->num_rows()) continue;
            Check(out.batch->GetColumnByName("result_kind") != nullptr, "primary result Schema");
            ++batches;
            result_rows += out.batch->num_rows();
            max_rows = std::max(max_rows, uint64_t(out.batch->num_rows()));
            bytes += arrow::util::TotalBufferSize(*out.batch);
            DataFrame frame;
            frame.FromArrow(out.batch);
            const auto append = Clock::now();
            Check(sink.Append(&frame) == 0, "production append");
            append_seconds += Seconds(append);
        }
    };
    for (;;) {
        auto event = input.input->PollBlock();
        if (event.kind == BlockPollEvent::kEof) break;
        Check(event.kind == BlockPollEvent::kData, "production input event");
        std::vector<BlockTransformOutputV1> outputs;
        Check(task->ProcessBlock(event.batch, 0, &outputs) == 0, task->LastError());
        drain(outputs);
        Check(input.input->ReleaseBlock(event.batch) == 0, "release production input");
    }
    std::vector<BlockTransformOutputV1> tail;
    Check(task->Flush(&tail) == 0, task->LastError());
    drain(tail);
    const double total_seconds = Seconds(begin);
    getrusage(RUSAGE_SELF, &measured);
    input_task->ReleaseDataFrameInput(input.input);
    input.input = nullptr;
    input.schema.reset();
    DataFrame result, model;
    Check(sink.Read(&result) == 0 && models->Read(&model) == 0, "production outputs");
    Check(result_rows == rows && model.RowCount() == 1, "production cardinality");
    uint64_t reference_bytes = 0;
    if (!reference.empty()) {
        for (auto item : {std::make_pair("results", result.ToArrow()), std::make_pair("models", model.ToArrow())}) {
            auto expected = Read(reference + "/" + item.first + ".arrow");
            reference_bytes += arrow::util::TotalBufferSize(*expected);
            Equal(item.second, expected);
        }
    }
    getrusage(RUSAGE_SELF, &compared);
    std::cout << std::setprecision(12) << "COST {\"mode\":\"production\",\"rows\":" << result_rows
              << ",\"batches\":" << batches << ",\"max_batch_rows\":" << max_rows << ",\"result_bytes\":" << bytes
              << ",\"total_s\":" << total_seconds << ",\"append_s\":" << append_seconds
              << ",\"max_rss_kib_before_compare\":" << measured.ru_maxrss
              << ",\"max_rss_kib_after_compare\":" << compared.ru_maxrss << ",\"reference_bytes\":" << reference_bytes
              << ",\"user_s\":"
              << (measured.ru_utime.tv_sec - before.ru_utime.tv_sec) +
                     (measured.ru_utime.tv_usec - before.ru_utime.tv_usec) * 1e-6
              << ",\"sys_s\":"
              << (measured.ru_stime.tv_sec - before.ru_stime.tv_sec) +
                     (measured.ru_stime.tv_usec - before.ru_stime.tv_usec) * 1e-6
              << ",\"equal\":true}" << std::endl;
}
void Representative(const std::string& workload, const char* path, const std::string& output, bool compare) {
    const size_t buckets = workload == "small" ? 32 : 4096;
    const size_t identities = workload == "value" || workload == "ratio" || workload == "relation" ? 4 : 1;
    auto config = Config(workload == "forecast" ? 2 : 0, 268435456,
                         workload == "ratio" || workload == "relation" ? workload : "value");
    if (workload == "value") {
        rapidjson::Document doc;
        doc.Parse(config.original_json.c_str());
        auto& dataset = doc["datasets"][0];
        dataset["fields"].AddMember("v2", "float64", doc.GetAllocator());
        rapidjson::Value second(dataset["metrics"][0], doc.GetAllocator());
        second["id"].SetString("v2", doc.GetAllocator());
        second["column"].SetString("v2", doc.GetAllocator());
        dataset["metrics"].PushBack(second, doc.GetAllocator());
        Check(ParseConfig(Json(doc), &config).ok(), "two-metric config");
    }
    auto owner = std::make_shared<Library>(path);
    auto task = Task(owner, config);
    auto model_sink = std::make_shared<DataFrameChannel>("dataframe", "models");
    Check(model_sink->Open() == 0, "model sink");
    BlockModelOutputBindingV1 binding;
    binding.primary_target = "dataframe.representative_results";
    binding.dataframe = model_sink;
    Check(dynamic_cast<IBlockTransformModelOutputTaskV1*>(task.get())->BindModelOutput(binding) == 0,
          task->LastError());
    Check(dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get())->BindInputSource("dataframe.samples") == 0,
          task->LastError());
    std::shared_ptr<arrow::Schema> schema;
    Check(task->Open(MakeSchema(SchemaKind::kObservation), &schema) == 0, task->LastError());
    std::vector<std::vector<Observation>> pages(1);
    for (size_t bucket = 0; bucket < buckets; ++bucket)
        for (size_t series = 1; series <= identities; ++series) {
            auto row = Input(config, bucket, series);
            if (pages.back().size() == 4096) pages.emplace_back();
            pages.back().push_back(row);
            if (workload == "value") {
                row.metric_id = "v2";
                row.value = *row.value * 2;
                Check(EncodeIdentity(config.config.source, row.dataset_id, row.metric_id, row.kind, {uint64_t(series)},
                                     &row.identity)
                          .ok(),
                      "second metric identity");
                if (pages.back().size() == 4096) pages.emplace_back();
                pages.back().push_back(std::move(row));
            }
        }
    std::vector<Batch> wires;
    for (const auto& page : pages) wires.push_back(Wire(page));
    DataFrameChannel results("dataframe", "results"), fusion("dataframe", "fusion");
    Check(results.Open() == 0 && fusion.Open() == 0, "private sinks");
    size_t count = 0;
    double append_seconds = 0;
    auto drain = [&](const std::vector<BlockTransformOutputV1>& outputs) {
        for (const auto& out : outputs) {
            if (!out.batch->num_rows()) continue;
            DataFrame frame;
            frame.FromArrow(out.batch);
            const auto append = Clock::now();
            auto& sink = out.batch->GetColumnByName("result_kind") ? results : fusion;
            Check(sink.Append(&frame) == 0, "representative append");
            append_seconds += Seconds(append);
            ++count;
        }
    };
    rusage before{}, measured{}, compared{};
    getrusage(RUSAGE_SELF, &before);
    const auto begin = Clock::now();
    for (const auto& wire : wires) {
        std::vector<BlockTransformOutputV1> outputs;
        Check(task->ProcessBlock(wire, 0, &outputs) == 0, task->LastError());
        drain(outputs);
    }
    std::vector<BlockTransformOutputV1> tail;
    Check(task->Flush(&tail) == 0, task->LastError());
    drain(tail);
    const double total_seconds = Seconds(begin);
    getrusage(RUSAGE_SELF, &measured);
    DataFrame result, model, fused;
    Check(results.Read(&result) == 0 && model_sink->Read(&model) == 0 && fusion.Read(&fused) == 0, "read outputs");
    Check(model.RowCount() == int64_t(identities * (workload == "value" ? 2 : 1)), "model identities");
    uint64_t reference_bytes = 0;
    if (!output.empty()) {
        std::filesystem::create_directories(output);
        for (auto item : {std::make_pair("results", result.ToArrow()), std::make_pair("models", model.ToArrow()),
                          std::make_pair("fusion", fused.ToArrow())}) {
            if (!item.second) continue;
            const auto file = output + "/" + item.first + ".arrow";
            if (compare) {
                auto reference = Read(file);
                reference_bytes += arrow::util::TotalBufferSize(*reference);
                Equal(item.second, reference);
            } else
                Write(file, item.second);
        }
    }
    getrusage(RUSAGE_SELF, &compared);
    std::cout << std::setprecision(12) << "COST {\"mode\":\"" << workload << "\",\"total_s\":" << total_seconds
              << ",\"append_s\":" << append_seconds << ",\"batches\":" << count << ",\"rows\":" << result.RowCount()
              << ",\"result_bytes\":" << arrow::util::TotalBufferSize(*result.ToArrow())
              << ",\"reference_bytes\":" << reference_bytes << ",\"max_rss_kib_before_compare\":" << measured.ru_maxrss
              << ",\"max_rss_kib_after_compare\":" << compared.ru_maxrss << ",\"user_s\":"
              << (measured.ru_utime.tv_sec - before.ru_utime.tv_sec) +
                     (measured.ru_utime.tv_usec - before.ru_utime.tv_usec) * 1e-6
              << ",\"sys_s\":"
              << (measured.ru_stime.tv_sec - before.ru_stime.tv_sec) +
                     (measured.ru_stime.tv_usec - before.ru_stime.tv_usec) * 1e-6
              << ",\"equal\":true}" << std::endl;
}
}  // namespace
int main(int argc, char** argv) {
    try {
        Check(
            argc >= 3,
            "usage: --sql rows | --representative small|value|ratio|relation|forecast [library [reference [compare]]]");
        const auto path = argc > 3 ? argv[3] : "./libflowsql_baseliner.so";
        const std::string output = argc > 4 ? argv[4] : "";
        const bool compare = argc > 5 && std::string(argv[5]) == "compare";
        if (std::string(argv[1]) == "--sql")
            Sql(std::stoull(argv[2]), path, output, compare);
        else if (std::string(argv[1]) == "--production")
            Production(std::stoull(argv[2]), path, output);
        else {
            Check(std::string(argv[1]) == "--representative", "invalid mode");
            Representative(argv[2], path, output, compare);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
