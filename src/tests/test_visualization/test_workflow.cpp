// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <services/database/drivers/sqlite_driver.h>
#include <services/visualization/numeric_display.h>
#include <services/visualization/workflow.h>

#include <unistd.h>
#include <cassert>
#include <cstdio>
#include <filesystem>

using namespace flowsql;
using namespace flowsql::visualization;

int main() {
    assert(DecimalForDisplay("1.5").exact);
    assert(DecimalForDisplay("-0.000").exact);
    assert(DecimalForDisplay("1.5e0").exact);
    assert(!DecimalForDisplay("0.1").exact);
    assert(!DecimalForDisplay("0.10000000000000000001").exact);
    assert(DecimalForDisplay("9007199254740991").exact);
    assert(DecimalForDisplay("4.9406564584124654e-324").value != 0);
    for (const auto* text : {"NaN", "Infinity", "1e400", "1e-400", "9007199254740992",
                             "9007199254740991.000000000000001", "-9007199254740991.000000000000001"}) {
        bool rejected = false;
        try {
            DecimalForDisplay(text);
        } catch (const QueryFailure& error) {
            rejected = error.code == "UNSUPPORTED_TYPE";
        }
        assert(rejected);
    }
    for (const std::string dialect : {"sqlite", "mysql", "postgres", "clickhouse"}) {
        SourceSchemaV1 source{"order`\"s", {{"n", "value`\"", "number", "INTEGER"}}, dialect, "schema.dot"};
        rapidjson::Document input;
        input.Parse(
            R"({"workflow":[{"type":"filter","filters":[{"fid":"n","rule":{"type":"comparison","op":"gt","value":10}}]},{"type":"view","query":[{"op":"aggregate","groupBy":[],"measures":[{"field":"n","agg":"mean","asFieldKey":"avg"},{"field":"n","agg":"sum","asFieldKey":"sum"}]}]}]})");
        auto plan = CompileWorkflow(source, input, "complete", 100);
        assert(plan.numeric_inputs.size() == 2);
        assert(plan.numeric_inputs[0].aggregate == "mean" && plan.numeric_inputs[0].output_id == "avg");
        assert(plan.filtered_from.find(">?") != std::string::npos);
        assert(plan.parameters.size() == 1 && std::get<int64_t>(plan.parameters[0]) == 10);
        assert(plan.sql.find(QuoteIdentifier("schema.dot", dialect) + "." + QuoteIdentifier(source.table, dialect)) !=
               std::string::npos);
        assert(plan.sql.find(dialect == "clickhouse" ? "avgOrNull(" : "AVG(") != std::string::npos);
        assert(plan.sql.find(dialect == "clickhouse" ? "sumOrNull(" : "SUM(") != std::string::npos);
        assert(plan.sql.find(">?") != std::string::npos);
    }
    assert(QuoteIdentifier("a`b", "mysql") == "`a``b`");
    assert(QuoteIdentifier("a\"b", "postgres") == "\"a\"\"b\"");
    for (const std::string dialect : {"sqlite", "mysql", "postgres", "clickhouse"}) {
        SourceSchemaV1 calendar{
            "calendar", {{"t", "time", "string", "DATE"}, {"v", "value", "number", "INT"}}, dialect};
        for (const auto* unit : {"day", "month"}) {
            rapidjson::Document input;
            input.Parse(
                (std::string(
                     R"({"workflow":[{"type":"view","query":[{"op":"aggregate","groupBy":[{"field":"t","unit":")") +
                 unit +
                 R"(","asFieldKey":"bucket"}],"measures":[{"field":"v","agg":"distinctCount","asFieldKey":"n"}]}]},{"type":"sort","by":["bucket"],"sort":"ascending"}]})")
                    .c_str());
            auto plan = CompileWorkflow(calendar, input, "complete", 100);
            assert(plan.fields[0].id == "bucket" && plan.fields[0].type == "string");
            assert(plan.numeric_inputs.empty());  // no lossy display conversion of distinct inputs
            assert(plan.sql.find(dialect == "clickhouse" ? "uniqExact(" : "COUNT(DISTINCT ") != std::string::npos);
            assert(plan.sql.find("GROUP BY") != std::string::npos);
        }
    }
    char temporary[] = "/tmp/flowsql-exploration-workflow-XXXXXX";
    const int descriptor = mkstemp(temporary);
    assert(descriptor >= 0);
    close(descriptor);
    const std::string path = temporary;
    sqlite3* db = nullptr;
    assert(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    assert(sqlite3_exec(db,
                        "CREATE TABLE data(a TEXT,b TEXT,n INTEGER,mixed INTEGER);"
                        "CREATE TABLE distribution(n REAL,d REAL,allnull REAL);"
                        "CREATE TABLE calendar(t TEXT,v TEXT);"
                        "INSERT INTO calendar VALUES('2024-02-29T23:30:00-03:00','x'),"
                        "('2024-02-29T01:00:00Z','x'),('2024-03-01',''),('2025-01-01',NULL),(NULL,'y');"
                        "INSERT INTO distribution "
                        "VALUES(-10,-1,NULL),(-5,-0.5,NULL),(0,0,NULL),(5,0.5,NULL),(10,1,NULL),(NULL,NULL,NULL);"
                        "INSERT INTO data VALUES('x','y|z',10,1),('x','y|z',NULL,'text'),"
                        "('x|y','z',20,2),(NULL,'',NULL,NULL),('','',NULL,NULL)",
                        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
    {
        flowsql::database::SqliteDriver driver;
        assert(driver.Connect({{"path", path}, {"readonly", "true"}}) == 0);
        auto session = driver.CreateSession();
        auto* begin = dynamic_cast<flowsql::database::IDbSnapshotSession*>(session.get());
        auto* query = dynamic_cast<IDatabaseExplorationSessionV1*>(session.get());
        assert(begin && query);
        assert(begin->BeginSnapshot(DatabaseSnapshotOptionsV1{}) == 0);
        SourceSchemaV1 source{"data",
                              {{"a", "a", "string", "TEXT"},
                               {"b", "b", "string", "TEXT"},
                               {"n", "n", "number", "INTEGER"},
                               {"mixed", "mixed", "number", "INTEGER"}}};
        auto execute = [&](const char* json, const std::string& mode = "complete", uint32_t maximum = 10000) {
            rapidjson::Document document;
            document.Parse(json);
            assert(!document.HasParseError());
            auto plan = CompileWorkflow(source, document, mode, maximum);
            auto parameters = BindValues(plan.parameters);
            ExplorationResultV1 output;
            assert(query->Query(plan.sql.c_str(), parameters.data(), parameters.size(), maximum + 1, 8 * 1024 * 1024,
                                &output) == 0);
            return output;
        };
        auto mean = execute(
            R"({"workflow":[{"type":"view","query":[{"op":"aggregate","groupBy":[],"measures":[{"field":"n","agg":"mean","asFieldKey":"avg"},{"field":"n","agg":"count","asFieldKey":"nonnull"},{"field":"*","agg":"count","asFieldKey":"rows"}]}]}]})");
        assert(std::get<double>(mean.rows.at(0).at(0)) == 15);
        assert(std::get<int64_t>(mean.rows[0][1]) == 2 && std::get<int64_t>(mean.rows[0][2]) == 5);
        auto nulls = execute(
            R"({"workflow":[{"type":"filter","filters":[{"fid":"a","rule":{"type":"one of","value":[null,""]}}]},{"type":"view","query":[{"op":"aggregate","groupBy":[],"measures":[{"field":"n","agg":"sum","asFieldKey":"s"},{"field":"n","agg":"mean","asFieldKey":"m"},{"field":"n","agg":"min","asFieldKey":"l"},{"field":"n","agg":"max","asFieldKey":"h"}]}]}]})");
        for (const auto& value : nulls.rows[0]) assert(std::holds_alternative<std::monostate>(value));
        auto grouped = execute(
            R"({"workflow":[{"type":"view","query":[{"op":"aggregate","groupBy":["a","b"],"measures":[{"field":"*","agg":"count","asFieldKey":"c"}]}]}]})");
        assert(grouped.rows.size() == 4);  // tuple keys and NULL != empty string
        auto mixed = execute(R"({"workflow":[{"type":"view","query":[{"op":"raw","fields":["mixed"]}]}]})");
        assert(std::get<int64_t>(mixed.rows[0][0]) == 1 && std::get<std::string>(mixed.rows[1][0]) == "text");
        auto top = execute(
            R"({"workflow":[{"type":"view","query":[{"op":"aggregate","groupBy":["a"],"measures":[{"field":"n","agg":"sum","asFieldKey":"total"}]}]},{"type":"sort","by":["total"],"sort":"descending"}],"limit":1})");
        assert(top.rows.size() == 1 && std::get<int64_t>(top.rows[0][1]) == 20);
        const auto injection = execute(
            R"({"workflow":[{"type":"filter","filters":[{"fid":"a","rule":{"type":"one of","value":["' OR 1=1 --"]}}]},{"type":"view","query":[{"op":"raw","fields":["a"]}]}]})");
        assert(injection.rows.empty());
        ExplorationResultV1 invalid;
        assert(query->Query("DELETE FROM data", nullptr, 0, 10, 1024, &invalid) != 0);
        assert(query->Query("SELECT 1; SELECT 2", nullptr, 0, 10, 1024, &invalid) != 0);
        for (
            const char* json :
            {R"({"workflow":[{"type":"transform","transform":[]}]})",
             R"({"workflow":[{"type":"view","query":[{"op":"raw","fields":["unknown"]}]}]})",
             R"({"workflow":[{"type":"view","query":[{"op":"aggregate","groupBy":[],"measures":[{"field":"n","agg":"median","asFieldKey":"x"}]}]}]})"}) {
            rapidjson::Document input;
            input.Parse(json);
            bool rejected = false;
            try {
                CompileWorkflow(source, input, "complete", 10000);
            } catch (const QueryFailure&) {
                rejected = true;
            }
            assert(rejected);
        }
        SourceSchemaV1 distribution{
            "distribution",
            {{"n", "n", "number", "REAL"}, {"d", "d", "number", "REAL"}, {"allnull", "allnull", "number", "REAL"}}};
        auto histogram = [&](const std::string& field, int bins, const std::string& filter = "") {
            rapidjson::Document input;
            input.Parse(("{\"workflow\":[" + filter +
                         "{\"type\":\"view\",\"query\":[{\"op\":\"histogram\",\"field\":\"" + field +
                         "\",\"bins\":" + std::to_string(bins) + "}]}]}")
                            .c_str());
            auto plan = CompileWorkflow(distribution, input, "complete", 100);
            auto parameters = BindValues(plan.parameters);
            ExplorationResultV1 result;
            assert(query->Query(plan.sql.c_str(), parameters.data(), parameters.size(), 101, 65536, &result) == 0);
            return result;
        };
        auto bins = histogram("n", 4);
        assert(bins.rows.size() == 4);
        for (size_t i = 0; i < 4; ++i) {
            assert(std::get<int64_t>(bins.rows[i][0]) == int64_t(i));
            assert(std::get<double>(bins.rows[i][1]) == -10 + 5 * int(i));
            assert(std::get<double>(bins.rows[i][2]) == -5 + 5 * int(i));
            assert(std::get<int64_t>(bins.rows[i][3]) == (i == 3 ? 2 : 1));
        }
        assert(histogram("d", 4).rows.size() == 4);
        assert(histogram("allnull", 4).rows.empty());
        const auto filtered = histogram(
            "n", 2, R"({"type":"filter","filters":[{"fid":"n","rule":{"type":"comparison","op":"gte","value":0}}]},)");
        assert(filtered.rows.size() == 2 && std::get<int64_t>(filtered.rows[1][3]) == 2);
        const auto constant =
            histogram("n", 10, R"({"type":"filter","filters":[{"fid":"n","rule":{"type":"one of","value":[5]}}]},)");
        assert(constant.rows.size() == 1 && std::get<double>(constant.rows[0][1]) == 5 &&
               std::get<double>(constant.rows[0][2]) == 5);
        assert(
            histogram("n", 10,
                      R"({"type":"filter","filters":[{"fid":"n","rule":{"type":"comparison","op":"gt","value":20}}]},)")
                .rows.empty());
        for (int count : {0, -1, 101}) {
            bool rejected = false;
            try {
                histogram("n", count);
            } catch (const QueryFailure& error) {
                rejected = error.code == "INVALID_WORKFLOW";
            }
            assert(rejected);
        }
        source = {"calendar", {{"t", "t", "string", "TEXT"}, {"v", "v", "string", "TEXT"}}};
        auto days = execute(
            R"({"workflow":[{"type":"view","query":[{"op":"aggregate","groupBy":[{"field":"t","unit":"day","asFieldKey":"day"}],"measures":[{"field":"v","agg":"distinctCount","asFieldKey":"n"}]}]},{"type":"sort","by":["day"],"sort":"ascending"}]})");
        assert(days.rows.size() == 4 && std::holds_alternative<std::monostate>(days.rows[0][0]));
        assert(std::get<std::string>(days.rows[1][0]) == "2024-02-29" && std::get<int64_t>(days.rows[1][1]) == 1);
        assert(std::get<int64_t>(days.rows[2][1]) == 1 && std::get<int64_t>(days.rows[3][1]) == 0);
        auto months = execute(
            R"({"workflow":[{"type":"filter","filters":[{"fid":"v","rule":{"type":"one of","value":["x"]}}]},{"type":"view","query":[{"op":"aggregate","groupBy":[{"field":"t","unit":"month","asFieldKey":"month"}],"measures":[{"field":"*","agg":"count","asFieldKey":"n"}]}]}]})");
        assert(months.rows.size() == 1 && std::get<std::string>(months.rows[0][0]) == "2024-02-01" &&
               std::get<int64_t>(months.rows[0][1]) == 2);
        auto empty_distinct = execute(
            R"({"workflow":[{"type":"filter","filters":[{"fid":"v","rule":{"type":"one of","value":["missing"]}}]},{"type":"view","query":[{"op":"aggregate","groupBy":[],"measures":[{"field":"v","agg":"distinctCount","asFieldKey":"n"}]}]}]})");
        assert(empty_distinct.rows.size() == 1 && std::get<int64_t>(empty_distinct.rows[0][0]) == 0);
        for (
            const char* json :
            {R"({"workflow":[{"type":"view","query":[{"op":"aggregate","groupBy":[{"field":"t","unit":"week","asFieldKey":"b"}],"measures":[]}]}]})",
             R"({"workflow":[{"type":"view","query":[{"op":"aggregate","groupBy":[{"field":"unknown","unit":"day","asFieldKey":"b"}],"measures":[]}]}]})",
             R"({"workflow":[{"type":"view","query":[{"op":"aggregate","groupBy":[{"field":"t","unit":"day","asFieldKey":"b","sql":"bad"}],"measures":[]}]}]})",
             R"({"workflow":[{"type":"view","query":[{"op":"aggregate","groupBy":[{"field":"t","unit":"day","asFieldKey":"n"}],"measures":[{"field":"v","agg":"count","asFieldKey":"n"}]}]}]})",
             R"({"workflow":[{"type":"view","query":[{"op":"aggregate","groupBy":[],"measures":[{"field":"*","agg":"distinctCount","asFieldKey":"n"}]}]}]})"}) {
            bool rejected = false;
            try {
                execute(json);
            } catch (const QueryFailure&) {
                rejected = true;
            }
            assert(rejected);
        }
        assert(!SupportsTimeBucket({"n", "n", "number", "INTEGER"}, "sqlite"));
        assert(!SupportsTimeBucket({"t", "t", "string", "TIME"}, "mysql"));
        assert(!SupportsTimeBucket({"t", "t", "string", "TEXT"}, "postgres"));
        assert(SupportsTimeBucket({"t", "t", "string", "Nullable(DateTime64(3, 'Asia/Shanghai'))"}, "clickhouse"));
        query->Cancel();
        assert(query->Query("SELECT 1", nullptr, 0, 10, 1024, &invalid) != 0);
    }
    std::filesystem::remove(path);
    puts("PASS SQLite native aggregation, tuple/NULL/mixed values, prepared filters and read-only contract");
}
