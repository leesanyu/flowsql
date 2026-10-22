// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#ifdef FLOWSQL_VISUALIZATION_CLICKHOUSE
#include <services/database/drivers/clickhouse_driver.h>
#elif defined(FLOWSQL_VISUALIZATION_POSTGRES)
#include <services/database/drivers/postgres_driver.h>
#else
#include <services/database/drivers/mysql_driver.h>
#endif
#include <services/visualization/workflow.h>
#include <tests/common/test_environment.h>

#include <unistd.h>
#include <cassert>
#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <thread>

using namespace flowsql;
using namespace flowsql::visualization;
using namespace flowsql::database;

int main() {
    try {
#ifdef FLOWSQL_VISUALIZATION_CLICKHOUSE
        const std::string dialect = "clickhouse";
        ClickHouseDriver driver;
#elif defined(FLOWSQL_VISUALIZATION_POSTGRES)
        const std::string dialect = "postgres";
        PostgresDriver driver;
#else
        const std::string dialect = "mysql";
        MysqlDriver driver;
#endif
        if (driver.Connect(flowsql::test::DriverParameters(dialect))) throw std::runtime_error("connection failed");
        const auto table = "vis_connector_" + std::to_string(getpid());
        const auto quoted = QuoteIdentifier(table, dialect);
        const auto double_type = dialect == "postgres" ? "DOUBLE PRECISION" : "DOUBLE";
        const auto slow_query = dialect == "clickhouse" ? "SELECT sum(sipHash64(number)) FROM numbers(10000000000)"
                                : dialect == "postgres" ? "SELECT pg_sleep(10)"
                                                        : "SELECT SLEEP(10)";
        const auto timeout_query = dialect == "clickhouse" ? "SELECT sum(sipHash64(number)) FROM numbers(10000000000)"
                                   : dialect == "postgres" ? "SELECT pg_sleep(5)"
                                                           : "SELECT SLEEP(5)";
        auto setup = driver.CreateSession();
        const auto definition = dialect == "clickhouse"
                                    ? "(a Nullable(String),b Nullable(String),n Nullable(Float64),allnull "
                                      "Nullable(Float64),wide Int64,d Nullable(Decimal(30,10))) ENGINE=Memory"
                                    : "(a VARCHAR(50),b VARCHAR(50),n " + std::string(double_type) + ",allnull " +
                                          double_type + ",wide BIGINT,d DECIMAL(30,10))";
        assert(setup->ExecuteSql(("CREATE TABLE " + quoted + definition).c_str()) >= 0);
        assert(setup->ExecuteSql(("INSERT INTO " + quoted +
                                  " VALUES('x','y|z',10,NULL,9007199254740993,0.1),('x','y|z',NULL,NULL,1,1.5),('x|y','"
                                  "z',20,NULL,2,2.5),(NULL,'',NULL,NULL,3,NULL),('','',NULL,NULL,4,NULL)")
                                     .c_str()) >= 0);
        setup.reset();
        auto create = [&](uint32_t timeout = 5000) {
            DatabaseSnapshotOptionsV1 options;
            options.operation_timeout_ms = timeout;
            options.consistent_transaction = dialect != "clickhouse";
            std::shared_ptr<IDatabaseExplorationSessionV1> query;
            auto session = driver.CreateSession();
            auto* snapshot = dynamic_cast<IDbSnapshotSession*>(session.get());
            query = std::dynamic_pointer_cast<IDatabaseExplorationSessionV1>(session);
            assert(snapshot && query && snapshot->BeginSnapshot(options) == 0);
            return query;
        };
        {
            auto query = create();
            SourceSchemaV1 source{table,
                                  {{"a", "a", "string", "VARCHAR"},
                                   {"b", "b", "string", "VARCHAR"},
                                   {"n", "n", "number", "DOUBLE"},
                                   {"allnull", "allnull", "number", "DOUBLE"}},
                                  dialect};
            auto run = [&](const char* json) {
                rapidjson::Document input;
                input.Parse(json);
                auto plan = CompileWorkflow(source, input, "complete", 100);
                auto parameters = BindValues(plan.parameters);
                ExplorationResultV1 result;
                if (query->Query(plan.sql.c_str(), parameters.data(), parameters.size(), 100, 65536, &result)) {
                    std::cerr << query->QueryError() << std::endl;
                    assert(false);
                }
                return result;
            };
            auto mean = run(
                R"({"workflow":[{"type":"view","query":[{"op":"aggregate","groupBy":[],"measures":[{"field":"n","agg":"mean","asFieldKey":"avg"},{"field":"n","agg":"count","asFieldKey":"nonnull"},{"field":"*","agg":"count","asFieldKey":"rows"}]}]}]})");
            std::cerr << "mean/count/rows=" << std::get<double>(mean.rows[0][0]) << "/"
                      << std::get<int64_t>(mean.rows[0][1]) << "/" << std::get<int64_t>(mean.rows[0][2]) << std::endl;
            assert(std::get<double>(mean.rows[0][0]) == 15 && std::get<int64_t>(mean.rows[0][1]) == 2 &&
                   std::get<int64_t>(mean.rows[0][2]) == 5);
            ExplorationResultV1 quoted_alias;
            const std::string alias = "alias\\`\"";
            const auto alias_sql = "SELECT 1 AS " + QuoteIdentifier(alias, dialect);
            assert(query->Query(alias_sql.c_str(), nullptr, 0, 10, 4096, &quoted_alias) == 0 &&
                   quoted_alias.columns[0].name == alias);
            auto nulls = run(
                R"({"workflow":[{"type":"view","query":[{"op":"aggregate","groupBy":[],"measures":[{"field":"allnull","agg":"sum","asFieldKey":"s"},{"field":"allnull","agg":"mean","asFieldKey":"m"},{"field":"allnull","agg":"min","asFieldKey":"l"},{"field":"allnull","agg":"max","asFieldKey":"h"}]}]}]})");
            for (const auto& value : nulls.rows[0]) assert(std::holds_alternative<std::monostate>(value));
            auto grouped = run(
                R"({"workflow":[{"type":"view","query":[{"op":"aggregate","groupBy":["a","b"],"measures":[{"field":"*","agg":"count","asFieldKey":"c"}]}]}]})");
            assert(grouped.rows.size() == 4);
            auto bound = run(
                R"({"workflow":[{"type":"filter","filters":[{"fid":"a","rule":{"type":"one of","value":["' OR 1=1 --"]}}]},{"type":"view","query":[{"op":"raw","fields":["a"]}]}]})");
            assert(bound.rows.empty());
            ExplorationResultV1 result;
            assert(query->Query(("SELECT wide,d FROM " + quoted + " WHERE wide=9007199254740993").c_str(), nullptr, 0,
                                10, 65536, &result) == 0);
            assert(std::get<int64_t>(result.rows[0][0]) == 9007199254740993LL);
            assert(std::holds_alternative<ExplorationDecimalV1>(result.rows[0][1]));
            assert(query->Query(("SELECT a FROM " + quoted).c_str(), nullptr, 0, 1, 65536, &result) != 0 &&
                   result.rows.empty() && query->QueryError() == "RESULT_LIMIT");
        }
        {
            auto query = create();
            ExplorationResultV1 result;
            assert(query->Query(("DELETE FROM " + quoted).c_str(), nullptr, 0, 10, 1024, &result) != 0);
            assert(query->Query("SELECT 1; SELECT 2", nullptr, 0, 10, 1024, &result) != 0);
            assert(query->Query("SELECT 1 INTO OUTFILE '/tmp/flowsql-invalid'", nullptr, 0, 10, 1024, &result) != 0);
            assert(query->Query("SELECT CAST('2026-01-01' AS DATE)", nullptr, 0, 10, 1024, &result) == 0 &&
                   std::get<std::string>(result.rows[0][0]) == "2026-01-01");
            assert(query->Query("SELECT REPEAT('x',4096)", nullptr, 0, 10, 128, &result) != 0 && result.rows.empty() &&
                   query->QueryError() == "RESULT_LIMIT");
        }
        {
            auto query = create(), second = create();
            ExplorationResultV1 identity;
            assert(query->Query(dialect == "clickhouse" ? "SELECT 1"
                                : dialect == "postgres" ? "SELECT pg_backend_pid()"
                                                        : "SELECT CONNECTION_ID()",
                                nullptr, 0, 10, 1024, &identity) == 0);
            const auto connection_id = std::get<int64_t>(identity.rows[0][0]);
            auto future = std::async(std::launch::async, [&] {
                ExplorationResultV1 result;
                return query->Query(slow_query, nullptr, 0, 10, 1024, &result);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto start = std::chrono::steady_clock::now();
            query->Cancel();
            assert(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready && future.get() != 0);
            assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
            ExplorationResultV1 result;
            assert(second->Query("SELECT 1", nullptr, 0, 10, 1024, &result) == 0);
            const auto active =
                dialect == "clickhouse"
                    ? "SELECT count() FROM system.processes WHERE startsWith(query,'SELECT sum(sipHash64(number))')"
                : dialect == "postgres"
                    ? "SELECT COUNT(*) FROM pg_stat_activity WHERE pid=" + std::to_string(connection_id) +
                          " AND state='active'"
                    : "SELECT COUNT(*) FROM information_schema.processlist WHERE ID=" + std::to_string(connection_id) +
                          " AND COMMAND='Query'";
            assert(second->Query(active.c_str(), nullptr, 0, 10, 1024, &result) == 0 &&
                   std::get<int64_t>(result.rows[0][0]) == 0);
        }
        {
            auto query = create(50);
            ExplorationResultV1 result;
            const auto start = std::chrono::steady_clock::now();
            assert(query->Query(timeout_query, nullptr, 0, 10, 1024, &result) != 0 && query->QueryError() == "TIMEOUT");
            assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
        }
        if (dialect == "clickhouse") {
            auto query = create();
            ExplorationResultV1 result;
            assert(query->Query("SELECT toFloat64('nan')", nullptr, 0, 10, 4096, &result) == 0 &&
                   std::isnan(std::get<double>(result.rows[0][0])));
            const std::string special("x&database=system%00=\0tail", 26);
            DatabaseParameterV1 parameter;
            parameter.kind = DatabaseParameterKindV1::kString;
            parameter.data = special.data();
            parameter.size = special.size();
            assert(query->Query("SELECT ?", &parameter, 1, 10, 4096, &result) == 0 &&
                   std::get<std::string>(result.rows[0][0]) == special);
            assert(query->Query("SELECT toUInt64('18446744073709551615')", nullptr, 0, 10, 4096, &result) == 0 &&
                   std::get<ExplorationDecimalV1>(result.rows[0][0]).text == "18446744073709551615");
            assert(query->Query("SELECT [1,2,3]", nullptr, 0, 10, 4096, &result) != 0 && result.rows.empty() &&
                   query->QueryError() == "UNSUPPORTED_TYPE");
        }
        if (dialect == "postgres") {
            auto ddl = driver.CreateSession();
            const auto schema = "vis.schema_" + std::to_string(getpid());
            assert(ddl->ExecuteSql(("CREATE SCHEMA " + QuoteIdentifier(schema, dialect)).c_str()) >= 0);
            assert(ddl->ExecuteSql(
                       ("CREATE TABLE " + QuoteIdentifier(schema, dialect) + "." + quoted + "(n INTEGER)").c_str()) >=
                   0);
            assert(ddl->ExecuteSql(
                       ("INSERT INTO " + QuoteIdentifier(schema, dialect) + "." + quoted + " VALUES(10),(NULL),(20)")
                           .c_str()) >= 0);
            {
                auto query = create();
                ExplorationResultV1 result;
                SourceSchemaV1 source{table, {{"n", "n", "number", "INTEGER"}}, dialect, schema};
                rapidjson::Document input;
                input.Parse(
                    R"({"workflow":[{"type":"view","query":[{"op":"aggregate","groupBy":[],"measures":[{"field":"n","agg":"mean","asFieldKey":"avg"}]}]}]})");
                auto plan = CompileWorkflow(source, input, "complete", 10);
                assert(query->Query(plan.sql.c_str(), nullptr, 0, 10, 1024, &result) == 0 &&
                       std::stold(std::get<ExplorationDecimalV1>(result.rows[0][0]).text) == 15);
                assert(query->Query("SELECT true,false", nullptr, 0, 10, 1024, &result) == 0 &&
                       std::get<int64_t>(result.rows[0][0]) == 1 && std::get<int64_t>(result.rows[0][1]) == 0);
            }
            assert(ddl->ExecuteSql(("DROP SCHEMA " + QuoteIdentifier(schema, dialect) + " CASCADE").c_str()) >= 0);
        }
        const auto hist_table = QuoteIdentifier(table + "_hist", dialect);
        {
            auto ddl = driver.CreateSession();
            assert(
                ddl->ExecuteSql(
                    ("CREATE TABLE " + hist_table +
                     (dialect == "clickhouse"
                          ? "(n Nullable(Float64),d Nullable(Decimal(30,10)),allnull Nullable(Float64)) ENGINE=Memory"
                          : "(n " + std::string(double_type) + ",d DECIMAL(30,10),allnull " + double_type + ")"))
                        .c_str()) >= 0);
            assert(ddl->ExecuteSql(
                       ("INSERT INTO " + hist_table +
                        " VALUES(-10,-1,NULL),(-5,-0.5,NULL),(0,0,NULL),(5,0.5,NULL),(10,1,NULL),(NULL,NULL,NULL)")
                           .c_str()) >= 0);
        }
        {
            auto query = create();
            SourceSchemaV1 source{table + "_hist",
                                  {{"n", "n", "number", "DOUBLE"},
                                   {"d", "d", "number", "DECIMAL(30,10)"},
                                   {"allnull", "allnull", "number", "DOUBLE"}},
                                  dialect};
            auto number = [](const ExplorationValueV1& value) {
                if (auto p = std::get_if<int64_t>(&value)) return double(*p);
                if (auto p = std::get_if<double>(&value)) return *p;
                return std::stod(std::get<ExplorationDecimalV1>(value).text);
            };
            auto histogram = [&](const std::string& field, const std::string& filter = "") {
                rapidjson::Document input;
                input.Parse(("{\"workflow\":[" + filter +
                             "{\"type\":\"view\",\"query\":[{\"op\":\"histogram\",\"field\":\"" + field +
                             "\",\"bins\":4}]}]}")
                                .c_str());
                auto plan = CompileWorkflow(source, input, "complete", 100);
                auto parameters = BindValues(plan.parameters);
                ExplorationResultV1 result;
                if (query->Query(plan.sql.c_str(), parameters.data(), parameters.size(), 101, 65536, &result)) {
                    std::cerr << "histogram: " << query->QueryError() << std::endl;
                    assert(false);
                }
                return result;
            };
            for (const auto* field : {"n", "d"}) {
                auto result = histogram(field);
                assert(result.rows.size() == 4);
                const double step = std::string(field) == "n" ? 5 : 0.5;
                for (size_t i = 0; i < 4; ++i) {
                    assert(number(result.rows[i][0]) == i);
                    assert(number(result.rows[i][1]) == -2 * step + i * step);
                    assert(number(result.rows[i][2]) == -step + i * step);
                    assert(number(result.rows[i][3]) == (i == 3 ? 2 : 1));
                }
            }
            assert(histogram("allnull").rows.empty());
            auto constant =
                histogram("n", R"({"type":"filter","filters":[{"fid":"n","rule":{"type":"one of","value":[5]}}]},)");
            assert(constant.rows.size() == 1 && number(constant.rows[0][1]) == 5 && number(constant.rows[0][2]) == 5);
            assert(
                histogram(
                    "n",
                    R"({"type":"filter","filters":[{"fid":"n","rule":{"type":"comparison","op":"gt","value":20}}]},)")
                    .rows.empty());
            auto filtered = histogram(
                "n", R"({"type":"filter","filters":[{"fid":"n","rule":{"type":"comparison","op":"gte","value":0}}]},)");
            double total = 0;
            for (const auto& row : filtered.rows) total += number(row[3]);
            assert(total == 3);
            ExplorationResultV1 baseline;
            assert(query->Query(("SELECT COUNT(*) FROM " + hist_table + " WHERE n IS NOT NULL").c_str(), nullptr, 0, 10,
                                4096, &baseline) == 0);
            total = 0;
            for (const auto& row : histogram("n").rows) total += number(row[3]);
            assert(total == number(baseline.rows[0][0]));
        }
        auto cleanup = driver.CreateSession();
        assert(cleanup->ExecuteSql(("DROP TABLE " + quoted).c_str()) >= 0);
        assert(cleanup->ExecuteSql(("DROP TABLE " + hist_table).c_str()) >= 0);
        std::cout
            << "PASS " << dialect
            << " native workflow, NULL tuples, binding, exact values, read-only, limits, cancel/deadline/isolation"
            << std::endl;
        return 0;
    } catch (const std::exception&) {
        std::cerr << "connector test failed: configuration or database unavailable" << std::endl;
        return 1;
    }
}
