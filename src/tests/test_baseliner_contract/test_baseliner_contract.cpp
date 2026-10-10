// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/baseliner/baseliner_contract.h>

#include <arrow/api.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cassert>
#include <iostream>
#include <limits>

using namespace flowsql;
using namespace flowsql::baseliner;
namespace {
const char* kConfig = R"({
  "schema_version":1,"task_key":"link-byte-baseline","source":"mysql.npm","mode":"snapshot",
  "datasets":[{"id":"link","table":"npm_basic_history_v1",
    "fields":{"__npm_run_id":"utf8","session_id":"uint64","revision":"uint64",
      "observation_domain_id":"uint64","period_start_ns":"int64","period_complete":"boolean",
      "interval_wire_bytes_ab":"uint64"},
    "scope":{"run_ids":["completed-run-id"]},"series_keys":["observation_domain_id"],
    "row_semantics":"npm_period_increment",
    "deduplicate":{"keys":["__npm_run_id","session_id","revision"],"on_duplicate":"require_equal"},
    "bucket":{"column":"period_start_ns","unit":"ns"},
    "filter":[{"column":"period_complete","op":"eq","value":true}],
    "metrics":[{"id":"bytes_ab","kind":"value","column":"interval_wire_bytes_ab","aggregate":"sum",
      "null_policy":"skip","feature_type":"value_basic","profile":"default"}]}],
  "clock":{"bucket_seconds":60,"timezone":"Asia/Shanghai"},
  "calendar":{"calendar_id":"cn-holiday","calendar_version":"2026.1"},
  "bootstrap":{"mode":"cold"},"forecast":{"horizon_buckets":0},
  "read_policy":{"page_rows":4096,"max_pending_bytes":16777216},
  "state_policy":{"max_runtime_identities":1024,"max_model_identities":1024,"max_basis_versions":2,
    "capacity":"reject","release_scope":"RuntimeOnly"},
  "persistence":{"checkpoint_every_buckets":1,"max_checkpoint_bytes":16777216,
    "retain_generations":2,"restore":"if_exists"}})";

std::string Serialize(const rapidjson::Value& value) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    value.Accept(writer);
    return {buffer.GetString(), buffer.GetSize()};
}
template <typename F>
std::string Changed(F change) {
    rapidjson::Document doc;
    doc.Parse(kConfig);
    change(doc);
    return Serialize(doc);
}
void Good(const std::string& json) {
    ConfigSnapshot snapshot;
    const auto status = ParseConfig(json, &snapshot);
    if (!status.ok()) std::cerr << status.path << ": " << status.message << '\n';
    assert(status.ok());
}
void Bad(const std::string& json, ConfigError expected) {
    ConfigSnapshot snapshot;
    snapshot.original_json = "keep";
    snapshot.config.task_key = "unchanged";
    const auto status = ParseConfig(json, &snapshot);
    if (status.code != expected) std::cerr << status.path << ": " << status.message << '\n';
    assert(status.code == expected);
    assert(snapshot.original_json == "keep" && snapshot.config.task_key == "unchanged");
}
void TestStrictConfig() {
    ConfigSnapshot snapshot;
    assert(ParseConfig(kConfig, &snapshot).ok());
    assert(snapshot.original_json == kConfig && snapshot.sha256_hex.size() == 64);
    assert(snapshot.revision == 0 && snapshot.exact_reference.empty());
    assert(snapshot.config.datasets[0].fields.at("session_id") == LogicalType::kUInt64);
    assert(snapshot.config.datasets[0].scope.consistency == "npm_completed");
    assert(snapshot.config.persistence.lease_ms == 30000 && snapshot.config.state.idle_timeout_ms == 0);
    assert(!snapshot.config.datasets[0].metrics[0].algorithm_config_json.empty());
    Bad("{", ConfigError::kJson);
    Bad("{\"schema_version\":1,\"schema_version\":1}", ConfigError::kDuplicate);
    Bad(Changed([](auto& d) { d.AddMember("future", 1, d.GetAllocator()); }), ConfigError::kUnknown);
    Bad(Changed([](auto& d) { d["datasets"][0]["bucket"].AddMember("future", 1, d.GetAllocator()); }),
        ConfigError::kUnknown);
    Bad(Changed([](auto& d) { d["clock"].AddMember("timezone", "UTC", d.GetAllocator()); }), ConfigError::kDuplicate);
    Bad(Changed([](auto& d) { d["schema_version"].SetInt(2); }), ConfigError::kValue);
    Bad(Changed([](auto& d) { d["schema_version"].SetDouble(1.0); }), ConfigError::kType);
    Bad(Changed([](auto& d) { d.RemoveMember("task_key"); }), ConfigError::kMissing);
    Bad(Changed([](auto& d) { d["datasets"][0]["bucket"]["unit"].SetString("minutes"); }), ConfigError::kValue);
    Bad(Changed([](auto& d) { d["datasets"][0]["series_keys"][0].SetString("missing"); }), ConfigError::kValue);
    Bad(Changed([](auto& d) { d["datasets"][0]["fields"]["period_start_ns"].SetString("utf8"); }), ConfigError::kValue);
    Bad(Changed([](auto& d) { d["datasets"][0]["filter"][0]["value"].SetInt(1); }), ConfigError::kType);
    Bad(Changed([](auto& d) { d["clock"]["timezone"].SetString("Mars/Olympus"); }), ConfigError::kValue);
    Bad(Changed([](auto& d) { d["calendar"]["calendar_version"].SetString("latest"); }), ConfigError::kReference);
    Bad(Changed([](auto& d) { d["forecast"]["horizon_buckets"].SetUint(kMaxForecastBuckets + 1); }),
        ConfigError::kLimit);
    Bad(Changed([](auto& d) { d["read_policy"]["page_rows"].SetUint(0); }), ConfigError::kLimit);
    Bad(Changed([](auto& d) { d["state_policy"]["max_model_identities"].SetUint64(kMaxIdentities + 1); }),
        ConfigError::kLimit);
    Bad(Changed([](auto& d) { d["persistence"].AddMember("renew_ms", 30000, d.GetAllocator()); }), ConfigError::kValue);
    Bad(Changed([](auto& d) { d["state_policy"].AddMember("idle_timeout_ms", 1000, d.GetAllocator()); }),
        ConfigError::kValue);
    Bad(Changed([](auto& d) { d["datasets"][0]["metrics"][0].AddMember("divisor", 0, d.GetAllocator()); }),
        ConfigError::kValue);
    Bad(Changed([](auto& d) { d["datasets"][0]["metrics"][0]["feature_type"].SetString("value_sampled"); }),
        ConfigError::kValue);
    Bad(Changed([](auto& d) { d["datasets"][0]["deduplicate"]["keys"].PopBack(); }), ConfigError::kValue);
    Bad(std::string(kMaxConfigBytes + 1, ' '), ConfigError::kLimit);
    Bad(std::string(40, '[') + "0" + std::string(40, ']'), ConfigError::kLimit);
    std::string nul(kConfig);
    nul.push_back('\0');
    nul += "{}";
    Bad(nul, ConfigError::kJson);
    std::string utf8(kConfig);
    utf8.replace(utf8.find("link-byte-baseline"), 1, 1, '\xff');
    Bad(utf8, ConfigError::kJson);
    Good(Changed([](auto& d) { d["forecast"]["horizon_buckets"].SetUint(kMaxForecastBuckets); }));
    Good(Changed([](auto& d) {
        auto& m = d["datasets"][0]["metrics"][0];
        m["kind"].SetString("ratio");
        m["feature_type"].SetString("ratio");
        m["profile"].SetString("rate_core");
        m.RemoveMember("column");
        m.RemoveMember("aggregate");
        rapidjson::Value e(rapidjson::kObjectType);
        e.AddMember("column", "interval_wire_bytes_ab", d.GetAllocator());
        e.AddMember("aggregate", "sum", d.GetAllocator());
        rapidjson::Value numerator(e, d.GetAllocator());
        m.AddMember("numerator", numerator, d.GetAllocator());
        m.AddMember("denominator", e, d.GetAllocator());
    }));
}

class Registry final : public IConfigChannelRegistryV1 {
 public:
    ConfigChannelSnapshot snapshot;
    int calls = 0;
    int Resolve(const char* reference, ConfigChannelSnapshot* out, std::string*) override {
        ++calls;
        assert(std::string(reference) == "config.link_baseline@1");
        *out = snapshot;
        return 0;
    }
};
void TestResolveAndOwnership() {
    ConfigSnapshot parsed;
    assert(ParseConfig(kConfig, &parsed).ok());
    Registry registry;
    auto mutable_content = std::make_shared<std::string>(kConfig);
    registry.snapshot = {"link_baseline",         1, "json",         "baseliner.task.v1", parsed.sha256_hex,
                         mutable_content->size(), 0, mutable_content};
    ConfigSnapshot out;
    assert(ResolveConfig(R"({"config":"config.link_baseline@1"})", "mysql.npm", &registry, &out).ok());
    mutable_content->assign("replaced");
    assert(out.original_json == kConfig && out.revision == 1 && out.exact_reference == "config.link_baseline@1");
    const auto before = out.sha256_hex;
    registry.calls = 0;
    assert(!ResolveConfig(R"({"config":"config.link_baseline@latest"})", "mysql.npm", &registry, &out).ok());
    assert(!ResolveConfig(R"({"config":"config.link_baseline@01"})", "mysql.npm", &registry, &out).ok());
    assert(
        !ResolveConfig(R"({"config":"config.link_baseline@1","parameters":"{}"})", "mysql.npm", &registry, &out).ok());
    assert(registry.calls == 0 && out.sha256_hex == before);
    registry.snapshot.content = std::make_shared<const std::string>(kConfig);
    registry.snapshot.sha256_hex = std::string(64, '0');
    assert(!ResolveConfig(R"({"config":"config.link_baseline@1"})", "mysql.npm", &registry, &out).ok());
    registry.snapshot.sha256_hex = before;
    registry.snapshot.revision = 2;
    assert(!ResolveConfig(R"({"config":"config.link_baseline@1"})", "mysql.npm", &registry, &out).ok());
    rapidjson::Document with(rapidjson::kObjectType);
    with.AddMember("parameters", rapidjson::Value(kConfig, with.GetAllocator()), with.GetAllocator());
    assert(ResolveConfig(Serialize(with), "mysql.npm", nullptr, &out).ok());
    assert(out.revision == 0);
    assert(!ResolveConfig(Serialize(with), "mysql.other", nullptr, &out).ok());
    const auto original = out.original_json;
    const auto hash = out.sha256_hex;
    with.AddMember("model_output", "dataframe.models", with.GetAllocator());
    assert(ResolveConfig(Serialize(with), "mysql.npm", nullptr, &out).ok());
    assert(out.model_output == "dataframe.models" && out.original_json == original && out.sha256_hex == hash);
    for (const char* target : {"mysql.analysis.models", "postgres.analysis.models", "sqlite.analysis.models"}) {
        with["model_output"].SetString(target, with.GetAllocator());
        assert(ResolveConfig(Serialize(with), "mysql.npm", nullptr, &out).ok());
        assert(out.sha256_hex == hash);
    }
    for (const char* target :
         {"", "mysql.analysis", "clickhouse.analysis.models", "dataframe.a.b", "sqlite.a.x;DROP"}) {
        with["model_output"].SetString(target, with.GetAllocator());
        assert(!ResolveConfig(Serialize(with), "mysql.npm", nullptr, &out).ok());
    }
    with["model_output"].SetString("dataframe.models");
    auto poll = Changed([](auto& d) { d["mode"].SetString("poll"); });
    with["parameters"].SetString(poll.c_str(), with.GetAllocator());
    assert(!ResolveConfig(Serialize(with), "mysql.npm", nullptr, &out).ok());
    with.RemoveMember("model_output");
    assert(ResolveConfig(Serialize(with), "mysql.npm", nullptr, &out).ok());
    with["parameters"].SetString(kConfig, with.GetAllocator());
    with.AddMember("model_output", "dataframe.models", with.GetAllocator());
    registry.snapshot.revision = 1;
    assert(ResolveConfig(R"({"config":"config.link_baseline@1","model_output":"dataframe.models"})", "mysql.npm",
                         &registry, &out)
               .ok());
    assert(out.exact_reference == "config.link_baseline@1" && out.revision == 1 && out.sha256_hex == hash);
    with["model_output"].SetInt(3);
    assert(!ResolveConfig(Serialize(with), "mysql.npm", nullptr, &out).ok());
    assert(!ResolveConfig(
                R"({"config":"config.link_baseline@1","model_output":"dataframe.a","model_output":"dataframe.b"})",
                "mysql.npm", &registry, &out)
                .ok());
}

std::string Parameters(std::string_view json) {
    rapidjson::Document with(rapidjson::kObjectType);
    with.AddMember("parameters", rapidjson::Value(json.data(), json.size(), with.GetAllocator()), with.GetAllocator());
    return Serialize(with);
}
const char* kSingle = R"({
  "task_key":"samples-baseline",
  "clock":{"bucket_seconds":60,"timezone":"Asia/Shanghai"},
  "calendar":{"calendar_id":"cn-holiday","calendar_version":"2026.1"},
  "dataset":{
    "fields":{"bucket":"int64","link":"utf8","bytes":"float64"},
    "series_keys":["link"],"deduplicate":{"keys":["bucket","link"],"on_duplicate":"require_equal"},
    "bucket":{"column":"bucket","unit":"bucket_id"},
    "metrics":[{"id":"bytes","kind":"value","column":"bytes","aggregate":"sum",
                "feature_type":"value_basic","profile":"default"}]}})";
void TestSingleSourceContract() {
    for (const char* category : {"sqlite", "mysql", "postgres", "clickhouse"}) {
        const auto source = std::string(category) + ".npm.npm_basic_history_v1";
        const auto full = Changed([&](auto& d) { d["source"].SetString(source.c_str(), d.GetAllocator()); });
        ConfigSnapshot config;
        auto status = ParseConfig(full, &config);
        if (!status.ok()) std::cerr << status.path << ": " << status.message << '\n';
        assert(status.ok());
        assert(config.config.database_source && !config.config.dataframe_source);
        assert(config.config.source_relation == "npm_basic_history_v1");
        assert(config.original_json == full);
        rapidjson::Document doc;
        doc.Parse(full.c_str());
        doc["datasets"][0].RemoveMember("table");
        assert(ParseConfig(Serialize(doc), &config).ok());
        assert(config.config.datasets[0].table == "npm_basic_history_v1");
        doc["datasets"][0].AddMember("table", "another_table", doc.GetAllocator());
        Bad(Serialize(doc), ConfigError::kValue);
        doc["datasets"][0]["table"].SetString("npm_basic_history_v1");
        rapidjson::Value other(doc["datasets"][0], doc.GetAllocator());
        other["id"].SetString("other");
        doc["datasets"].PushBack(other, doc.GetAllocator());
        Bad(Serialize(doc), ConfigError::kValue);
    }
    for (const char* source : {"mysql.npm.table.extra", "dataframe.samples.table", "pcapfile.input.table",
                               "mysql..table", "mysql.npm.", "mysql.npm.bad/table"})
        Bad(Changed([&](auto& d) { d["source"].SetString(source, d.GetAllocator()); }), ConfigError::kValue);

    ConfigSnapshot frame;
    assert(ResolveConfig(Parameters(kSingle), "dataframe.samples", nullptr, &frame).ok());
    assert(frame.config.dataframe_source && !frame.config.database_source);
    assert(frame.config.source == "dataframe.samples" && frame.config.mode == Mode::kSnapshot);
    assert(frame.config.datasets.size() == 1 && frame.config.datasets[0].id == "source");
    assert(frame.config.datasets[0].table.empty() &&
           frame.config.datasets[0].scope.consistency == "dataframe_snapshot");
    assert(frame.revision == 0 && frame.exact_reference.empty());
    ConfigSnapshot canonical;
    assert(ParseConfig(frame.original_json, &canonical).ok());
    assert(canonical.sha256_hex == frame.sha256_hex);
    assert(canonical.config.datasets[0].metrics[0].algorithm_config_json ==
           frame.config.datasets[0].metrics[0].algorithm_config_json);
    rapidjson::Document original;
    original.Parse(kSingle);
    rapidjson::Document reordered(rapidjson::kObjectType);
    for (auto it = original.MemberEnd(); it != original.MemberBegin();) {
        --it;
        rapidjson::Value key(it->name, reordered.GetAllocator());
        rapidjson::Value value(it->value, reordered.GetAllocator());
        reordered.AddMember(key, value, reordered.GetAllocator());
    }
    assert(NormalizeInlineConfig(Serialize(reordered), "dataframe.samples", &canonical).ok());
    assert(canonical.original_json == frame.original_json && canonical.sha256_hex == frame.sha256_hex);
    rapidjson::Document additional;
    additional.Parse(R"([
      {"id":"rate","kind":"ratio","feature_type":"ratio","profile":"rate_core",
       "numerator":{"column":"bytes","aggregate":"sum"},"denominator":{"aggregate":"count"}},
      {"id":"mix","kind":"relation","feature_type":"relation","profile":"default",
       "group_space":{"id":"links","version":"1","column":"link",
                      "dictionary":[{"key":"a","group_idx":0},{"key":"b","group_idx":1}]},
       "metrics":[{"id":"bytes","column":"bytes","aggregate":"sum"}]}])");
    for (auto& metric : additional.GetArray()) {
        rapidjson::Value owned(metric, original.GetAllocator());
        original["dataset"]["metrics"].PushBack(owned, original.GetAllocator());
    }
    assert(NormalizeInlineConfig(Serialize(original), "dataframe.samples", &canonical).ok());
    assert(canonical.config.datasets[0].metrics.size() == 3);
    assert(canonical.config.datasets[0].metrics[1].kind == BaselineTaskKind::kRatio);
    assert(canonical.config.datasets[0].metrics[2].kind == BaselineTaskKind::kRelation);
    auto bad_inline = [&](const std::string& json, const char* source, ConfigError expected) {
        ConfigSnapshot keep = frame;
        auto status = ResolveConfig(Parameters(json), source, nullptr, &keep);
        if (status.code != expected) std::cerr << status.path << ": " << status.message << '\n';
        assert(status.code == expected && keep.original_json == frame.original_json);
    };
    rapidjson::Document short_form;
    short_form.Parse(kSingle);
    auto& allocator = short_form.GetAllocator();
    short_form["dataset"].AddMember("scope", rapidjson::kObjectType, allocator);
    auto& scope = short_form["dataset"]["scope"];
    scope.AddMember("begin_bucket", 0, allocator);
    scope.AddMember("end_bucket", 120, allocator);
    scope.AddMember("consistency", "consistent_snapshot", allocator);
    ConfigSnapshot database;
    assert(ResolveConfig(Parameters(Serialize(short_form)), "mysql.npm.samples", nullptr, &database).ok());
    assert(database.config.datasets[0].table == "samples" && database.config.source_relation == "samples");
    bad_inline(kSingle, "mysql.npm", ConfigError::kValue);
    bad_inline(kSingle, "pcapfile.samples", ConfigError::kValue);
    bad_inline(kSingle, "mysql.npm.samples.extra", ConfigError::kValue);
    bad_inline(Serialize(short_form), "dataframe.samples", ConfigError::kValue);
    short_form["dataset"].RemoveMember("scope");
    short_form.AddMember("datasets", rapidjson::kArrayType, allocator);
    bad_inline(Serialize(short_form), "dataframe.samples", ConfigError::kValue);
    short_form.RemoveMember("datasets");
    short_form.AddMember("source", "dataframe.other", allocator);
    bad_inline(Serialize(short_form), "dataframe.samples", ConfigError::kValue);
    short_form["source"].SetString("dataframe.samples");
    assert(ResolveConfig(Parameters(Serialize(short_form)), "dataframe.samples", nullptr, &canonical).ok());
    short_form.AddMember("mode", "poll", allocator);
    bad_inline(Serialize(short_form), "dataframe.samples", ConfigError::kValue);
    short_form.RemoveMember("mode");
    short_form["dataset"].AddMember("table", "samples", allocator);
    bad_inline(Serialize(short_form), "dataframe.samples", ConfigError::kValue);
    short_form["dataset"].RemoveMember("table");
    short_form["dataset"].AddMember("schema", "public", allocator);
    bad_inline(Serialize(short_form), "dataframe.samples", ConfigError::kValue);
    short_form["dataset"].RemoveMember("schema");
    short_form.AddMember("future", true, allocator);
    bad_inline(Serialize(short_form), "dataframe.samples", ConfigError::kUnknown);
    short_form.RemoveMember("future");
    short_form.AddMember("task_key", "duplicate", allocator);
    bad_inline(Serialize(short_form), "dataframe.samples", ConfigError::kDuplicate);

    rapidjson::Document full_frame;
    full_frame.Parse(frame.original_json.c_str());
    full_frame["mode"].SetString("poll");
    Bad(Serialize(full_frame), ConfigError::kValue);
    full_frame["mode"].SetString("snapshot");
    full_frame["datasets"][0]["scope"]["consistency"].SetString("immutable_range");
    Bad(Serialize(full_frame), ConfigError::kValue);
    full_frame["datasets"][0].RemoveMember("scope");
    assert(ParseConfig(Serialize(full_frame), &canonical).ok());
    rapidjson::Value scope_with_runs(rapidjson::kObjectType);
    rapidjson::Value runs(rapidjson::kArrayType);
    runs.PushBack("some-run", full_frame.GetAllocator());
    scope_with_runs.AddMember("run_ids", runs, full_frame.GetAllocator());
    full_frame["datasets"][0].AddMember("scope", scope_with_runs, full_frame.GetAllocator());
    Bad(Serialize(full_frame), ConfigError::kValue);
    full_frame["datasets"][0].RemoveMember("scope");
    rapidjson::Value second(full_frame["datasets"][0], full_frame.GetAllocator());
    second["id"].SetString("other");
    full_frame["datasets"].PushBack(second, full_frame.GetAllocator());
    Bad(Serialize(full_frame), ConfigError::kValue);

    ConfigSnapshot old;
    assert(ResolveConfig(Parameters(kConfig), "mysql.npm", nullptr, &old).ok());
    assert(old.original_json == kConfig && old.sha256_hex.size() == 64);
    assert(!old.config.dataframe_source && old.config.source_relation.empty());
}
void TestDeclaredMappingsAndBounds() {
    Good(Changed([](auto& d) {
        d["mode"].SetString("poll");
        d["state_policy"].AddMember("idle_timeout_ms", 5000, d.GetAllocator());
        d["state_policy"]["capacity"].SetString("evict_idle");
    }));
    const auto ordinary = Changed([](auto& d) {
        d["source"].SetString("sqlite.business");
        d["datasets"][0]["row_semantics"].SetString("immutable");
        auto& scope = d["datasets"][0]["scope"];
        scope.RemoveMember("run_ids");
        scope.AddMember("consistency", "immutable_range", d.GetAllocator());
        scope.AddMember("begin_bucket", -10, d.GetAllocator());
        scope.AddMember("end_bucket", 100, d.GetAllocator());
        d["datasets"][0]["bucket"]["unit"].SetString("bucket_id");
    });
    Good(ordinary);
    rapidjson::Document o;
    o.Parse(ordinary.c_str());
    o["datasets"][0]["scope"]["end_bucket"].SetInt(-10);
    Bad(Serialize(o), ConfigError::kValue);
    auto relation = Changed([](auto& d) {
        rapidjson::Document m;
        m.Parse(R"({"id":"mix","kind":"relation","feature_type":"relation","profile":"default",
           "group_space":{"id":"clients","version":"1","column":"observation_domain_id"},
           "metrics":[{"id":"bytes","column":"interval_wire_bytes_ab","aggregate":"sum"},
                      {"id":"packets","aggregate":"count"}]})");
        d["datasets"][0]["metrics"][0].CopyFrom(m, d.GetAllocator());
    });
    Good(relation);
    ConfigSnapshot snapshot;
    assert(ParseConfig(relation, &snapshot).ok());
    const auto& m = snapshot.config.datasets[0].metrics[0];
    assert(m.relation_metrics[0].id == "bytes" && m.relation_metrics[1].id == "packets");
    rapidjson::Document r;
    r.Parse(relation.c_str());
    r["state_policy"]["max_basis_versions"].SetInt(1);
    Bad(Serialize(r), ConfigError::kLimit);
    r["state_policy"]["max_basis_versions"].SetInt(2);
    auto& groups = r["datasets"][0]["metrics"][0]["group_space"];
    groups["version"].SetString("latest");
    Bad(Serialize(r), ConfigError::kReference);
    groups["version"].SetString("1");
    rapidjson::Document dict;
    dict.Parse(R"([{"key":18446744073709551615,"group_idx":7},{"key":3,"group_idx":8}])");
    rapidjson::Value dictionary(dict, r.GetAllocator());
    groups.AddMember("dictionary", dictionary, r.GetAllocator());
    Good(Serialize(r));
    groups["dictionary"][1]["group_idx"].SetInt(7);
    Bad(Serialize(r), ConfigError::kValue);
    groups["dictionary"][1]["group_idx"].SetInt(8);
    groups["dictionary"][0]["group_idx"].SetUint64(uint64_t(UINT32_MAX) + 1);
    Bad(Serialize(r), ConfigError::kLimit);
    Bad(Changed([](auto& d) {
            rapidjson::Value other(d["datasets"][0], d.GetAllocator());
            d["datasets"].PushBack(other, d.GetAllocator());
        }),
        ConfigError::kValue);
    Bad(Changed([](auto& d) {
            auto& metrics = d["datasets"][0]["metrics"];
            rapidjson::Value other(metrics[0], d.GetAllocator());
            metrics.PushBack(other, d.GetAllocator());
        }),
        ConfigError::kValue);
    Bad(Changed([](auto& d) { d["read_policy"]["max_pending_bytes"].SetUint64(kMaxBufferBytes + 1); }),
        ConfigError::kLimit);
    Bad(Changed([](auto& d) { d["persistence"]["max_checkpoint_bytes"].SetUint64(kMaxBufferBytes + 1); }),
        ConfigError::kLimit);
    Bad(Changed([](auto& d) { d["datasets"][0]["filter"][0]["value"].SetNull(); }), ConfigError::kType);
    Good(Changed([](auto& d) {
        auto& bootstrap = d["bootstrap"];
        bootstrap["mode"].SetString("history");
        bootstrap.AddMember("begin_bucket", -20, d.GetAllocator());
        bootstrap.AddMember("end_bucket", 0, d.GetAllocator());
        bootstrap.AddMember("min_observations", 10, d.GetAllocator());
        bootstrap.AddMember("insufficient", "cold", d.GetAllocator());
        auto& metric = d["datasets"][0]["metrics"][0];
        metric["feature_type"].SetString("value_sampled");
        metric["profile"].SetString("cont_core");
        rapidjson::Value sample(rapidjson::kObjectType);
        sample.AddMember("aggregate", "count", d.GetAllocator());
        metric.AddMember("sample_count", sample, d.GetAllocator());
    }));
}

void TestIdentitySchemaAndProgress() {
    std::string a, b;
    assert(EncodeIdentity("mysql.npm", "a|b", "c", BaselineTaskKind::kValue, {uint64_t(-1)}, &a).ok());
    assert(EncodeIdentity("mysql.npm", "a", "b|c", BaselineTaskKind::kValue, {uint64_t(-1)}, &b).ok());
    assert(a != b);
    assert(EncodeIdentity("mysql.npm", "a|b", "c", BaselineTaskKind::kValue, {int64_t(-1)}, &b).ok());
    assert(a != b);
    assert(EncodeIdentity("s", "d", "m", BaselineTaskKind::kValue, {0.0}, &a).ok());
    assert(EncodeIdentity("s", "d", "m", BaselineTaskKind::kValue, {-0.0}, &b).ok() && a == b);
    assert(
        !EncodeIdentity("s", "d", "m", BaselineTaskKind::kValue, {std::numeric_limits<double>::infinity()}, &b).ok() &&
        a == b);
    const auto input = MakeSchema(SchemaKind::kObservation);
    assert(input->GetFieldByName("identity")->type()->Equals(arrow::binary()));
    assert(input->GetFieldByName("sample_count")->type()->Equals(arrow::uint64()));
    assert(input->GetFieldByName("bucket_id")->type()->Equals(arrow::int64()));
    assert(input->GetFieldByName("group_idx")->type()->id() == arrow::Type::LIST);
    assert(input->GetFieldByName("metrics")->type()->id() == arrow::Type::LIST);
    assert(!input->GetFieldByName("identity")->nullable());
    assert(input->metadata()->Get("flowsql.baseliner.version").ValueOrDie() == "1");
    const auto output = MakeSchema(SchemaKind::kResults);
    assert(output->GetFieldByName("expected")->nullable() && output->GetFieldByName("observed")->nullable());
    for (const bool routed : {false, true})
        for (const bool forecast : {false, true}) {
            const auto keys = ResultKeyFields(forecast, routed);
            for (const auto& key : keys) assert(output->GetFieldByName(key));
            assert((std::find(keys.begin(), keys.end(), "issued_after_bucket") != keys.end()) == forecast);
            assert((std::find(keys.begin(), keys.end(), "basis_id") != keys.end()) == routed);
        }
    assert(MakeSchema(SchemaKind::kRelationFusion)->GetFieldByName("fusion_score"));
    assert(MakeSchema(SchemaKind::kMaintenance)->GetFieldByName("runtime_identities"));
    assert(MakeSchema(SchemaKind::kProgress)->GetFieldByName("closed_before_bucket"));
    ProgressEnvelope progress{1, {{"d", "epoch", "opaque:42", -1}}};
    assert(ValidateProgress(progress, {"d"}).ok());
    progress.datasets.push_back(progress.datasets[0]);
    assert(!ValidateProgress(progress, {"d"}).ok());
    progress.datasets.pop_back();
    progress.contract_version = 2;
    assert(!ValidateProgress(progress, {"d"}).ok());
    ScalarResult cold;
    cold.target_bucket = 10;
    cold.issued_after_bucket = 10;
    cold.observed = 9.0;
    assert(ValidateResult(cold).ok());
    cold.expected = 0.0;
    assert(!ValidateResult(cold).ok());
    ScalarResult forecast;
    forecast.forecast = true;
    forecast.target_bucket = 11;
    forecast.issued_after_bucket = 10;
    forecast.status = BaselineStatus::kOk;
    forecast.expected = 9.0;
    forecast.lower = 8.0;
    forecast.upper = 10.0;
    assert(ValidateResult(forecast).ok());
    forecast.observed = 9.0;
    assert(!ValidateResult(forecast).ok());
    forecast.observed.reset();
    forecast.can_alert = false;
    assert(!ValidateResult(forecast).ok());
    forecast.can_alert.reset();
    forecast.target_bucket = 10;
    assert(!ValidateResult(forecast).ok());
}
}  // namespace
int main() {
    TestStrictConfig();
    TestResolveAndOwnership();
    TestSingleSourceContract();
    TestDeclaredMappingsAndBounds();
    TestIdentitySchemaAndProgress();
    std::cout << "baseliner configuration, schema, identity and result contracts passed\n";
}
