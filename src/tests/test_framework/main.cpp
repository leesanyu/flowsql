#include <cstdio>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <regex>

#include <common/loader.hpp>
#include <framework/core/channel_adapter.h>
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/core/pipeline.h>
#include <framework/core/sql_parser.h>
#include <framework/interfaces/ichannel.h>
#include <framework/interfaces/idataframe_channel.h>
#include <framework/interfaces/ioperator.h>

using namespace flowsql;

// Test declarations
void test_dataframe_basic();
void test_dataframe_arrow();
void test_dataframe_json();
void test_dataframe_clear();
void test_dataframe_channel();
void test_sql_parser();
void test_normalize_from_table_name();
void test_channel_adapter_copy();
void test_pipeline(const std::string& plugin_dir);

// ============================================================
// Test 1: DataFrame 基本操作
// ============================================================
void test_dataframe_basic() {
    printf("[TEST] DataFrame basic operations...\n");
    DataFrame df;

    std::vector<Field> schema = {
        {"src_ip", DataType::STRING, 0, ""},
        {"dst_ip", DataType::STRING, 0, ""},
        {"bytes_sent", DataType::UINT64, 0, ""},
        {"protocol", DataType::STRING, 0, ""},
    };
    df.SetSchema(schema);

    df.AppendRow({std::string("192.168.1.1"), std::string("8.8.8.8"), uint64_t(1024), std::string("HTTP")});
    df.AppendRow({std::string("10.0.0.1"), std::string("172.16.0.1"), uint64_t(2048), std::string("DNS")});

    assert(df.RowCount() == 2);

    auto row0 = df.GetRow(0);
    assert(std::get<std::string>(row0[0]) == "192.168.1.1");
    assert(std::get<uint64_t>(row0[2]) == 1024);

    auto col = df.GetColumn("protocol");
    assert(col.size() == 2);
    assert(std::get<std::string>(col[0]) == "HTTP");
    assert(std::get<std::string>(col[1]) == "DNS");

    printf("[PASS] DataFrame basic operations\n");
}

// ============================================================
// Test 2: DataFrame Arrow 互操作
// ============================================================
void test_dataframe_arrow() {
    printf("[TEST] DataFrame Arrow interop...\n");
    DataFrame df1;
    std::vector<Field> schema = {
        {"id", DataType::INT32, 0, ""},
        {"name", DataType::STRING, 0, ""},
        {"score", DataType::DOUBLE, 0, ""},
    };
    df1.SetSchema(schema);
    df1.AppendRow({int32_t(1), std::string("Alice"), double(95.5)});
    df1.AppendRow({int32_t(2), std::string("Bob"), double(87.3)});

    auto batch = df1.ToArrow();
    assert(batch != nullptr);
    assert(batch->num_rows() == 2);
    assert(batch->num_columns() == 3);

    DataFrame df2;
    df2.FromArrow(batch);
    assert(df2.RowCount() == 2);
    auto row = df2.GetRow(1);
    assert(std::get<int32_t>(row[0]) == 2);
    assert(std::get<std::string>(row[1]) == "Bob");

    printf("[PASS] DataFrame Arrow interop\n");
}

// ============================================================
// Test 3: DataFrame JSON 序列化
// ============================================================
void test_dataframe_json() {
    printf("[TEST] DataFrame JSON serialization...\n");
    DataFrame df1;
    std::vector<Field> schema = {
        {"ip", DataType::STRING, 0, ""},
        {"port", DataType::UINT32, 0, ""},
        {"active", DataType::BOOLEAN, 0, ""},
    };
    df1.SetSchema(schema);
    df1.AppendRow({std::string("10.0.0.1"), uint32_t(8080), true});
    df1.AppendRow({std::string("10.0.0.2"), uint32_t(443), false});

    std::string json = df1.ToJson();
    assert(!json.empty());

    DataFrame df2;
    bool ok = df2.FromJson(json);
    assert(ok);
    assert(df2.RowCount() == 2);
    auto row = df2.GetRow(0);
    assert(std::get<std::string>(row[0]) == "10.0.0.1");
    assert(std::get<uint32_t>(row[1]) == 8080);
    assert(std::get<bool>(row[2]) == true);

    printf("[PASS] DataFrame JSON serialization\n");
}

// ============================================================
// Test 4: 插件加载 + Pipeline 数据流通（使用 PluginLoader）
// ============================================================
void test_pipeline(const std::string& plugin_dir) {
    printf("[TEST] Plugin loading + Pipeline (PluginLoader)...\n");

    PluginLoader* loader = PluginLoader::Single();

    std::string plugin_name = "libflowsql_example.so";
    const char* relapath[] = {plugin_name.c_str()};
    const char* options[] = {nullptr};
    int ret = loader->Load(plugin_dir.c_str(), relapath, options, 1);
    if (ret != 0) {
        printf("[SKIP] Plugin not found: %s, skipping pipeline test\n", plugin_name.c_str());
        return;
    }
    loader->StartAll();

    // 查询插件（通过 IQuerier 接口）
    IChannel* source_ch = nullptr;
    loader->Traverse(IID_CHANNEL, [&](void* p) -> int {
        auto* ch = static_cast<IChannel*>(p);
        if (std::string(ch->Catelog()) == "example" && std::string(ch->Name()) == "memory") {
            source_ch = ch;
            return -1;
        }
        return 0;
    });

    IOperator* op = nullptr;
    loader->Traverse(IID_OPERATOR, [&](void* p) -> int {
        auto* o = static_cast<IOperator*>(p);
        if (o->Catelog() == "example" && o->Name() == "passthrough") {
            op = o;
            return -1;
        }
        return 0;
    });

    assert(source_ch != nullptr);
    assert(op != nullptr);

    auto* df_source = dynamic_cast<IDataFrameChannel*>(source_ch);
    assert(df_source != nullptr);

    DataFrame df_input;
    std::vector<Field> schema = {
        {"name", DataType::STRING, 0, ""},
        {"value", DataType::INT32, 0, ""},
    };
    df_input.SetSchema(schema);
    df_input.AppendRow({std::string("alpha"), int32_t(10)});
    df_input.AppendRow({std::string("beta"), int32_t(20)});

    df_source->Open();
    df_source->Write(&df_input);

    DataFrameChannel sink("test", "sink");
    sink.Open();

    auto pipeline = PipelineBuilder()
        .SetSource(df_source)
        .SetOperator(op)
        .SetSink(&sink)
        .Build();
    pipeline->Run();
    assert(pipeline->State() == PipelineState::STOPPED);

    DataFrame df_output;
    sink.Read(&df_output);
    assert(df_output.RowCount() == 2);

    auto row = df_output.GetRow(0);
    assert(std::get<std::string>(row[0]) == "alpha");
    assert(std::get<int32_t>(row[1]) == 10);

    auto row1 = df_output.GetRow(1);
    assert(std::get<std::string>(row1[0]) == "beta");
    assert(std::get<int32_t>(row1[1]) == 20);

    df_source->Close();
    sink.Close();
    loader->StopAll();
    loader->Unload();

    printf("[PASS] Plugin loading + Pipeline (PluginLoader)\n");
}

// ============================================================
// Test 5: DataFrame Clear + 重复使用
// ============================================================
void test_dataframe_clear() {
    printf("[TEST] DataFrame clear and reuse...\n");
    DataFrame df;
    std::vector<Field> schema = {{"val", DataType::INT32, 0, ""}};
    df.SetSchema(schema);
    df.AppendRow({int32_t(42)});
    assert(df.RowCount() == 1);

    df.Clear();
    assert(df.RowCount() == 0);

    df.AppendRow({int32_t(100)});
    assert(df.RowCount() == 1);
    assert(std::get<int32_t>(df.GetRow(0)[0]) == 100);

    printf("[PASS] DataFrame clear and reuse\n");
}

// ============================================================
// Test 6: DataFrameChannel 读写语义
// ============================================================
void test_dataframe_channel() {
    printf("[TEST] DataFrameChannel read/write semantics...\n");

    DataFrameChannel ch("test", "channel");
    ch.Open();

    DataFrame df1;
    df1.SetSchema({{"x", DataType::INT32, 0, ""}});
    df1.AppendRow({int32_t(1)});
    df1.AppendRow({int32_t(2)});
    ch.Write(&df1);

    DataFrame out1, out2;
    ch.Read(&out1);
    ch.Read(&out2);
    assert(out1.RowCount() == 2);
    assert(out2.RowCount() == 2);
    assert(std::get<int32_t>(out1.GetRow(0)[0]) == 1);
    assert(std::get<int32_t>(out2.GetRow(1)[0]) == 2);

    DataFrame df2;
    df2.SetSchema({{"y", DataType::STRING, 0, ""}});
    df2.AppendRow({std::string("hello")});
    ch.Write(&df2);

    DataFrame out3;
    ch.Read(&out3);
    assert(out3.RowCount() == 1);
    assert(std::get<std::string>(out3.GetRow(0)[0]) == "hello");

    assert(std::string(ch.Type()) == "dataframe");
    assert(std::string(ch.Catelog()) == "test");
    assert(std::string(ch.Name()) == "channel");

    ch.Close();
    printf("[PASS] DataFrameChannel read/write semantics\n");
}

// ============================================================
// Test 7: SQL 解析器基础测试
// ============================================================
void test_sql_parser() {
    printf("[TEST] SQL parser (USING optional + columns)...\n");
    SqlParser parser;

    {
        auto stmt = parser.Parse("SELECT * FROM test.data USING explore.chisquare WITH threshold=0.05 INTO result");
        assert(stmt.error.empty());
        assert(stmt.source == "test.data");
        assert(stmt.op_catelog == "explore");
        assert(stmt.op_name == "chisquare");
        assert(stmt.with_params["threshold"] == "0.05");
        assert(stmt.dest == "result");
        assert(stmt.columns.empty());
        assert(stmt.HasOperator());
    }

    {
        auto stmt = parser.Parse("SELECT * FROM memory_data INTO clickhouse.my_table");
        assert(stmt.error.empty());
        assert(stmt.source == "memory_data");
        assert(!stmt.HasOperator());
        assert(stmt.dest == "clickhouse.my_table");
    }

    {
        auto stmt = parser.Parse("SELECT * FROM test.data");
        assert(stmt.error.empty());
        assert(stmt.source == "test.data");
        assert(!stmt.HasOperator());
        assert(stmt.dest.empty());
    }

    {
        auto stmt = parser.Parse("SELECT src_ip, dst_ip, bytes_sent FROM test.data USING explore.chisquare");
        assert(stmt.error.empty());
        assert(stmt.columns.size() == 3);
        assert(stmt.columns[0] == "src_ip");
        assert(stmt.HasOperator());
    }

    {
        auto stmt = parser.Parse("select * from test.data into result");
        assert(stmt.error.empty());
        assert(stmt.source == "test.data");
        assert(stmt.dest == "result");
    }

    // Test sql_part extraction with GROUP BY/ORDER BY (Story 4.5)
    {
        auto stmt = parser.Parse("SELECT a, COUNT(*) FROM source GROUP BY a ORDER BY COUNT(*) DESC USING ml.predict");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT a, COUNT(*) FROM source GROUP BY a ORDER BY COUNT(*) DESC");
        assert(stmt.op_catelog == "ml");
        assert(stmt.op_name == "predict");
        assert(stmt.HasOperator());
    }

    // Test sql_part extraction without extension
    {
        auto stmt = parser.Parse("SELECT a FROM source WHERE x>1 GROUP BY a");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT a FROM source WHERE x>1 GROUP BY a");
        assert(!stmt.HasOperator());
    }

    // Test sql_part extraction with WITH clause
    {
        auto stmt = parser.Parse("SELECT a, b FROM source WHERE x>1 WITH threshold=0.5 INTO dest");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT a, b FROM source WHERE x>1");
        assert(stmt.with_params["threshold"] == "0.5");
        assert(stmt.dest == "dest");
    }

    // Test sql_part extraction with all clauses
    {
        auto stmt = parser.Parse("SELECT a, COUNT(*) FROM source WHERE x>1 GROUP BY a HAVING COUNT(*)>5 ORDER BY a LIMIT 10 USING ml.train WITH epochs=100 INTO result");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT a, COUNT(*) FROM source WHERE x>1 GROUP BY a HAVING COUNT(*)>5 ORDER BY a LIMIT 10");
        assert(stmt.op_catelog == "ml");
        assert(stmt.op_name == "train");
        assert(stmt.with_params["epochs"] == "100");
        assert(stmt.dest == "result");
    }

    printf("[PASS] SQL parser (USING optional + columns)\n");
}

// ============================================================
// Test 8: NormalizeFromTableName (Story 4.5)
// ============================================================
static std::string NormalizeFromTableName(const std::string& sql) {
    std::string result = sql;
    std::regex FROM_PATTERN(R"((\bFROM\s+)((?:[\w]+\.)*)([\w]+))");
    result = std::regex_replace(result, FROM_PATTERN, "$1$3");
    return result;
}

static std::string ExtractTableName_Test(const std::string& dest_name) {
    auto pos1 = dest_name.find('.');
    if (pos1 != std::string::npos) {
        auto pos2 = dest_name.find('.', pos1 + 1);
        if (pos2 != std::string::npos) {
            return dest_name.substr(pos2 + 1);  // 三段式
        }
        return dest_name.substr(pos1 + 1);  // 两段式
    }
    return dest_name;
}

void test_normalize_from_table_name() {
    printf("[TEST] NormalizeFromTableName...\n");

    // Test 1: Three-part table name
    {
        std::string sql = "SELECT * FROM sqlite.mydb.users WHERE id > 1";
        std::string normalized = NormalizeFromTableName(sql);
        assert(normalized == "SELECT * FROM users WHERE id > 1");
        printf("  [PASS] Three-part table name\n");
    }

    // Test 2: Two-part table name
    {
        std::string sql = "SELECT * FROM mydb.users WHERE id > 1";
        std::string normalized = NormalizeFromTableName(sql);
        assert(normalized == "SELECT * FROM users WHERE id > 1");
        printf("  [PASS] Two-part table name\n");
    }

    // Test 3: One-part table name
    {
        std::string sql = "SELECT * FROM users WHERE id > 1";
        std::string normalized = NormalizeFromTableName(sql);
        assert(normalized == "SELECT * FROM users WHERE id > 1");
        printf("  [PASS] One-part table name\n");
    }

    // Test 4: String literal with dots
    {
        std::string sql = "SELECT * FROM users WHERE name = 'John.Doe'";
        std::string normalized = NormalizeFromTableName(sql);
        assert(normalized == "SELECT * FROM users WHERE name = 'John.Doe'");
        printf("  [PASS] String literal with dots\n");
    }

    // Test 5: Subquery normalization
    {
        std::string sql = "SELECT * FROM sqlite.mydb.users WHERE id IN (SELECT user_id FROM mydb.orders)";
        std::string normalized = NormalizeFromTableName(sql);
        assert(normalized == "SELECT * FROM users WHERE id IN (SELECT user_id FROM orders)");
        printf("  [PASS] Subquery normalization\n");
    }

    // Test 6: GROUP BY and ORDER BY preserved
    {
        std::string sql = "SELECT a, COUNT(*) FROM sqlite.mydb.logs GROUP BY a ORDER BY COUNT(*) DESC";
        std::string normalized = NormalizeFromTableName(sql);
        assert(normalized == "SELECT a, COUNT(*) FROM logs GROUP BY a ORDER BY COUNT(*) DESC");
        printf("  [PASS] GROUP BY and ORDER BY preserved\n");
    }

    // Test 7: Complex query with multiple clauses
    {
        std::string sql = "SELECT a, COUNT(*) FROM catalog.db.table WHERE x > 1 GROUP BY a HAVING COUNT(*) > 5 ORDER BY a LIMIT 10";
        std::string normalized = NormalizeFromTableName(sql);
        assert(normalized == "SELECT a, COUNT(*) FROM table WHERE x > 1 GROUP BY a HAVING COUNT(*) > 5 ORDER BY a LIMIT 10");
        printf("  [PASS] Complex query with multiple clauses\n");
    }

    printf("[PASS] NormalizeFromTableName\n");
}

// ============================================================
// Test 9: BuildQuery integration test (Story 4.5)
// ============================================================
static std::string BuildQuery_Test(const std::string& source_name, const SqlStatement& stmt) {
    std::string sql = stmt.sql_part;
    sql = NormalizeFromTableName(sql);
    std::string table = ExtractTableName_Test(source_name);
    std::regex FROM_PATTERN(R"((\bFROM\s+)[\w\.]+)");
    sql = std::regex_replace(sql, FROM_PATTERN, "$1" + table);
    return sql;
}

void test_build_query_integration() {
    printf("[TEST] BuildQuery integration (Story 4.5)...\n");
    SqlParser parser;

    // Test 1: Database channel with GROUP BY/ORDER BY
    {
        auto stmt = parser.Parse("SELECT a, COUNT(*) FROM sqlite.mydb.logs GROUP BY a ORDER BY COUNT(*) DESC USING ml.predict");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT a, COUNT(*) FROM source GROUP BY a ORDER BY COUNT(*) DESC");

        std::string query = BuildQuery_Test("sqlite.mydb.logs", stmt);
        assert(query == "SELECT a, COUNT(*) FROM logs GROUP BY a ORDER BY COUNT(*) DESC");
        printf("  [PASS] Database channel with GROUP BY/ORDER BY\n");
    }

    // Test 2: Database channel with subquery
    {
        auto stmt = parser.Parse("SELECT * FROM sqlite.mydb.users WHERE id IN (SELECT user_id FROM sqlite.mydb.orders)");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT * FROM source WHERE id IN (SELECT user_id FROM sqlite.mydb.orders)");

        std::string query = BuildQuery_Test("sqlite.mydb.users", stmt);
        assert(query == "SELECT * FROM users WHERE id IN (SELECT user_id FROM orders)");
        printf("  [PASS] Database channel with subquery\n");
    }

    // Test 3: DataFrame channel (sql_part should contain full SQL without USING)
    {
        auto stmt = parser.Parse("SELECT a, b FROM dataframe_source WHERE x > 1 AND y < 10");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT a, b FROM dataframe_source WHERE x > 1 AND y < 10");
        assert(stmt.where_clause == "x > 1 AND y < 10");
        printf("  [PASS] DataFrame channel sql_part and where_clause\n");
    }

    // Test 4: Full SQL with all clauses
    {
        auto stmt = parser.Parse("SELECT a, COUNT(*) FROM catalog.db.table WHERE x > 1 GROUP BY a HAVING COUNT(*) > 5 ORDER BY a LIMIT 10 USING ml.train WITH epochs=100 INTO result");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT a, COUNT(*) FROM source WHERE x > 1 GROUP BY a HAVING COUNT(*) > 5 ORDER BY a LIMIT 10");
        assert(stmt.op_catelog == "ml");
        assert(stmt.op_name == "train");
        assert(stmt.with_params["epochs"] == "100");
        assert(stmt.dest == "result");

        std::string query = BuildQuery_Test("catalog.db.table", stmt);
        assert(query == "SELECT a, COUNT(*) FROM table WHERE x > 1 GROUP BY a HAVING COUNT(*) > 5 ORDER BY a LIMIT 10");
        printf("  [PASS] Full SQL with all clauses\n");
    }

    printf("[PASS] BuildQuery integration\n");
}

// ============================================================
// Test 10: ChannelAdapter — DataFrame 搬运
// ============================================================
void test_channel_adapter_copy() {
    printf("[TEST] ChannelAdapter CopyDataFrame...\n");

    DataFrameChannel src("test", "src");
    DataFrameChannel dst("test", "dst");
    src.Open();
    dst.Open();

    DataFrame df;
    df.SetSchema({{"x", DataType::INT32, 0, ""}, {"y", DataType::STRING, 0, ""}});
    df.AppendRow({int32_t(1), std::string("hello")});
    df.AppendRow({int32_t(2), std::string("world")});
    src.Write(&df);

    int rc = ChannelAdapter::CopyDataFrame(&src, &dst);
    assert(rc == 0);

    DataFrame result;
    dst.Read(&result);
    assert(result.RowCount() == 2);
    assert(std::get<int32_t>(result.GetRow(0)[0]) == 1);
    assert(std::get<std::string>(result.GetRow(1)[1]) == "world");

    src.Close();
    dst.Close();
    printf("[PASS] ChannelAdapter CopyDataFrame\n");
}

// ============================================================
// main
// ============================================================
int main(int argc, char* argv[]) {
    printf("=== FlowSQL Framework Tests ===\n\n");

    test_dataframe_basic();
    test_dataframe_arrow();
    test_dataframe_json();
    test_dataframe_clear();
    test_dataframe_channel();
    test_sql_parser();
    test_normalize_from_table_name();
    test_build_query_integration();
    test_channel_adapter_copy();

    // Pipeline 测试需要插件 .so
    std::string plugin_dir = get_absolute_process_path();
    test_pipeline(plugin_dir);

    printf("\n=== All tests passed ===\n");
    return 0;
}
