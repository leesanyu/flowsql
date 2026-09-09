// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <cstdio>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>
#include <regex>
#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <framework/core/channel_adapter.h>
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/core/error_contract.h>
#include <framework/core/filter_binding.h>
#include <framework/core/filter_expression.h>
#include <framework/core/filter_executor.h>
#include <framework/core/filter_planner.h>
#include <framework/core/json_error_builder.h>
#include <framework/core/memory_channel.h>
#include <framework/core/packet_codec.h>
#include <framework/core/packet_filter_plan.h>
#include <framework/builtin/dataframe/passthrough_operator.h>
#include <framework/core/pipeline.h>
#include <framework/core/sql_parser.h>
#include <framework/core/sql_text_splitter.h>
#include <framework/interfaces/ichannel.h>
#include <framework/interfaces/iblock_stream_factory.h>
#include <framework/interfaces/iblock_stream_operator.h>
#include <framework/interfaces/iblock_stream_reader.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/idataframe_channel.h>
#include <framework/interfaces/ifilter_pushdown.h>
#include <framework/interfaces/ifilter_domain_resolver.h>
#include <framework/interfaces/ioperator.h>
#include <rapidjson/document.h>

using namespace flowsql;

// Test declarations
void test_dataframe_basic();
void test_dataframe_arrow();
void test_dataframe_json();
void test_dataframe_clear();
void test_dataframe_channel();
void test_dataframe_channel_append();
void test_sql_parser();
void test_filter_expression_parser();
void test_filter_schema_binding();
void test_filter_mask_evaluation();
void test_filter_record_batch();
void test_block_filter_stage();
void test_block_transform_pipeline_runner();
void test_filter_pushdown_split();
void test_filter_pushdown_negotiation();
void test_statement_stage_filters();
void test_offline_filter_public_contracts();
void test_offline_filter_reader_factory_contracts();
void test_stage_filter_interfaces();
void test_filter_task_session_isolation();
void test_sql_text_splitter();
void test_operator_multi_input_fallback();
void test_span_safety();
void test_normalize_from_table_name();
void test_channel_adapter_copy();
void test_channel_type_constants();
void test_json_error_builder();
void test_pipeline();

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
// Test 4: Pipeline 数据流通（直接构造 core 组件）
// ============================================================
void test_pipeline() {
    printf("[TEST] Pipeline run with MemoryChannel + PassthroughOperator...\n");

    MemoryChannel source;
    source.SetIdentity("test", "memory");
    PassthroughOperator op;

    DataFrame df_input;
    std::vector<Field> schema = {
        {"name", DataType::STRING, 0, ""},
        {"value", DataType::INT32, 0, ""},
    };
    df_input.SetSchema(schema);
    df_input.AppendRow({std::string("alpha"), int32_t(10)});
    df_input.AppendRow({std::string("beta"), int32_t(20)});

    source.Open();
    source.Write(&df_input);

    DataFrameChannel sink("test", "sink");
    sink.Open();

    auto pipeline = PipelineBuilder()
        .SetSource(&source)
        .SetOperator(&op)
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

    source.Close();
    sink.Close();

    printf("[PASS] Pipeline run with MemoryChannel + PassthroughOperator\n");
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
    assert(std::string(ch.Category()) == "test");
    assert(std::string(ch.Name()) == "channel");

    ch.Close();
    printf("[PASS] DataFrameChannel read/write semantics\n");
}

// ============================================================
// Test 6.1: DataFrameChannel Append 追加语义
// ============================================================
void test_dataframe_channel_append() {
    printf("[TEST] DataFrameChannel append semantics...\n");

    DataFrameChannel ch("test", "append_channel");
    ch.Open();

    auto* appendable = dynamic_cast<IAppendableDataFrameChannel*>(&ch);
    assert(appendable != nullptr);

    DataFrame base;
    base.SetSchema({{"x", DataType::INT32, 0, ""}});
    base.AppendRow({int32_t(1)});
    base.AppendRow({int32_t(2)});
    assert(ch.Write(&base) == 0);

    DataFrame inc;
    inc.SetSchema({{"x", DataType::INT32, 0, ""}});
    inc.AppendRow({int32_t(3)});
    assert(appendable->Append(&inc) == 0);

    DataFrame out;
    assert(ch.Read(&out) == 0);
    assert(out.RowCount() == 3);
    assert(std::get<int32_t>(out.GetRow(0)[0]) == 1);
    assert(std::get<int32_t>(out.GetRow(1)[0]) == 2);
    assert(std::get<int32_t>(out.GetRow(2)[0]) == 3);

    DataFrame bad;
    bad.SetSchema({{"y", DataType::STRING, 0, ""}});
    bad.AppendRow({std::string("bad")});
    assert(appendable->Append(&bad) != 0);

    DataFrame out_after_bad;
    assert(ch.Read(&out_after_bad) == 0);
    assert(out_after_bad.RowCount() == 3);

    ch.Close();

    MemoryChannel mem;
    mem.SetIdentity("test", "append_memory");
    mem.Open();
    auto* mem_appendable = dynamic_cast<IAppendableDataFrameChannel*>(&mem);
    assert(mem_appendable != nullptr);

    DataFrame mem_base;
    mem_base.SetSchema({{"k", DataType::INT32, 0, ""}});
    mem_base.AppendRow({int32_t(10)});
    assert(mem.Write(&mem_base) == 0);

    DataFrame mem_inc;
    mem_inc.SetSchema({{"k", DataType::INT32, 0, ""}});
    mem_inc.AppendRow({int32_t(20)});
    assert(mem_appendable->Append(&mem_inc) == 0);

    DataFrame mem_out;
    assert(mem.Read(&mem_out) == 0);
    assert(mem_out.RowCount() == 2);
    assert(std::get<int32_t>(mem_out.GetRow(0)[0]) == 10);
    assert(std::get<int32_t>(mem_out.GetRow(1)[0]) == 20);
    mem.Close();

    printf("[PASS] DataFrameChannel append semantics\n");
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
        assert(stmt.sources.size() == 1);
        assert(stmt.sources[0] == "test.data");
        assert(stmt.op_category == "explore");
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
        assert(stmt.sources.size() == 1);
        assert(stmt.sources[0] == "memory_data");
        assert(!stmt.HasOperator());
        assert(stmt.dest == "clickhouse.my_table");
    }

    {
        auto stmt = parser.Parse("SELECT * FROM test.data");
        assert(stmt.error.empty());
        assert(stmt.source == "test.data");
        assert(stmt.sources.size() == 1);
        assert(stmt.sources[0] == "test.data");
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
        assert(stmt.sources.size() == 1);
        assert(stmt.sources[0] == "test.data");
        assert(stmt.dest == "result");
    }

    // Test multi-source FROM parsing
    {
        auto stmt = parser.Parse("SELECT * FROM dataframe.d1, dataframe.d2 USING builtin.concat INTO dataframe.dd");
        assert(stmt.error.empty());
        assert(stmt.source == "dataframe.d1");
        assert(stmt.sources.size() == 2);
        assert(stmt.sources[0] == "dataframe.d1");
        assert(stmt.sources[1] == "dataframe.d2");
        assert(stmt.op_category == "builtin");
        assert(stmt.op_name == "concat");
        assert(stmt.dest == "dataframe.dd");
        assert(stmt.sql_part == "SELECT * FROM dataframe.d1, dataframe.d2");
    }

    // Test multi-source parser error: missing source after FROM
    {
        auto stmt = parser.Parse("SELECT * FROM");
        assert(stmt.error == "expected source channel name after FROM");
    }

    // Test multi-source parser error: missing source after comma
    {
        auto stmt = parser.Parse("SELECT * FROM dataframe.d1,");
        assert(stmt.error == "expected source channel name after ','");
    }

    // Test sql_part extraction with GROUP BY/ORDER BY (Story 4.5)
    {
        auto stmt = parser.Parse("SELECT a, COUNT(*) FROM source GROUP BY a ORDER BY COUNT(*) DESC USING ml.predict");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT a, COUNT(*) FROM source GROUP BY a ORDER BY COUNT(*) DESC");
        assert(stmt.op_category == "ml");
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
        assert(stmt.op_category == "ml");
        assert(stmt.op_name == "train");
        assert(stmt.with_params["epochs"] == "100");
        assert(stmt.dest == "result");
    }

    // Test THEN pipeline with per-operator WITH
    {
        auto stmt = parser.Parse(
            "SELECT * FROM source USING builtin.op1 WITH p1=1,p2=2 THEN ml.op2 WITH p3=3 INTO result");
        assert(stmt.error.empty());
        assert(stmt.operators.size() == 2);
        assert(stmt.operator_with_params.size() == 2);
        assert(stmt.operators[0].category == "builtin");
        assert(stmt.operators[0].name == "op1");
        assert(stmt.operator_with_params[0]["p1"] == "1");
        assert(stmt.operator_with_params[0]["p2"] == "2");
        assert(stmt.operators[1].category == "ml");
        assert(stmt.operators[1].name == "op2");
        assert(stmt.operator_with_params[1]["p3"] == "3");
    }

    // Test pipeline rejects global WITH after USING/THEN chain
    {
        auto stmt = parser.Parse("SELECT * FROM source USING builtin.op1 THEN builtin.op2 WITH p=1 INTO result");
        assert(stmt.error.empty());
        auto bad = parser.Parse("SELECT * FROM source USING builtin.op1 THEN builtin.op2 WITH p=1 WITH q=2 INTO result");
        assert(!bad.error.empty());
    }

    // Test stream selector syntax (Story 14.12)
    {
        auto stmt = parser.Parse(
            "SELECT * FROM stream.npm_hub[*], stream.npm_hub[0] "
            "USING builtin.passthrough_stream INTO stream.npm_sink");
        assert(stmt.error.empty());
        assert(stmt.sources.size() == 2);
        assert(stmt.sources[0] == "stream.npm_hub[*]");
        assert(stmt.sources[1] == "stream.npm_hub[0]");
        assert(stmt.dest == "stream.npm_sink");
    }

    // Test selector parse errors
    {
        auto bad = parser.Parse("SELECT * FROM stream.npm_hub[a] INTO dataframe.out");
        assert(!bad.error.empty());
        auto bad2 = parser.Parse("SELECT * FROM stream.npm_hub[ INTO dataframe.out");
        assert(!bad2.error.empty());
        auto bad3 = parser.Parse("SELECT * FROM stream.npm_hub[0][1] INTO dataframe.out");
        assert(!bad3.error.empty());
        auto ok = parser.Parse("SELECT * FROM stream.npm_hub INTO stream.npm_out[0]");
        assert(ok.error.empty());
        assert(ok.dest == "stream.npm_out[0]");
    }

    printf("[PASS] SQL parser (USING optional + columns)\n");
}

// ============================================================
// Test 7.1: typed filter expression parser
// ============================================================
void test_filter_expression_parser() {
    printf("[TEST] typed filter expression parser...\n");

    {
        std::shared_ptr<FilterExpr> expr;
        std::string error;
        assert(ParseFilterExpression("a = 1 OR b = 2 AND NOT c", &expr, &error));
        assert(error.empty());
        assert(expr && expr->kind == FilterExprKind::kOr && expr->node_id == 1);
        assert(expr->operands.size() == 2);
        assert(expr->operands[0]->kind == FilterExprKind::kCompare);
        assert(expr->operands[0]->compare_op == FilterCompareOp::kEqual);
        assert(expr->operands[0]->node_id == 2);
        assert(expr->operands[0]->operands[0]->field_name == "a");
        assert(expr->operands[0]->operands[0]->node_id == 3);
        assert(expr->operands[0]->operands[1]->literal.kind == FilterLiteralKind::kInteger);
        assert(expr->operands[0]->operands[1]->literal.text == "1");
        assert(expr->operands[1]->kind == FilterExprKind::kAnd);
        assert(expr->operands[1]->node_id == 5);
        assert(expr->operands[1]->operands[1]->kind == FilterExprKind::kNot);
        assert(expr->operands[1]->operands[1]->operands[0]->field_name == "c");
    }

    {
        std::shared_ptr<FilterExpr> expr;
        std::string error;
        assert(ParseFilterExpression("src_port NOT IN (80, 443)", &expr, &error));
        assert(expr && expr->kind == FilterExprKind::kNot);
        assert(expr->operands.size() == 1);
        const auto& in = expr->operands[0];
        assert(in->kind == FilterExprKind::kIn && in->operands.size() == 3);
        assert(in->operands[0]->field_name == "src_port");
        assert(in->operands[1]->literal.text == "80");
        assert(in->operands[2]->literal.text == "443");
    }

    {
        std::shared_ptr<FilterExpr> expr;
        std::string error;
        assert(ParseFilterExpression(
            "captured_len NOT BETWEEN -1.5e2 AND 1500", &expr, &error));
        assert(expr && expr->kind == FilterExprKind::kNot);
        const auto& between = expr->operands[0];
        assert(between->kind == FilterExprKind::kBetween && between->operands.size() == 3);
        assert(between->operands[1]->literal.kind == FilterLiteralKind::kFloating);
        assert(between->operands[1]->literal.text == "-1.5e2");
        assert(between->operands[2]->literal.kind == FilterLiteralKind::kInteger);
    }

    {
        std::shared_ptr<FilterExpr> expr;
        std::string error;
        assert(ParseFilterExpression(
            "message = 'can''t use INTO' AND HAS_LAYER('tcp')", &expr, &error));
        assert(expr && expr->kind == FilterExprKind::kAnd);
        const auto& compare = expr->operands[0];
        assert(compare->operands[1]->literal.kind == FilterLiteralKind::kString);
        assert(compare->operands[1]->literal.text == "can't use INTO");
        const auto& call = expr->operands[1];
        assert(call->kind == FilterExprKind::kCall);
        assert(call->function_name == "has_layer");
        assert(call->operands.size() == 1);
        assert(call->operands[0]->literal.text == "tcp");
    }

    {
        std::shared_ptr<FilterExpr> expr;
        std::string error;
        assert(ParseFilterExpression(
            "timestamp_ns >= TiMeStAmP '2026-09-08T09:30:00.123456789+08:00'",
            &expr, &error));
        assert(error.empty());
        assert(expr && expr->kind == FilterExprKind::kCompare);
        assert(expr->compare_op == FilterCompareOp::kGreaterEqual);
        assert(expr->operands[0]->field_name == "timestamp_ns");
        assert(expr->operands[1]->literal.kind == FilterLiteralKind::kTyped);
        assert(expr->operands[1]->literal.type_name == "timestamp");
        assert(expr->operands[1]->literal.text ==
               "2026-09-08T09:30:00.123456789+08:00");

        assert(ParseFilterExpression(
            "timestamp_ns BETWEEN TIMESTAMP '2026-09-08T01:30:00Z' "
            "AND TIMESTAMP 'not-validated-by-parser'",
            &expr, &error));
        assert(expr && expr->kind == FilterExprKind::kBetween);
        assert(expr->operands[1]->literal.kind == FilterLiteralKind::kTyped);
        assert(expr->operands[1]->literal.type_name == "timestamp");
        assert(expr->operands[1]->literal.text == "2026-09-08T01:30:00Z");
        assert(expr->operands[2]->literal.kind == FilterLiteralKind::kTyped);
        assert(expr->operands[2]->literal.type_name == "timestamp");
        assert(expr->operands[2]->literal.text == "not-validated-by-parser");

        assert(ParseFilterExpression(
            "request_id = UUID '123e4567-e89b-12d3-a456-426614174000'", &expr, &error));
        assert(expr && expr->kind == FilterExprKind::kCompare);
        assert(expr->operands[1]->literal.kind == FilterLiteralKind::kTyped);
        assert(expr->operands[1]->literal.type_name == "uuid");
        assert(expr->operands[1]->literal.text ==
               "123e4567-e89b-12d3-a456-426614174000");
    }

    {
        const std::vector<std::pair<std::string, std::string>> packet_calls = {
            {"TCP('192.0.2.10', 52314, '198.51.100.20', 3389)", "tcp"},
            {"udp('2001:db8::10', 53000, '2001:db8::20', 53)", "udp"},
        };
        for (const auto& item : packet_calls) {
            std::shared_ptr<FilterExpr> expr;
            std::string error;
            assert(ParseFilterExpression(item.first, &expr, &error));
            assert(error.empty());
            assert(expr && expr->kind == FilterExprKind::kCall);
            assert(expr->function_name == item.second);
            assert(expr->operands.size() == 4);
            assert(expr->operands[0]->literal.kind == FilterLiteralKind::kString);
            assert(expr->operands[1]->literal.kind == FilterLiteralKind::kInteger);
            assert(expr->operands[2]->literal.kind == FilterLiteralKind::kString);
            assert(expr->operands[3]->literal.kind == FilterLiteralKind::kInteger);
        }
    }

    {
        std::shared_ptr<FilterExpr> expr;
        std::string error;
        assert(ParseFilterExpression("src_port IS NOT NULL", &expr, &error));
        assert(expr && expr->kind == FilterExprKind::kNot);
        assert(expr->operands[0]->kind == FilterExprKind::kIsNull);
        assert(expr->operands[0]->operands[0]->field_name == "src_port");

        assert(ParseFilterExpression("protocol = HTTP", &expr, &error));
        assert(expr->kind == FilterExprKind::kCompare);
        assert(expr->operands[1]->kind == FilterExprKind::kField);
        assert(expr->operands[1]->field_name == "HTTP");
    }

    {
        const std::vector<std::pair<std::string, FilterCompareOp>> comparisons = {
            {"a = 1", FilterCompareOp::kEqual},
            {"a != 1", FilterCompareOp::kNotEqual},
            {"a < 1", FilterCompareOp::kLess},
            {"a <= 1", FilterCompareOp::kLessEqual},
            {"a > 1", FilterCompareOp::kGreater},
            {"a >= 1", FilterCompareOp::kGreaterEqual},
        };
        for (const auto& item : comparisons) {
            std::shared_ptr<FilterExpr> expr;
            std::string error;
            assert(ParseFilterExpression(item.first, &expr, &error));
            assert(expr->kind == FilterExprKind::kCompare);
            assert(expr->compare_op == item.second);
        }
    }

    {
        std::shared_ptr<FilterExpr> expr;
        std::string error;
        assert(ParseFilterExpression(
            "1 < captured_len AND wire_len >= captured_len AND ports_valid = TRUE", &expr, &error));
        assert(expr && expr->kind == FilterExprKind::kAnd);
        const auto& right = expr->operands[1];
        assert(right->kind == FilterExprKind::kCompare);
        assert(right->operands[1]->literal.kind == FilterLiteralKind::kBoolean);
        assert(right->operands[1]->literal.text == "TRUE");

        assert(ParseFilterExpression("src_port IS NULL", &expr, &error));
        assert(expr->kind == FilterExprKind::kIsNull);

        assert(ParseFilterExpression("UNKNOWN_FUNCTION(field, FALSE)", &expr, &error));
        assert(expr->kind == FilterExprKind::kCall);
        assert(expr->function_name == "unknown_function");
        assert(expr->operands[0]->kind == FilterExprKind::kField);
        assert(expr->operands[1]->literal.kind == FilterLiteralKind::kBoolean);
    }

    const std::vector<std::string> invalid = {
        "ipv4 & tcp",
        "protocol == 'HTTP'",
        "src_port = NULL",
        "0 < captured_len < 1500",
        "src_port IN ()",
        "(src_port = 80",
        "message = \"HTTP\"",
        "1 = 1",
        "src_port IN (80, NULL)",
        "src_port IN (80, other_port)",
        "src_port BETWEEN lower AND 100",
        "TIMESTAMP '2026-09-08T01:30:00Z'",
    };
    for (const auto& input : invalid) {
        std::shared_ptr<FilterExpr> expr;
        std::string error;
        assert(!ParseFilterExpression(input, &expr, &error));
        assert(!expr && !error.empty());
    }

    printf("[PASS] typed filter expression parser\n");
}

// ============================================================
// Test 7.2: filter Schema binding
// ============================================================
void test_filter_schema_binding() {
    printf("[TEST] filter Schema binding...\n");
    auto schema = arrow::schema({
        arrow::field("id", arrow::int32(), false),
        arrow::field("small", arrow::uint16(), true),
        arrow::field("score", arrow::float64(), false),
        arrow::field("name", arrow::utf8(), true),
        arrow::field("enabled", arrow::boolean(), true),
        arrow::field("peer", arrow::int64(), false),
        arrow::field("layers", arrow::fixed_size_list(arrow::uint16(), 4), false),
    });

    auto bind = [&](const std::string& text,
                    std::shared_ptr<const BoundFilterExpr>* output,
                    std::string* error) {
        std::shared_ptr<FilterExpr> expression;
        std::string parse_error;
        assert(ParseFilterExpression(text, &expression, &parse_error));
        return BindFilterExpression(schema, expression, output, error);
    };

    {
        std::shared_ptr<const BoundFilterExpr> bound;
        std::string error;
        assert(bind("id >= 42 AND enabled", &bound, &error) == FilterBindError::kNone);
        assert(error.empty());
        assert(bound && bound->kind == FilterExprKind::kAnd);
        assert(bound->value_type->id() == arrow::Type::BOOL && bound->nullable);
        const auto& compare = bound->operands[0];
        assert(compare->node_id == 2 && compare->kind == FilterExprKind::kCompare);
        assert(compare->comparison_type->id() == arrow::Type::INT32);
        assert(compare->operands[0]->field_index == 0);
        assert(compare->operands[1]->literal->type->id() == arrow::Type::INT32);
        assert(std::static_pointer_cast<arrow::Int32Scalar>(
                   compare->operands[1]->literal)->value == 42);
        assert(bound->operands[1]->field_index == 4);
    }

    {
        std::shared_ptr<const BoundFilterExpr> bound;
        std::string error;
        assert(bind("small IN (1, 65535)", &bound, &error) == FilterBindError::kNone);
        assert(bound->kind == FilterExprKind::kIn && bound->nullable);
        assert(bound->comparison_type->id() == arrow::Type::UINT16);
        assert(bound->operands[0]->field_index == 1);
        assert(std::static_pointer_cast<arrow::UInt16Scalar>(
                   bound->operands[2]->literal)->value == 65535);

        assert(bind("score BETWEEN -1.5 AND 2", &bound, &error) ==
               FilterBindError::kNone);
        assert(bound->kind == FilterExprKind::kBetween);
        assert(bound->comparison_type->id() == arrow::Type::DOUBLE);
        assert(bound->operands[1]->literal->type->id() == arrow::Type::DOUBLE);

        assert(bind("name = 'HTTP'", &bound, &error) == FilterBindError::kNone);
        assert(bound->nullable);
        assert(bound->operands[1]->literal->ToString() == "HTTP");
    }

    {
        std::shared_ptr<const BoundFilterExpr> bound;
        std::string error;
        assert(bind("1 < id", &bound, &error) == FilterBindError::kNone);
        assert(bound->operands[0]->literal->type->id() == arrow::Type::INT32);
        assert(bound->operands[1]->field_index == 0);

        assert(bind("id < peer", &bound, &error) == FilterBindError::kNone);
        assert(bound->comparison_type->id() == arrow::Type::INT64);

        assert(bind("small < score", &bound, &error) == FilterBindError::kNone);
        assert(bound->comparison_type->id() == arrow::Type::DOUBLE);

        assert(bind("enabled IS NULL", &bound, &error) == FilterBindError::kNone);
        assert(bound->kind == FilterExprKind::kIsNull);
        assert(!bound->nullable && bound->value_type->id() == arrow::Type::BOOL);
    }

    const std::vector<std::pair<std::string, FilterBindError>> invalid = {
        {"missing = 1", FilterBindError::kUnknownField},
        {"ID = 1", FilterBindError::kUnknownField},
        {"id", FilterBindError::kNonBooleanRoot},
        {"small = 65536", FilterBindError::kLiteralOutOfRange},
        {"small = -1", FilterBindError::kLiteralOutOfRange},
        {"name = id", FilterBindError::kTypeMismatch},
        {"peer < score", FilterBindError::kTypeMismatch},
        {"layers = layers", FilterBindError::kUnsupportedType},
        {"enabled > TRUE", FilterBindError::kTypeMismatch},
        {"has_layer('tcp')", FilterBindError::kUnknownFunction},
    };
    for (const auto& item : invalid) {
        std::shared_ptr<const BoundFilterExpr> bound =
            std::make_shared<BoundFilterExpr>();
        std::string error;
        assert(bind(item.first, &bound, &error) == item.second);
        assert(!bound && !error.empty());
    }

    {
        auto duplicate_schema = arrow::schema({
            arrow::field("dup", arrow::int32()),
            arrow::field("dup", arrow::int32()),
        });
        std::shared_ptr<FilterExpr> expression;
        std::string error;
        assert(ParseFilterExpression("dup = 1", &expression, &error));
        std::shared_ptr<const BoundFilterExpr> bound;
        assert(BindFilterExpression(duplicate_schema, expression, &bound, &error) ==
               FilterBindError::kAmbiguousField);
        assert(!bound && !error.empty());
    }

    {
        auto malformed = std::make_shared<FilterExpr>();
        malformed->kind = FilterExprKind::kCompare;
        malformed->compare_op = FilterCompareOp::kEqual;
        malformed->operands.push_back(std::make_shared<FilterExpr>());
        std::shared_ptr<const BoundFilterExpr> bound;
        std::string error;
        assert(BindFilterExpression(schema, malformed, &bound, &error) ==
               FilterBindError::kInvalidAst);
        assert(!bound && !error.empty());
        assert(BindFilterExpression(nullptr, malformed, &bound, &error) ==
               FilterBindError::kInvalidArgument);
    }

    printf("[PASS] filter Schema binding\n");
}

// ============================================================
// Test 7.3: bound filter Arrow Boolean mask evaluation
// ============================================================
void test_filter_mask_evaluation() {
    printf("[TEST] filter Boolean mask evaluation...\n");
    auto schema = arrow::schema({
        arrow::field("id", arrow::int32(), true),
        arrow::field("peer", arrow::int64(), false),
        arrow::field("name", arrow::utf8(), true),
        arrow::field("enabled", arrow::boolean(), true),
    });

    arrow::Int32Builder id_builder;
    assert(id_builder.Append(1).ok());
    assert(id_builder.Append(2).ok());
    assert(id_builder.AppendNull().ok());
    assert(id_builder.Append(4).ok());
    std::shared_ptr<arrow::Array> ids;
    assert(id_builder.Finish(&ids).ok());

    arrow::Int64Builder peer_builder;
    assert(peer_builder.Append(2).ok());
    assert(peer_builder.Append(2).ok());
    assert(peer_builder.Append(3).ok());
    assert(peer_builder.Append(5).ok());
    std::shared_ptr<arrow::Array> peers;
    assert(peer_builder.Finish(&peers).ok());

    arrow::StringBuilder name_builder;
    assert(name_builder.Append("HTTP").ok());
    assert(name_builder.Append("DNS").ok());
    assert(name_builder.AppendNull().ok());
    assert(name_builder.Append("HTTP").ok());
    std::shared_ptr<arrow::Array> names;
    assert(name_builder.Finish(&names).ok());

    arrow::BooleanBuilder enabled_builder;
    assert(enabled_builder.Append(true).ok());
    assert(enabled_builder.AppendNull().ok());
    assert(enabled_builder.Append(false).ok());
    assert(enabled_builder.Append(true).ok());
    std::shared_ptr<arrow::Array> enabled;
    assert(enabled_builder.Finish(&enabled).ok());

    auto batch = arrow::RecordBatch::Make(schema, 4, {ids, peers, names, enabled});
    auto bind = [&](const std::string& text) {
        std::shared_ptr<FilterExpr> parsed;
        std::string error;
        assert(ParseFilterExpression(text, &parsed, &error));
        std::shared_ptr<const BoundFilterExpr> bound;
        assert(BindFilterExpression(schema, parsed, &bound, &error) ==
               FilterBindError::kNone);
        assert(bound && error.empty());
        return bound;
    };
    auto evaluate = [&](const std::string& text,
                        const std::shared_ptr<arrow::RecordBatch>& input,
                        std::shared_ptr<arrow::BooleanArray>* mask,
                        std::string* error) {
        return EvaluateFilterMask(input, bind(text), mask, error);
    };
    auto expect_mask = [](const std::shared_ptr<arrow::BooleanArray>& mask,
                          const std::vector<int>& expected) {
        assert(mask && mask->length() == static_cast<int64_t>(expected.size()));
        for (int64_t i = 0; i < mask->length(); ++i) {
            if (expected[static_cast<size_t>(i)] < 0) {
                assert(mask->IsNull(i));
            } else {
                assert(!mask->IsNull(i));
                assert(mask->Value(i) == (expected[static_cast<size_t>(i)] != 0));
            }
        }
    };

    const std::vector<std::pair<std::string, std::vector<int>>> comparisons = {
        {"id = 2", {0, 1, -1, 0}},
        {"id != 2", {1, 0, -1, 1}},
        {"id < 2", {1, 0, -1, 0}},
        {"id <= 2", {1, 1, -1, 0}},
        {"id > 2", {0, 0, -1, 1}},
        {"id >= 2", {0, 1, -1, 1}},
        {"1 < id", {0, 1, -1, 1}},
        {"id < peer", {1, 0, -1, 1}},
    };
    for (const auto& item : comparisons) {
        std::shared_ptr<arrow::BooleanArray> mask;
        std::string error;
        assert(evaluate(item.first, batch, &mask, &error) == FilterEvalError::kNone);
        assert(error.empty());
        expect_mask(mask, item.second);
    }

    {
        std::shared_ptr<arrow::BooleanArray> mask;
        std::string error;
        assert(evaluate("name IN ('HTTP', 'TLS')", batch, &mask, &error) ==
               FilterEvalError::kNone);
        expect_mask(mask, {1, 0, -1, 1});

        assert(evaluate("id BETWEEN 2 AND 4", batch, &mask, &error) ==
               FilterEvalError::kNone);
        expect_mask(mask, {0, 1, -1, 1});

        assert(evaluate("name IS NULL", batch, &mask, &error) ==
               FilterEvalError::kNone);
        expect_mask(mask, {0, 0, 1, 0});
    }

    {
        std::shared_ptr<arrow::BooleanArray> mask;
        std::string error;
        assert(evaluate("enabled AND id > 1", batch, &mask, &error) ==
               FilterEvalError::kNone);
        expect_mask(mask, {0, -1, 0, 1});

        assert(evaluate("enabled OR id > 1", batch, &mask, &error) ==
               FilterEvalError::kNone);
        expect_mask(mask, {1, 1, -1, 1});

        assert(evaluate("NOT enabled", batch, &mask, &error) ==
               FilterEvalError::kNone);
        expect_mask(mask, {0, -1, 1, 0});
    }

    {
        std::shared_ptr<arrow::BooleanArray> mask;
        std::string error;
        assert(evaluate("id = 1", batch->Slice(0, 0), &mask, &error) ==
               FilterEvalError::kNone);
        assert(mask && mask->type_id() == arrow::Type::BOOL && mask->length() == 0);
    }

    {
        arrow::Int64Builder wrong_id_builder;
        assert(wrong_id_builder.AppendValues({1, 2, 3, 4}).ok());
        std::shared_ptr<arrow::Array> wrong_ids;
        assert(wrong_id_builder.Finish(&wrong_ids).ok());
        auto wrong_schema = arrow::schema({
            arrow::field("id", arrow::int64(), false),
            schema->field(1),
            schema->field(2),
            schema->field(3),
        });
        auto wrong_batch = arrow::RecordBatch::Make(
            wrong_schema, 4, {wrong_ids, peers, names, enabled});
        auto mask = std::make_shared<arrow::BooleanArray>(0, nullptr);
        std::string error;
        assert(EvaluateFilterMask(wrong_batch, bind("id = 1"), &mask, &error) ==
               FilterEvalError::kSchemaMismatch);
        assert(!mask && !error.empty());
    }

    {
        auto malformed = std::make_shared<BoundFilterExpr>();
        malformed->kind = FilterExprKind::kAnd;
        malformed->node_id = 1;
        malformed->value_type = arrow::boolean();
        std::shared_ptr<arrow::BooleanArray> mask;
        std::string error;
        assert(EvaluateFilterMask(batch, malformed, &mask, &error) ==
               FilterEvalError::kInvalidBoundExpression);
        assert(!mask && !error.empty());

        auto function = std::make_shared<BoundFilterExpr>();
        function->kind = FilterExprKind::kCall;
        function->node_id = 2;
        function->value_type = arrow::boolean();
        assert(EvaluateFilterMask(batch, function, &mask, &error) ==
               FilterEvalError::kUnsupportedFunction);
        assert(!mask && !error.empty());

        assert(EvaluateFilterMask(nullptr, function, &mask, &error) ==
               FilterEvalError::kInvalidArgument);
        assert(!mask && !error.empty());
    }

    printf("[PASS] filter Boolean mask evaluation\n");
}

// ============================================================
// Test 7.4: packet RecordBatch filtering and data invariants
// ============================================================
void test_filter_record_batch() {
    printf("[TEST] packet RecordBatch filtering...\n");

    std::vector<std::shared_ptr<std::vector<uint8_t>>> byte_owners;
    std::vector<packet::PacketRecord> records;
    for (size_t row = 0; row < 4; ++row) {
        auto bytes = std::make_shared<std::vector<uint8_t>>(32 + row);
        for (size_t index = 0; index < bytes->size(); ++index) {
            (*bytes)[index] = static_cast<uint8_t>(row * 0x20 + index);
        }
        byte_owners.push_back(bytes);

        packet::PacketRecord record;
        record.meta.timestamp_ns = static_cast<int64_t>(1000 + row);
        record.meta.captured_len = static_cast<uint32_t>(bytes->size());
        record.meta.wire_len = static_cast<uint32_t>(bytes->size());
        record.meta.link_type = 1;
        record.meta.source_id = 7;
        record.meta.sequence = 100 + row;
        record.raw_data.owner = bytes;
        record.raw_data.data = bytes->data();
        record.raw_data.size = static_cast<uint32_t>(bytes->size());
        record.layer.status = packet::LayerStatus::kDecoded;
        record.layer.layer_count = 2;
        record.layer.layers[0] = {static_cast<uint16_t>(10 + row), 0};
        record.layer.layers[1] = {static_cast<uint16_t>(20 + row), 14};
        record.layer.network_layer_index = 0;
        record.layer.transport_layer_index = 1;
        record.layer.payload_offset = 14;
        record.layer.src_mac.valid = 1;
        for (size_t index = 0; index < sizeof(record.layer.src_mac.value.bytes); ++index) {
            record.layer.src_mac.value.bytes[index] =
                static_cast<uint8_t>(0xa0 + row * 8 + index);
        }
        record.layer.transport_protocol = 6;
        record.layer.ports_valid = row == 1 ? 0 : 1;
        record.layer.src_port = row == 2 ? 80 : 443;
        record.layer.dst_port = static_cast<uint16_t>(5000 + row);
        records.push_back(std::move(record));
    }

    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    assert(packet::EncodePacketBatch(records, &batch, &error) ==
           packet::PacketBatchError::kNone);
    assert(batch && batch->num_rows() == 4 && error.empty());

    auto bind = [&](const std::string& text) {
        std::shared_ptr<FilterExpr> parsed;
        std::string bind_error;
        assert(ParseFilterExpression(text, &parsed, &bind_error));
        std::shared_ptr<const BoundFilterExpr> bound;
        assert(BindFilterExpression(batch->schema(), parsed, &bound, &bind_error) ==
               FilterBindError::kNone);
        assert(bound && bind_error.empty());
        return bound;
    };

    std::shared_ptr<arrow::RecordBatch> filtered;
    assert(FilterRecordBatch(batch, bind("src_port = 443"), &filtered, &error) ==
           FilterEvalError::kNone);
    assert(error.empty());
    assert(filtered && filtered->num_rows() == 2 && filtered->num_columns() == 30);
    assert(filtered->schema()->Equals(*batch->schema(), true));
    assert(filtered->schema()->metadata()->Get("flowsql.entity").ValueOrDie() ==
           "packet");
    assert(filtered->schema()->metadata()->Get("flowsql.schema_version").ValueOrDie() ==
           "1");

    auto sequences = std::static_pointer_cast<arrow::UInt64Array>(
        filtered->GetColumnByName("sequence"));
    assert(sequences->Value(0) == 100);
    assert(sequences->Value(1) == 103);

    auto raw = std::static_pointer_cast<arrow::BinaryArray>(
        filtered->GetColumnByName("raw_data"));
    assert(raw->GetView(0) == std::string_view(
                                  reinterpret_cast<const char*>(byte_owners[0]->data()),
                                  byte_owners[0]->size()));
    assert(raw->GetView(1) == std::string_view(
                                  reinterpret_cast<const char*>(byte_owners[3]->data()),
                                  byte_owners[3]->size()));

    auto src_mac = std::static_pointer_cast<arrow::FixedSizeBinaryArray>(
        filtered->GetColumnByName("src_mac"));
    assert(!src_mac->IsNull(0) && !src_mac->IsNull(1));
    assert(std::memcmp(src_mac->GetValue(0), records[0].layer.src_mac.value.bytes, 6) == 0);
    assert(std::memcmp(src_mac->GetValue(1), records[3].layer.src_mac.value.bytes, 6) == 0);

    auto layer_ids = std::static_pointer_cast<arrow::FixedSizeListArray>(
        filtered->GetColumnByName("layer_ids"));
    auto layer_id_values = std::static_pointer_cast<arrow::UInt16Array>(
        layer_ids->values());
    const auto first_ids = layer_ids->value_offset(0);
    const auto second_ids = layer_ids->value_offset(1);
    assert(layer_id_values->Value(first_ids) == 10);
    assert(layer_id_values->Value(first_ids + 1) == 20);
    assert(layer_id_values->Value(second_ids) == 13);
    assert(layer_id_values->Value(second_ids + 1) == 23);

    auto layer_offsets = std::static_pointer_cast<arrow::FixedSizeListArray>(
        filtered->GetColumnByName("layer_offsets"));
    auto layer_offset_values = std::static_pointer_cast<arrow::UInt32Array>(
        layer_offsets->values());
    const auto first_offsets = layer_offsets->value_offset(0);
    const auto second_offsets = layer_offsets->value_offset(1);
    assert(layer_offset_values->Value(first_offsets) == 0);
    assert(layer_offset_values->Value(first_offsets + 1) == 14);
    assert(layer_offset_values->Value(second_offsets) == 0);
    assert(layer_offset_values->Value(second_offsets + 1) == 14);

    assert(FilterRecordBatch(batch, bind("captured_len > 1000"), &filtered, &error) ==
           FilterEvalError::kNone);
    assert(filtered && filtered->num_rows() == 0);
    assert(filtered->schema()->Equals(*batch->schema(), true));

    std::shared_ptr<arrow::RecordBatch> empty_batch;
    assert(packet::EncodePacketBatch({}, &empty_batch, &error) ==
           packet::PacketBatchError::kNone);
    assert(FilterRecordBatch(empty_batch, bind("ports_valid"), &filtered, &error) ==
           FilterEvalError::kNone);
    assert(filtered && filtered->num_rows() == 0);
    assert(filtered->schema()->Equals(*batch->schema(), true));

    filtered = batch;
    assert(FilterRecordBatch(nullptr, bind("ports_valid"), &filtered, &error) ==
           FilterEvalError::kInvalidArgument);
    assert(!filtered && !error.empty());

    printf("[PASS] packet RecordBatch filtering\n");
}

// ============================================================
// Test 7.5: reusable runtime residual Filter Stage
// ============================================================
void test_block_filter_stage() {
    printf("[TEST] block residual Filter Stage...\n");
    auto metadata = arrow::key_value_metadata({"flowsql.test"}, {"filter-stage"});
    auto schema = arrow::schema({
        arrow::field("id", arrow::int32(), true),
        arrow::field("enabled", arrow::boolean(), true),
    }, metadata);

    arrow::Int32Builder id_builder;
    assert(id_builder.Append(1).ok());
    assert(id_builder.Append(2).ok());
    assert(id_builder.AppendNull().ok());
    assert(id_builder.Append(4).ok());
    std::shared_ptr<arrow::Array> ids;
    assert(id_builder.Finish(&ids).ok());
    arrow::BooleanBuilder enabled_builder;
    assert(enabled_builder.Append(true).ok());
    assert(enabled_builder.Append(true).ok());
    assert(enabled_builder.Append(true).ok());
    assert(enabled_builder.AppendNull().ok());
    std::shared_ptr<arrow::Array> enabled;
    assert(enabled_builder.Finish(&enabled).ok());
    auto batch = arrow::RecordBatch::Make(schema, 4, {ids, enabled});

    auto bind = [&](const std::string& text) {
        std::shared_ptr<FilterExpr> parsed;
        std::string bind_error;
        assert(ParseFilterExpression(text, &parsed, &bind_error));
        std::shared_ptr<const BoundFilterExpr> bound;
        assert(BindFilterExpression(schema, parsed, &bound, &bind_error) ==
               FilterBindError::kNone);
        assert(bound && bind_error.empty());
        return bound;
    };

    BlockFilterStage filtered(bind("id >= 2 AND enabled"));
    BlockTransformOutputV1 output{batch, 999};
    std::string error = "stale";
    assert(filtered.ProcessBlock(batch, 123, &output, &error) ==
           FilterEvalError::kInvalidArgument);
    assert(!output.batch && output.ts_ms == 0 && !error.empty());

    std::shared_ptr<arrow::Schema> output_schema;
    assert(filtered.Open(schema, &output_schema, &error) == FilterEvalError::kNone);
    assert(filtered.IsOpen() && output_schema == schema && error.empty());
    output_schema = schema;
    assert(filtered.Open(schema, &output_schema, &error) ==
           FilterEvalError::kInvalidArgument);
    assert(!output_schema && !error.empty());

    assert(filtered.ProcessBlock(batch, 123, &output, &error) == FilterEvalError::kNone);
    assert(error.empty() && output.batch && output.ts_ms == 123);
    assert(output.batch->num_rows() == 1);
    assert(output.batch->schema()->Equals(*schema, true));
    auto filtered_ids = std::static_pointer_cast<arrow::Int32Array>(
        output.batch->GetColumnByName("id"));
    assert(filtered_ids->Value(0) == 2);

    auto wrong_schema = arrow::schema({
        arrow::field("id", arrow::int64(), true),
        arrow::field("enabled", arrow::boolean(), true),
    });
    auto wrong_ids = std::make_shared<arrow::Int64Array>(0, nullptr);
    auto wrong_enabled = std::make_shared<arrow::BooleanArray>(0, nullptr);
    auto wrong_batch = arrow::RecordBatch::Make(
        wrong_schema, 0, {wrong_ids, wrong_enabled});
    output = {batch, 999};
    assert(filtered.ProcessBlock(wrong_batch, 456, &output, &error) ==
           FilterEvalError::kSchemaMismatch);
    assert(!output.batch && output.ts_ms == 0 && !error.empty());

    BlockFilterStage all_filtered(bind("id > 100"));
    assert(all_filtered.Open(schema, &output_schema, &error) == FilterEvalError::kNone);
    assert(all_filtered.ProcessBlock(batch, 789, &output, &error) ==
           FilterEvalError::kNone);
    assert(output.batch && output.batch->num_rows() == 0 && output.ts_ms == 789);
    assert(output.batch->schema()->Equals(*schema, true));

    BlockFilterStage passthrough(nullptr);
    assert(passthrough.Open(schema, &output_schema, &error) == FilterEvalError::kNone);
    assert(passthrough.ProcessBlock(batch, 321, &output, &error) ==
           FilterEvalError::kNone);
    assert(output.batch == batch && output.ts_ms == 321 && error.empty());

    auto unsupported_call = std::make_shared<BoundFilterExpr>();
    unsupported_call->kind = FilterExprKind::kCall;
    unsupported_call->node_id = 1;
    unsupported_call->value_type = arrow::boolean();
    BlockFilterStage unsupported(unsupported_call);
    output_schema = schema;
    assert(unsupported.Open(schema, &output_schema, &error) ==
           FilterEvalError::kUnsupportedFunction);
    assert(!unsupported.IsOpen() && !output_schema && !error.empty());

    printf("[PASS] block residual Filter Stage\n");
}

class ScriptedBlockSource final : public IBlockStreamChannel {
 public:
    explicit ScriptedBlockSource(std::vector<BlockPollEvent> events,
                                 std::vector<std::string>* log = nullptr)
        : events_(std::move(events)), log_(log) {}

    const char* Category() override { return "test"; }
    const char* Name() override { return "scripted_block_source"; }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return ""; }
    int Open() override {
        opened_ = true;
        return 0;
    }
    int Close() override {
        opened_ = false;
        return 0;
    }
    bool IsOpened() const override { return opened_; }
    int Flush() override { return 0; }

    BlockPollEvent PollBlock(int timeout_ms) override {
        observed_timeout_ms = timeout_ms;
        ++poll_calls;
        if (log_) log_->push_back("poll-" + std::to_string(poll_calls));
        if (throw_on_poll) throw std::runtime_error("scripted poll exception");
        if (next_event_ < events_.size()) return events_[next_event_++];
        return {BlockPollEvent::kEof, nullptr, 0};
    }

    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& block) override {
        ++release_calls;
        released_blocks.push_back(block);
        if (log_) log_->push_back("release-" + std::to_string(release_calls));
        if (throw_on_release) throw std::runtime_error("scripted release exception");
        return release_return_code;
    }

    void Cancel() override {
        ++cancel_calls;
        if (log_) log_->push_back("source-cancel");
    }
    bool IsFinished() const override { return next_event_ >= events_.size(); }

    int observed_timeout_ms = -1;
    int poll_calls = 0;
    int release_calls = 0;
    int cancel_calls = 0;
    int release_return_code = 0;
    bool throw_on_poll = false;
    bool throw_on_release = false;
    std::vector<std::shared_ptr<arrow::RecordBatch>> released_blocks;

 private:
    std::vector<BlockPollEvent> events_;
    std::vector<std::string>* log_ = nullptr;
    size_t next_event_ = 0;
    bool opened_ = true;
};

class ScriptedBlockTransformTask final : public IBlockTransformTaskV1 {
 public:
    struct ProcessStep {
        int return_code = e2i(BlockTransformStatusV1::kContinue);
        std::vector<BlockTransformOutputV1> outputs;
    };

    explicit ScriptedBlockTransformTask(std::shared_ptr<arrow::Schema> output_schema,
                                        std::vector<std::string>* log = nullptr)
        : output_schema_(std::move(output_schema)), log_(log) {}

    int Open(std::shared_ptr<arrow::Schema> input_schema,
             std::shared_ptr<arrow::Schema>* output_schema) override {
        ++open_calls;
        opened_input_schema = std::move(input_schema);
        if (log_) log_->push_back("transform-open");
        if (!output_schema) return EINVAL;
        output_schema->reset();
        if (throw_on_open) throw std::runtime_error("scripted open exception");
        if (open_return_code != 0) return open_return_code;
        *output_schema = output_schema_;
        return 0;
    }

    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input,
                     int64_t ts_ms,
                     std::vector<BlockTransformOutputV1>* outputs) override {
        ++process_calls;
        if (log_) log_->push_back("process-" + std::to_string(process_calls));
        assert(outputs && outputs->empty());
        process_inputs.push_back(input);
        process_timestamps.push_back(ts_ms);
        if (throw_on_process) throw std::runtime_error("scripted process exception");
        if (next_process_step >= process_steps.size()) return EIO;
        const auto& step = process_steps[next_process_step++];
        *outputs = step.outputs;
        return step.return_code;
    }

    int Flush(std::vector<BlockTransformOutputV1>* outputs) override {
        ++flush_calls;
        if (log_) log_->push_back("flush");
        assert(outputs && outputs->empty());
        if (throw_on_flush) throw std::runtime_error("scripted flush exception");
        *outputs = flush_outputs;
        return flush_return_code;
    }

    void Cancel() override {
        ++cancel_calls;
        if (log_) log_->push_back("transform-cancel");
    }
    std::string LastError() const override { return last_error; }

    std::vector<ProcessStep> process_steps;
    std::vector<BlockTransformOutputV1> flush_outputs;
    int open_return_code = 0;
    int flush_return_code = 0;
    bool throw_on_open = false;
    bool throw_on_process = false;
    bool throw_on_flush = false;
    std::string last_error = "scripted transform failed";
    int open_calls = 0;
    int process_calls = 0;
    int flush_calls = 0;
    int cancel_calls = 0;
    size_t next_process_step = 0;
    std::shared_ptr<arrow::Schema> opened_input_schema;
    std::vector<std::shared_ptr<arrow::RecordBatch>> process_inputs;
    std::vector<int64_t> process_timestamps;

 private:
    std::shared_ptr<arrow::Schema> output_schema_;
    std::vector<std::string>* log_ = nullptr;
};

// ============================================================
// Test 7.6: single-transform block pipeline lifecycle
// ============================================================
void test_block_transform_pipeline_runner() {
    printf("[TEST] block transform pipeline runner...\n");
    auto schema = arrow::schema({arrow::field("id", arrow::int32(), true)});
    auto make_batch = [&](const std::vector<int32_t>& values) {
        arrow::Int32Builder builder;
        assert(builder.AppendValues(values).ok());
        std::shared_ptr<arrow::Array> array;
        assert(builder.Finish(&array).ok());
        return arrow::RecordBatch::Make(
            schema, static_cast<int64_t>(values.size()), {array});
    };
    auto bind = [&](const std::string& text) {
        std::shared_ptr<FilterExpr> parsed;
        std::string bind_error;
        assert(ParseFilterExpression(text, &parsed, &bind_error));
        std::shared_ptr<const BoundFilterExpr> bound;
        assert(BindFilterExpression(schema, parsed, &bound, &bind_error) ==
               FilterBindError::kNone);
        return bound;
    };

    const auto first_input = make_batch({1, 2});
    const auto second_input = make_batch({3});
    const auto output_three = make_batch({3});
    const auto output_two = make_batch({2});
    const auto output_four = make_batch({4});
    std::vector<std::string> log;
    ScriptedBlockSource source({
        {BlockPollEvent::kTimeout, nullptr, 0},
        {BlockPollEvent::kData, first_input, 0},
        {BlockPollEvent::kData, second_input, 0},
        {BlockPollEvent::kEof, nullptr, 0},
    }, &log);
    ScriptedBlockTransformTask transform(schema, &log);
    transform.process_steps = {
        {e2i(BlockTransformStatusV1::kContinue), {}},
        {e2i(BlockTransformStatusV1::kContinue), {{output_three, 201}, {output_two, 202}}},
    };
    transform.flush_outputs = {{output_four, 300}};
    std::vector<BlockTransformOutputV1> delivered;
    BlockTransformPipelineConfig config;
    config.source = &source;
    config.source_schema = schema;
    config.transform = &transform;
    config.source_residual = bind("id >= 2");
    config.transform_residual = bind("id >= 3");
    config.poll_timeout_ms = 17;
    config.output_consumer = [&](const BlockTransformOutputV1& output) {
        log.push_back("consume-" + std::to_string(output.ts_ms));
        delivered.push_back(output);
        return 0;
    };
    BlockTransformPipelineRunner runner(std::move(config));
    BlockTransformPipelineResult result;
    std::string error;
    assert(runner.Run(&result, &error) == BlockTransformPipelineError::kNone);
    assert(error.empty() && result.terminal == BlockTransformPipelineTerminal::kCompleted);
    assert(result.input_blocks == 2 && result.input_rows == 3);
    assert(result.output_blocks == 3 && result.output_rows == 2);
    assert(source.observed_timeout_ms == 17 && source.poll_calls == 4);
    assert(source.release_calls == 2 && source.released_blocks[0] == first_input &&
           source.released_blocks[1] == second_input);
    assert(transform.open_calls == 1 && transform.process_calls == 2 &&
           transform.flush_calls == 1 && transform.cancel_calls == 0);
    assert(transform.opened_input_schema == schema);
    assert(transform.process_timestamps == std::vector<int64_t>({0, 0}));
    assert(transform.process_inputs[0]->num_rows() == 1);
    assert(std::static_pointer_cast<arrow::Int32Array>(
               transform.process_inputs[0]->column(0))->Value(0) == 2);
    assert(transform.process_inputs[1]->num_rows() == 1);
    assert(delivered.size() == 3);
    assert(delivered[0].batch->num_rows() == 1 && delivered[0].ts_ms == 201);
    assert(delivered[1].batch->num_rows() == 0 && delivered[1].ts_ms == 202);
    assert(delivered[2].batch->num_rows() == 1 && delivered[2].ts_ms == 300);
    const std::vector<std::string> expected_log = {
        "transform-open", "poll-1", "poll-2", "process-1", "release-1",
        "poll-3", "process-2", "consume-201", "consume-202", "release-2",
        "poll-4", "flush", "consume-300",
    };
    assert(log == expected_log);

    assert(runner.Run(&result, &error) == BlockTransformPipelineError::kAlreadyRun);
    assert(result.terminal == BlockTransformPipelineTerminal::kFailed && !error.empty());

    {
        log.clear();
        ScriptedBlockSource stop_source({
            {BlockPollEvent::kData, first_input, 0},
            {BlockPollEvent::kData, second_input, 0},
        }, &log);
        ScriptedBlockTransformTask stop_transform(schema, &log);
        stop_transform.process_steps = {
            {e2i(BlockTransformStatusV1::kStop), {{output_three, 401}}},
        };
        stop_transform.flush_outputs = {{output_four, 402}};
        std::vector<int64_t> timestamps;
        BlockTransformPipelineConfig stop_config;
        stop_config.source = &stop_source;
        stop_config.source_schema = schema;
        stop_config.transform = &stop_transform;
        stop_config.output_consumer = [&](const BlockTransformOutputV1& output) {
            log.push_back("consume-" + std::to_string(output.ts_ms));
            timestamps.push_back(output.ts_ms);
            return 0;
        };
        BlockTransformPipelineRunner stop_runner(std::move(stop_config));
        assert(stop_runner.Run(&result, &error) == BlockTransformPipelineError::kNone);
        assert(result.terminal == BlockTransformPipelineTerminal::kStopped);
        assert(stop_source.poll_calls == 1 && stop_source.release_calls == 1);
        assert(stop_transform.process_calls == 1 && stop_transform.flush_calls == 1);
        assert(timestamps == std::vector<int64_t>({401, 402}));
        const std::vector<std::string> stop_log = {
            "transform-open", "poll-1", "process-1", "consume-401",
            "release-1", "flush", "consume-402",
        };
        assert(log == stop_log);
    }

    {
        ScriptedBlockSource cancelled_source({
            {BlockPollEvent::kCancelled, nullptr, ECANCELED},
        });
        ScriptedBlockTransformTask cancelled_transform(schema);
        BlockTransformPipelineConfig cancelled_config;
        cancelled_config.source = &cancelled_source;
        cancelled_config.source_schema = schema;
        cancelled_config.transform = &cancelled_transform;
        cancelled_config.output_consumer = [](const BlockTransformOutputV1&) { return 0; };
        BlockTransformPipelineRunner cancelled_runner(std::move(cancelled_config));
        assert(cancelled_runner.Run(&result, &error) ==
               BlockTransformPipelineError::kCancelled);
        assert(result.terminal == BlockTransformPipelineTerminal::kCancelled);
        assert(cancelled_transform.flush_calls == 0 && cancelled_transform.cancel_calls == 1);
        assert(cancelled_source.cancel_calls == 1 && !error.empty());
    }

    {
        ScriptedBlockSource cancelled_source({
            {BlockPollEvent::kEof, nullptr, 0},
        });
        ScriptedBlockTransformTask cancelled_transform(schema);
        BlockTransformPipelineConfig cancelled_config;
        cancelled_config.source = &cancelled_source;
        cancelled_config.source_schema = schema;
        cancelled_config.transform = &cancelled_transform;
        cancelled_config.output_consumer = [](const BlockTransformOutputV1&) { return 0; };
        BlockTransformPipelineRunner cancelled_runner(std::move(cancelled_config));
        cancelled_runner.Cancel();
        cancelled_runner.Cancel();
        assert(cancelled_source.cancel_calls == 1 && cancelled_transform.cancel_calls == 1);
        assert(cancelled_runner.Run(&result, &error) ==
               BlockTransformPipelineError::kCancelled);
        assert(result.terminal == BlockTransformPipelineTerminal::kCancelled);
        assert(cancelled_source.poll_calls == 0 && cancelled_transform.open_calls == 0);
    }

    auto run_failure = [&](BlockPollEvent terminal_event,
                           ScriptedBlockTransformTask* failure_transform,
                           int release_return_code,
                           int consumer_return_code,
                           BlockTransformPipelineError expected) {
        ScriptedBlockSource failure_source({
            {BlockPollEvent::kData, first_input, 0}, terminal_event,
        });
        failure_source.release_return_code = release_return_code;
        BlockTransformPipelineConfig failure_config;
        failure_config.source = &failure_source;
        failure_config.source_schema = schema;
        failure_config.transform = failure_transform;
        failure_config.output_consumer = [consumer_return_code](const BlockTransformOutputV1&) {
            return consumer_return_code;
        };
        BlockTransformPipelineRunner failure_runner(std::move(failure_config));
        const auto failure = failure_runner.Run(&result, &error);
        assert(failure == expected);
        assert(result.terminal == BlockTransformPipelineTerminal::kFailed);
        assert(failure_source.release_calls == 1);
        assert(failure_transform->flush_calls == 0);
        assert(!error.empty());
        return failure_source;
    };

    {
        ScriptedBlockTransformTask bad_transform(schema);
        bad_transform.process_steps = {{-EIO, {{output_three, 501}}}};
        auto failure_source = run_failure(
            {BlockPollEvent::kEof, nullptr, 0},
            &bad_transform,
            0,
            0,
            BlockTransformPipelineError::kTransformContractViolation);
        assert(bad_transform.cancel_calls == 1 && failure_source.cancel_calls == 1);
    }

    {
        ScriptedBlockTransformTask invalid_output(schema);
        invalid_output.process_steps = {
            {e2i(BlockTransformStatusV1::kContinue), {{nullptr, 0}}},
        };
        run_failure(
            {BlockPollEvent::kEof, nullptr, 0},
            &invalid_output,
            0,
            0,
            BlockTransformPipelineError::kTransformContractViolation);
    }

    {
        ScriptedBlockTransformTask consumer_failure(schema);
        consumer_failure.process_steps = {
            {e2i(BlockTransformStatusV1::kContinue), {{output_three, 601}}},
        };
        run_failure(
            {BlockPollEvent::kEof, nullptr, 0},
            &consumer_failure,
            0,
            EIO,
            BlockTransformPipelineError::kOutputConsumerFailed);
    }

    {
        ScriptedBlockTransformTask release_failure(schema);
        release_failure.process_steps = {
            {e2i(BlockTransformStatusV1::kContinue), {}},
        };
        run_failure(
            {BlockPollEvent::kEof, nullptr, 0},
            &release_failure,
            EIO,
            0,
            BlockTransformPipelineError::kSourceReleaseFailed);
    }

    {
        ScriptedBlockSource poll_failure({
            {BlockPollEvent::kError, nullptr, EIO},
        });
        ScriptedBlockTransformTask idle_transform(schema);
        BlockTransformPipelineConfig failure_config;
        failure_config.source = &poll_failure;
        failure_config.source_schema = schema;
        failure_config.transform = &idle_transform;
        failure_config.output_consumer = [](const BlockTransformOutputV1&) { return 0; };
        BlockTransformPipelineRunner failure_runner(std::move(failure_config));
        assert(failure_runner.Run(&result, &error) ==
               BlockTransformPipelineError::kSourcePollFailed);
        assert(result.terminal == BlockTransformPipelineTerminal::kFailed);
        assert(poll_failure.release_calls == 0 && poll_failure.cancel_calls == 1);
        assert(idle_transform.flush_calls == 0 && idle_transform.cancel_calls == 1);
    }

    {
        ScriptedBlockSource flush_source({
            {BlockPollEvent::kEof, nullptr, 0},
        });
        ScriptedBlockTransformTask flush_failure(schema);
        flush_failure.flush_return_code = EIO;
        flush_failure.flush_outputs = {{output_three, 701}};
        int consumed = 0;
        BlockTransformPipelineConfig failure_config;
        failure_config.source = &flush_source;
        failure_config.source_schema = schema;
        failure_config.transform = &flush_failure;
        failure_config.output_consumer = [&](const BlockTransformOutputV1&) {
            ++consumed;
            return 0;
        };
        BlockTransformPipelineRunner failure_runner(std::move(failure_config));
        assert(failure_runner.Run(&result, &error) ==
               BlockTransformPipelineError::kTransformContractViolation);
        assert(result.terminal == BlockTransformPipelineTerminal::kFailed);
        assert(flush_failure.flush_calls == 1 && consumed == 0);
    }

    printf("[PASS] block transform pipeline runner\n");
}

// ============================================================
// Test 7.7: exact pushdown candidate and residual splitting
// ============================================================
void test_filter_pushdown_split() {
    printf("[TEST] filter pushed/residual split...\n");
    auto schema = arrow::schema({
        arrow::field("a", arrow::boolean(), true),
        arrow::field("b", arrow::boolean(), true),
        arrow::field("c", arrow::boolean(), true),
    });
    auto bind = [&](const std::string& text) {
        std::shared_ptr<FilterExpr> parsed;
        std::string error;
        assert(ParseFilterExpression(text, &parsed, &error));
        std::shared_ptr<const BoundFilterExpr> bound;
        assert(BindFilterExpression(schema, parsed, &bound, &error) ==
               FilterBindError::kNone);
        assert(bound && error.empty());
        return bound;
    };

    const auto original = bind("a AND b AND c");
    FilterPushdownSplit split;
    std::string error;
    assert(SplitFilterForPushdown(original, {}, &split, &error) ==
           FilterPlanError::kNone);
    assert(error.empty());
    assert(split.candidate_node_ids == std::vector<uint32_t>({3, 4, 5}));
    assert(split.accepted_node_ids.empty());
    assert(!split.pushed_expression && split.residual_expression == original);

    assert(SplitFilterForPushdown(original, {5, 3}, &split, &error) ==
           FilterPlanError::kNone);
    assert(split.candidate_node_ids == std::vector<uint32_t>({3, 4, 5}));
    assert(split.accepted_node_ids == std::vector<uint32_t>({3, 5}));
    assert(split.pushed_expression && split.pushed_expression->node_id == 1);
    assert(split.pushed_expression->kind == FilterExprKind::kAnd);
    assert(split.pushed_expression->operands[0]->node_id == 3);
    assert(split.pushed_expression->operands[1]->node_id == 5);
    assert(split.residual_expression && split.residual_expression->node_id == 4);

    arrow::BooleanBuilder a_builder;
    arrow::BooleanBuilder b_builder;
    arrow::BooleanBuilder c_builder;
    assert(a_builder.AppendValues(
        std::vector<bool>({true, true, true, false, true})).ok());
    assert(b_builder.Append(true).ok());
    assert(b_builder.Append(false).ok());
    assert(b_builder.AppendNull().ok());
    assert(b_builder.Append(true).ok());
    assert(b_builder.Append(true).ok());
    assert(c_builder.AppendValues(
        std::vector<bool>({true, true, true, true, false})).ok());
    std::shared_ptr<arrow::Array> a;
    std::shared_ptr<arrow::Array> b;
    std::shared_ptr<arrow::Array> c;
    assert(a_builder.Finish(&a).ok());
    assert(b_builder.Finish(&b).ok());
    assert(c_builder.Finish(&c).ok());
    auto batch = arrow::RecordBatch::Make(schema, 5, {a, b, c});

    std::shared_ptr<arrow::RecordBatch> expected;
    std::shared_ptr<arrow::RecordBatch> pushed;
    std::shared_ptr<arrow::RecordBatch> actual;
    assert(FilterRecordBatch(batch, original, &expected, &error) ==
           FilterEvalError::kNone);
    assert(FilterRecordBatch(batch, split.pushed_expression, &pushed, &error) ==
           FilterEvalError::kNone);
    assert(FilterRecordBatch(pushed, split.residual_expression, &actual, &error) ==
           FilterEvalError::kNone);
    assert(expected && actual && expected->Equals(*actual, true));

    assert(SplitFilterForPushdown(original, {5, 4, 3}, &split, &error) ==
           FilterPlanError::kNone);
    assert(split.accepted_node_ids == std::vector<uint32_t>({3, 4, 5}));
    assert(split.pushed_expression == original && !split.residual_expression);

    split.pushed_expression = original;
    assert(SplitFilterForPushdown(original, {3, 3}, &split, &error) ==
           FilterPlanError::kDuplicateAcceptedNode);
    assert(split.candidate_node_ids.empty() && !split.pushed_expression);
    assert(!error.empty());

    assert(SplitFilterForPushdown(original, {999}, &split, &error) ==
           FilterPlanError::kUnknownAcceptedNode);
    assert(split.candidate_node_ids.empty() && !split.residual_expression);
    assert(!error.empty());

    assert(SplitFilterForPushdown(original, {2}, &split, &error) ==
           FilterPlanError::kNonCandidateAcceptedNode);
    assert(split.candidate_node_ids.empty() && !split.residual_expression);
    assert(!error.empty());

    const auto disjunction = bind("a OR b");
    assert(SplitFilterForPushdown(disjunction, {}, &split, &error) ==
           FilterPlanError::kNone);
    assert(split.candidate_node_ids == std::vector<uint32_t>({1}));
    assert(!split.pushed_expression && split.residual_expression == disjunction);
    assert(SplitFilterForPushdown(disjunction, {2}, &split, &error) ==
           FilterPlanError::kNonCandidateAcceptedNode);
    assert(SplitFilterForPushdown(disjunction, {1}, &split, &error) ==
           FilterPlanError::kNone);
    assert(split.pushed_expression == disjunction && !split.residual_expression);

    const auto negation = bind("NOT a");
    assert(SplitFilterForPushdown(negation, {}, &split, &error) ==
           FilterPlanError::kNone);
    assert(split.candidate_node_ids == std::vector<uint32_t>({1}));

    auto malformed = std::make_shared<BoundFilterExpr>(*original);
    malformed->operands = {original->operands[0], original->operands[0]};
    assert(SplitFilterForPushdown(malformed, {}, &split, &error) ==
           FilterPlanError::kInvalidBoundExpression);
    assert(split.candidate_node_ids.empty() && !split.pushed_expression &&
           !split.residual_expression && !error.empty());

    assert(SplitFilterForPushdown(nullptr, {}, &split, &error) ==
           FilterPlanError::kInvalidArgument);
    assert(split.candidate_node_ids.empty() && !error.empty());

    printf("[PASS] filter pushed/residual split\n");
}

class RecordingFilterPushdownProvider final : public IFilterPushdownV1 {
 public:
    int EvaluatePushdown(const FilterPushdownRequestV1& request,
                         FilterPushdownResultV1* result) const override {
        ++call_count;
        observed_contract_version = request.contract_version;
        observed_target_kind = request.target_kind;
        observed_category = request.target_category ? request.target_category : "";
        observed_name = request.target_name ? request.target_name : "";
        observed_schema = request.output_schema;
        observed_plan = request.canonical_plan_json ? request.canonical_plan_json : "";
        observed_candidates = request.candidate_node_ids;
        assert(result);
        result->accepted_node_ids = accepted_node_ids;
        result->diagnostic = diagnostic;
        return return_code;
    }

    int return_code = ENOTSUP;
    std::vector<uint32_t> accepted_node_ids;
    std::string diagnostic;
    mutable int call_count = 0;
    mutable uint32_t observed_contract_version = 0;
    mutable FilterPushdownTargetKindV1 observed_target_kind =
        FilterPushdownTargetKindV1::kSource;
    mutable std::string observed_category;
    mutable std::string observed_name;
    mutable std::shared_ptr<arrow::Schema> observed_schema;
    mutable std::string observed_plan;
    mutable std::vector<uint32_t> observed_candidates;
};

class RecordingFilterPushdownQuerier final : public IQuerier {
 public:
    int Traverse(const Guid& iid, fntraverse proc) override {
        ++traverse_call_count;
        observed_iid = iid;
        for (void* provider : providers) {
            ++callback_call_count;
            if (proc(provider) == -1) break;
        }
        return traverse_return_code;
    }

    void* First(const Guid&) override {
        ++first_call_count;
        return nullptr;
    }

    std::vector<void*> providers;
    int traverse_return_code = 0;
    int traverse_call_count = 0;
    int callback_call_count = 0;
    int first_call_count = 0;
    Guid observed_iid{};
};

// ============================================================
// Test 7.6: canonical plan and IID pushdown negotiation
// ============================================================
void test_filter_pushdown_negotiation() {
    printf("[TEST] canonical filter pushdown negotiation...\n");
    auto schema = arrow::schema({
        arrow::field("id", arrow::int32(), false),
        arrow::field("enabled", arrow::boolean(), true),
    });
    std::shared_ptr<FilterExpr> parsed;
    std::string error;
    assert(ParseFilterExpression("id >= 42 AND enabled", &parsed, &error));
    std::shared_ptr<const BoundFilterExpr> original;
    assert(BindFilterExpression(schema, parsed, &original, &error) ==
           FilterBindError::kNone);

    std::string canonical;
    assert(BuildCanonicalFilterPlan(schema, original, &canonical, &error) ==
           FilterPlanError::kNone);
    assert(error.empty() && !canonical.empty());
    std::string canonical_again;
    assert(BuildCanonicalFilterPlan(schema, original, &canonical_again, &error) ==
           FilterPlanError::kNone);
    assert(canonical_again == canonical);

    rapidjson::Document document;
    document.Parse(canonical.c_str());
    assert(!document.HasParseError() && document.IsObject());
    assert(document["version"].IsUint() && document["version"].GetUint() == 1);
    const auto& root = document["root"];
    assert(root["node_id"].GetUint() == 1);
    assert(std::string(root["kind"].GetString()) == "and");
    assert(std::string(root["type"].GetString()) == "bool");
    assert(root["nullable"].GetBool());
    assert(root["operands"].IsArray() && root["operands"].Size() == 2);
    const auto& compare = root["operands"][0];
    assert(compare["node_id"].GetUint() == 2);
    assert(std::string(compare["kind"].GetString()) == "compare");
    assert(std::string(compare["compare_op"].GetString()) == "greater_equal");
    assert(std::string(compare["comparison_type"].GetString()) == "int32");
    const auto& field = compare["operands"][0];
    assert(field["field_index"].GetInt() == 0);
    assert(std::string(field["field_name"].GetString()) == "id");
    assert(std::string(field["type"].GetString()) == "int32");
    assert(!field["nullable"].GetBool());
    const auto& literal = compare["operands"][1];
    assert(std::string(literal["kind"].GetString()) == "literal");
    assert(std::string(literal["type"].GetString()) == "int32");
    assert(std::string(literal["value"].GetString()) == "42");
    const auto& enabled = root["operands"][1];
    assert(enabled["field_index"].GetInt() == 1);
    assert(std::string(enabled["field_name"].GetString()) == "enabled");

    FilterPushdownTarget target;
    target.kind = FilterPushdownTargetKindV1::kTransform;
    target.category = "npm";
    target.name = "basic";
    target.output_schema = schema;

    RecordingFilterPushdownProvider unsupported;
    unsupported.return_code = ENOTSUP;
    unsupported.accepted_node_ids = {2};
    unsupported.diagnostic = "not this target";
    RecordingFilterPushdownProvider owner;
    owner.return_code = 0;
    owner.accepted_node_ids = {5, 2};
    owner.diagnostic = "both predicates are exact";
    RecordingFilterPushdownProvider after_owner;
    after_owner.return_code = 0;
    after_owner.accepted_node_ids = {2};
    RecordingFilterPushdownQuerier querier;
    querier.providers = {&unsupported, &owner, &after_owner};

    FilterPushdownNegotiation negotiation;
    assert(NegotiateFilterPushdown(&querier, target, original, &negotiation, &error) ==
           FilterPlanError::kNone);
    assert(error.empty() && negotiation.provider_matched);
    assert(negotiation.canonical_plan_json == canonical);
    assert(negotiation.provider_diagnostic == "both predicates are exact");
    assert(negotiation.split.candidate_node_ids == std::vector<uint32_t>({2, 5}));
    assert(negotiation.split.accepted_node_ids == std::vector<uint32_t>({2, 5}));
    assert(negotiation.split.pushed_expression == original);
    assert(!negotiation.split.residual_expression);
    assert(unsupported.call_count == 1 && owner.call_count == 1 && after_owner.call_count == 0);
    assert(querier.traverse_call_count == 1 && querier.callback_call_count == 2);
    assert(querier.first_call_count == 0);
    assert(std::memcmp(&querier.observed_iid, &IID_FILTER_PUSHDOWN_V1, sizeof(Guid)) == 0);
    assert(owner.observed_contract_version == kFilterPushdownContractVersionV1);
    assert(owner.observed_target_kind == FilterPushdownTargetKindV1::kTransform);
    assert(owner.observed_category == "npm" && owner.observed_name == "basic");
    assert(owner.observed_schema == schema && owner.observed_plan == canonical);
    assert(owner.observed_candidates == std::vector<uint32_t>({2, 5}));

    auto expect_cleared = [](const FilterPushdownNegotiation& value) {
        assert(!value.provider_matched && value.canonical_plan_json.empty());
        assert(value.provider_diagnostic.empty());
        assert(value.split.candidate_node_ids.empty());
        assert(value.split.accepted_node_ids.empty());
        assert(!value.split.pushed_expression && !value.split.residual_expression);
    };
    auto prime = [&]() {
        FilterPushdownNegotiation value;
        value.provider_matched = true;
        value.canonical_plan_json = "stale";
        value.provider_diagnostic = "stale";
        value.split.candidate_node_ids = {999};
        value.split.accepted_node_ids = {999};
        value.split.pushed_expression = original;
        value.split.residual_expression = original;
        return value;
    };

    RecordingFilterPushdownQuerier no_provider;
    assert(NegotiateFilterPushdown(&no_provider, target, original, &negotiation, &error) ==
           FilterPlanError::kNone);
    assert(!negotiation.provider_matched && negotiation.provider_diagnostic.empty());
    assert(negotiation.canonical_plan_json == canonical);
    assert(negotiation.split.candidate_node_ids == std::vector<uint32_t>({2, 5}));
    assert(negotiation.split.accepted_node_ids.empty());
    assert(!negotiation.split.pushed_expression &&
           negotiation.split.residual_expression == original);

    RecordingFilterPushdownProvider only_unsupported;
    only_unsupported.return_code = ENOTSUP;
    only_unsupported.diagnostic = "ignored";
    RecordingFilterPushdownQuerier all_unsupported;
    all_unsupported.providers = {&only_unsupported};
    assert(NegotiateFilterPushdown(
               &all_unsupported, target, original, &negotiation, &error) ==
           FilterPlanError::kNone);
    assert(!negotiation.provider_matched && negotiation.provider_diagnostic.empty());
    assert(negotiation.split.residual_expression == original);

    RecordingFilterPushdownProvider empty_owner;
    empty_owner.return_code = 0;
    empty_owner.diagnostic = "owned but no exact predicate";
    RecordingFilterPushdownProvider should_not_run;
    should_not_run.return_code = 0;
    RecordingFilterPushdownQuerier empty_acceptance;
    empty_acceptance.providers = {&empty_owner, &should_not_run};
    assert(NegotiateFilterPushdown(
               &empty_acceptance, target, original, &negotiation, &error) ==
           FilterPlanError::kNone);
    assert(negotiation.provider_matched);
    assert(negotiation.provider_diagnostic == "owned but no exact predicate");
    assert(negotiation.split.accepted_node_ids.empty());
    assert(negotiation.split.residual_expression == original);
    assert(should_not_run.call_count == 0);

    RecordingFilterPushdownProvider partial_owner;
    partial_owner.return_code = 0;
    partial_owner.accepted_node_ids = {5};
    RecordingFilterPushdownQuerier partial;
    partial.providers = {&partial_owner};
    assert(NegotiateFilterPushdown(&partial, target, original, &negotiation, &error) ==
           FilterPlanError::kNone);
    assert(negotiation.split.accepted_node_ids == std::vector<uint32_t>({5}));
    assert(negotiation.split.pushed_expression &&
           negotiation.split.pushed_expression->node_id == 5);
    assert(negotiation.split.residual_expression &&
           negotiation.split.residual_expression->node_id == 2);

    const std::vector<std::pair<std::vector<uint32_t>, FilterPlanError>> invalid_sets = {
        {{2, 2}, FilterPlanError::kDuplicateAcceptedNode},
        {{999}, FilterPlanError::kUnknownAcceptedNode},
        {{1}, FilterPlanError::kNonCandidateAcceptedNode},
    };
    for (const auto& item : invalid_sets) {
        RecordingFilterPushdownProvider invalid_owner;
        invalid_owner.return_code = 0;
        invalid_owner.accepted_node_ids = item.first;
        RecordingFilterPushdownQuerier invalid_querier;
        invalid_querier.providers = {&invalid_owner};
        negotiation = prime();
        assert(NegotiateFilterPushdown(
                   &invalid_querier, target, original, &negotiation, &error) == item.second);
        expect_cleared(negotiation);
        assert(!error.empty());
    }

    RecordingFilterPushdownProvider failed_owner;
    failed_owner.return_code = EIO;
    failed_owner.diagnostic = "provider read failed";
    RecordingFilterPushdownQuerier provider_failure;
    provider_failure.providers = {&failed_owner, &after_owner};
    negotiation = prime();
    assert(NegotiateFilterPushdown(
               &provider_failure, target, original, &negotiation, &error) ==
           FilterPlanError::kProviderNegotiationFailed);
    expect_cleared(negotiation);
    assert(error.find("provider read failed") != std::string::npos);

    RecordingFilterPushdownQuerier traversal_failure;
    traversal_failure.traverse_return_code = EIO;
    negotiation = prime();
    assert(NegotiateFilterPushdown(
               &traversal_failure, target, original, &negotiation, &error) ==
           FilterPlanError::kQuerierError);
    expect_cleared(negotiation);
    assert(!error.empty());

    auto out_of_range_field =
        std::make_shared<BoundFilterExpr>(*original->operands[0]->operands[0]);
    out_of_range_field->field_index = 99;
    auto invalid_compare = std::make_shared<BoundFilterExpr>(*original->operands[0]);
    invalid_compare->operands[0] = out_of_range_field;
    auto invalid_root = std::make_shared<BoundFilterExpr>(*original);
    invalid_root->operands[0] = invalid_compare;
    canonical_again = "stale";
    assert(BuildCanonicalFilterPlan(schema, invalid_root, &canonical_again, &error) ==
           FilterPlanError::kCanonicalPlanError);
    assert(canonical_again.empty() && !error.empty());

    auto wrong_schema = arrow::schema({
        arrow::field("id", arrow::int64(), false),
        arrow::field("enabled", arrow::boolean(), true),
    });
    assert(BuildCanonicalFilterPlan(wrong_schema, original, &canonical_again, &error) ==
           FilterPlanError::kCanonicalPlanError);
    assert(canonical_again.empty() && !error.empty());

    auto unsupported_call = std::make_shared<BoundFilterExpr>();
    unsupported_call->kind = FilterExprKind::kCall;
    unsupported_call->node_id = 1;
    unsupported_call->value_type = arrow::boolean();
    assert(BuildCanonicalFilterPlan(schema, unsupported_call, &canonical_again, &error) ==
           FilterPlanError::kCanonicalPlanError);
    assert(canonical_again.empty() && !error.empty());

    printf("[PASS] canonical filter pushdown negotiation\n");
}

// ============================================================
// Test 7.7: statement stage filter binding and compatibility
// ============================================================
void test_statement_stage_filters() {
    printf("[TEST] statement stage filters...\n");
    SqlParser parser;

    {
        auto stmt = parser.Parse(
            "SELECT * FROM pcapfile.rdp "
            "WHERE has_layer('ipv4') AND has_layer('tcp') "
            "USING npm.basic WHERE protocol = 'HTTP' "
            "INTO dataframe.http_packets");
        assert(stmt.error.empty());
        assert(stmt.where_clause == "has_layer('ipv4') AND has_layer('tcp')");
        assert(stmt.sql_part ==
               "SELECT * FROM pcapfile.rdp WHERE has_layer('ipv4') AND has_layer('tcp')");
        assert(stmt.stage_filters.size() == 2);
        assert(stmt.stage_filters[0].after_stage == 0);
        assert(stmt.stage_filters[0].filter_text ==
               "has_layer('ipv4') AND has_layer('tcp')");
        assert(stmt.stage_filters[1].after_stage == 1);
        assert(stmt.stage_filters[1].filter_text == "protocol = 'HTTP'");
    }

    {
        auto stmt = parser.Parse(
            "SELECT * FROM pcapfile.rdp "
            "WHERE timestamp_ns >= TIMESTAMP '2026-09-08T09:30:00.123456789+08:00' "
            "AND tcp('192.0.2.10', 52314, '198.51.100.20', 3389) "
            "INTO dataframe.rdp");
        assert(stmt.error.empty());
        assert(stmt.stage_filters.size() == 1);
        assert(stmt.stage_filters[0].filter_text ==
               "timestamp_ns >= TIMESTAMP '2026-09-08T09:30:00.123456789+08:00' "
               "AND tcp('192.0.2.10', 52314, '198.51.100.20', 3389)");
        assert(stmt.dest == "dataframe.rdp");
    }

    {
        auto stmt = parser.Parse(
            "SELECT * FROM source.input WHERE captured_len > 100 "
            "USING demo.op1 WITH mode=fast WHERE score >= 0.8 "
            "THEN demo.op2 WHERE result IS NOT NULL INTO dataframe.output");
        assert(stmt.error.empty());
        assert(stmt.operators.size() == 2);
        assert(stmt.operator_with_params.size() == 2);
        assert(stmt.operator_with_params[0].at("mode") == "fast");
        assert(stmt.stage_filters.size() == 3);
        assert(stmt.stage_filters[0].after_stage == 0);
        assert(stmt.stage_filters[1].after_stage == 1);
        assert(stmt.stage_filters[2].after_stage == 2);
        assert(stmt.stage_filters[2].filter_text == "result IS NOT NULL");
        assert(stmt.dest == "dataframe.output");
    }

    {
        auto stmt = parser.Parse(
            "SELECT * FROM source.input USING demo.op "
            "WHERE note = 'USING THEN INTO WHERE' INTO dataframe.output");
        assert(stmt.error.empty());
        assert(stmt.where_clause.empty());
        assert(stmt.stage_filters.size() == 1);
        assert(stmt.stage_filters[0].after_stage == 1);
        assert(stmt.stage_filters[0].filter_text ==
               "note = 'USING THEN INTO WHERE'");
    }

    {
        const std::string native_sql =
            "SELECT * FROM sqlite.mydb.users "
            "WHERE id IN (SELECT user_id FROM sqlite.mydb.orders)";
        auto stmt = parser.Parse(native_sql);
        assert(stmt.error.empty());
        assert(stmt.sql_part == native_sql);
        assert(stmt.stage_filters.empty());
    }

    {
        auto stmt = parser.Parse(
            "SELECT * FROM sqlite.mydb.logs "
            "WHERE note = 'USING INTO THEN WHERE' ORDER BY ts "
            "USING ml.predict WHERE score > 0.5 INTO dataframe.output");
        assert(stmt.error.empty());
        assert(stmt.sql_part ==
               "SELECT * FROM sqlite.mydb.logs "
               "WHERE note = 'USING INTO THEN WHERE' ORDER BY ts");
        assert(stmt.stage_filters.size() == 1);
        assert(stmt.stage_filters[0].after_stage == 1);
    }

    {
        auto stmt = parser.Parse(
            "SELECT * FROM sqlite.mydb.left_table JOIN right_table USING (id) "
            "USING ml.predict WHERE score > 0.5 INTO dataframe.output");
        assert(stmt.error.empty());
        assert(stmt.sql_part ==
               "SELECT * FROM sqlite.mydb.left_table JOIN right_table USING (id)");
        assert(stmt.operators.size() == 1);
        assert(stmt.stage_filters.size() == 1);
        assert(stmt.stage_filters[0].after_stage == 1);
    }

    {
        const std::vector<std::string> deferred_filter_syntax = {
            "ipv4 & tcp",
            "protocol == 'HTTP'",
        };
        for (const auto& filter_text : deferred_filter_syntax) {
            auto stmt = parser.Parse(
                "SELECT * FROM pcapfile.rdp WHERE " + filter_text +
                " INTO dataframe.output");
            assert(stmt.error.empty());
            assert(stmt.stage_filters.size() == 1);
            assert(stmt.stage_filters[0].after_stage == 0);
            assert(stmt.stage_filters[0].filter_text == filter_text);
        }
    }

    {
        auto stmt = parser.Parse(
            "SELECT * FROM dataframe.left, dataframe.right "
            "WHERE id > 0 USING builtin.concat INTO dataframe.output");
        assert(!stmt.error.empty());
    }

    const std::vector<std::string> invalid = {
        "SELECT * FROM pcapfile.rdp WHERE USING npm.basic INTO dataframe.output",
        "SELECT * FROM pcapfile.rdp USING npm.basic WHERE protocol = 'HTTP' "
        "WHERE score > 0 INTO dataframe.output",
        "SELECT * FROM pcapfile.rdp THEN npm.basic INTO dataframe.output",
        "SELECT * FROM pcapfile.rdp INTO dataframe.output trailing",
        "SELECT * FROM sqlite.mydb.logs USING malformed INTO dataframe.output",
        "SELECT * FROM sqlite.mydb.logs INTO dataframe.output trailing",
    };
    for (const auto& sql : invalid) {
        auto stmt = parser.Parse(sql);
        assert(!stmt.error.empty());
    }

    printf("[PASS] statement stage filters\n");
}

class TestFilterPushdown final : public IFilterPushdownV1 {
 public:
    int EvaluatePushdown(const FilterPushdownRequestV1& request,
                         FilterPushdownResultV1* result) const override {
        if (!result || request.contract_version != kFilterPushdownContractVersionV1 ||
            !request.canonical_plan_json || !request.output_schema) {
            return -1;
        }
        result->accepted_node_ids.clear();
        if (!request.candidate_node_ids.empty()) {
            result->accepted_node_ids.push_back(request.candidate_node_ids.front());
        }
        return 0;
    }
};

class LegacyBlockOperatorAbiFixture final : public IBlockStreamOperator {
 public:
    std::string Category() override { return "test"; }
    std::string Name() override { return "legacy_abi"; }
    std::string Description() override { return "legacy ABI fixture"; }
    int Configure(const char*, const char*) override { return 0; }
    int Init(const char*) override { return 0; }
    int OnSchemaReady(std::shared_ptr<arrow::Schema>) override { return 0; }
    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>&, int64_t) override { return 0; }
    int Flush() override { return 0; }
};

class TestFilterDomainResolver final : public IFilterDomainResolverV1 {
 public:
    int Resolve(const FilterDomainResolveRequestV1& request,
                FilterDomainResolveResultV1* result) const override {
        if (result) *result = FilterDomainResolveResultV1{};
        if (!result || request.contract_version != kFilterDomainResolverContractVersionV1 ||
            !request.target_category || !request.target_name || !request.output_schema ||
            !request.expression) {
            return EINVAL;
        }
        if (std::string(request.target_category) != "pcapfile") return ENOTSUP;
        result->lowered_expression = std::make_shared<FilterExpr>(*request.expression);
        result->diagnostic = "packet domain resolved";
        return 0;
    }
};

class LegacyBlockStreamFactoryAbiFixture final : public IBlockStreamFactory {
 public:
    IBlockStreamChannel* Get(const char*, const char*) override { return nullptr; }
    void List(std::function<void(const char*, const char*, IBlockStreamChannel*)>) override {}
};

class TestExclusiveBlockStreamReader final : public IBlockStreamChannel {
 public:
    explicit TestExclusiveBlockStreamReader(const BlockStreamReaderConfigV1& config)
        : task_id_(config.task_id),
          source_category_(config.source_category),
          source_name_(config.source_name),
          pushed_filter_plan_json_(config.pushed_filter_plan_json) {}

    const std::string& TaskId() const { return task_id_; }
    const std::string& PushedFilterPlanJson() const { return pushed_filter_plan_json_; }
    bool IsCancelled() const { return cancelled_; }

    const char* Category() override { return source_category_.c_str(); }
    const char* Name() override { return source_name_.c_str(); }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return ""; }
    int Open() override {
        opened_ = true;
        return 0;
    }
    int Close() override {
        opened_ = false;
        return 0;
    }
    bool IsOpened() const override { return opened_; }
    int Flush() override { return 0; }
    BlockPollEvent PollBlock(int) override {
        return {cancelled_ ? BlockPollEvent::kCancelled : BlockPollEvent::kEof, nullptr, 0};
    }
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>&) override { return 0; }
    void Cancel() override { cancelled_ = true; }
    bool IsFinished() const override { return true; }

 private:
    std::string task_id_;
    std::string source_category_;
    std::string source_name_;
    std::string pushed_filter_plan_json_;
    bool opened_ = false;
    bool cancelled_ = false;
};

class TestBlockStreamReaderFactory final : public IBlockStreamReaderFactoryV1 {
 public:
    int CreateReader(const BlockStreamReaderConfigV1& config,
                     IBlockStreamChannel** reader) override {
        if (!reader) return EINVAL;
        *reader = nullptr;
        if (config.contract_version != kBlockStreamReaderContractVersionV1 || !config.task_id ||
            !config.source_category || !config.source_name || !config.pushed_filter_plan_json) {
            return EINVAL;
        }
        if (std::string(config.source_category) != "pcapfile") return ENOTSUP;
        *reader = new TestExclusiveBlockStreamReader(config);
        ++live_readers;
        return 0;
    }

    void ReleaseReader(IBlockStreamChannel* reader) override {
        assert(reader && live_readers > 0);
        delete reader;
        --live_readers;
        ++release_calls;
    }

    int live_readers = 0;
    int release_calls = 0;
};

// ============================================================
// Test 7.8: offline filter public contracts
// ============================================================
void test_offline_filter_public_contracts() {
    printf("[TEST] offline filter public contracts...\n");

    using ResolveMethod = int (IFilterDomainResolverV1::*)(
        const FilterDomainResolveRequestV1&, FilterDomainResolveResultV1*) const;
    const ResolveMethod resolve_method = &IFilterDomainResolverV1::Resolve;
    using CreateReaderMethod = int (IBlockStreamReaderFactoryV1::*)(
        const BlockStreamReaderConfigV1&, IBlockStreamChannel**);
    const CreateReaderMethod create_reader_method = &IBlockStreamReaderFactoryV1::CreateReader;
    using ReleaseReaderMethod = void (IBlockStreamReaderFactoryV1::*)(IBlockStreamChannel*);
    const ReleaseReaderMethod release_reader_method = &IBlockStreamReaderFactoryV1::ReleaseReader;
    assert(resolve_method && create_reader_method && release_reader_method);

    assert(sizeof(IID_FILTER_DOMAIN_RESOLVER_V1) == sizeof(Guid));
    assert(sizeof(IID_BLOCK_STREAM_READER_FACTORY_V1) == sizeof(Guid));
    assert(memcmp(&IID_FILTER_DOMAIN_RESOLVER_V1,
                  &IID_BLOCK_STREAM_READER_FACTORY_V1,
                  sizeof(Guid)) != 0);
    assert(memcmp(&IID_FILTER_DOMAIN_RESOLVER_V1,
                  &IID_FILTER_PUSHDOWN_V1,
                  sizeof(Guid)) != 0);
    assert(memcmp(&IID_BLOCK_STREAM_READER_FACTORY_V1,
                  &IID_BLOCK_STREAM_FACTORY,
                  sizeof(Guid)) != 0);
    assert(kFilterDomainSyntheticNodeIdBaseV1 == 0x80000000u);

    auto input = std::make_shared<FilterExpr>();
    input->kind = FilterExprKind::kField;
    input->node_id = 7;
    input->field_name = "ports_valid";
    FilterDomainResolveRequestV1 resolve_request;
    assert(resolve_request.contract_version == kFilterDomainResolverContractVersionV1);
    assert(resolve_request.target_kind == FilterDomainTargetKindV1::kSource);
    resolve_request.target_category = "pcapfile";
    resolve_request.target_name = "rdp";
    resolve_request.output_schema = packet::PacketSchema();
    resolve_request.expression = input;

    TestFilterDomainResolver resolver;
    FilterDomainResolveResultV1 resolved;
    assert(resolver.Resolve(resolve_request, &resolved) == 0);
    assert(resolved.lowered_expression && resolved.lowered_expression.get() != input.get());
    assert(resolved.lowered_expression->node_id == input->node_id);
    assert(resolved.lowered_expression->field_name == input->field_name);
    assert(input->field_name == "ports_valid" && input->operands.empty());
    assert(!resolved.diagnostic.empty());

    resolve_request.target_category = "other";
    assert(resolver.Resolve(resolve_request, &resolved) == ENOTSUP);
    assert(!resolved.lowered_expression && resolved.diagnostic.empty());

    BlockStreamReaderConfigV1 reader_config;
    assert(reader_config.contract_version == kBlockStreamReaderContractVersionV1);
    assert(!reader_config.task_id && !reader_config.source_category && !reader_config.source_name);
    assert(!reader_config.pushed_filter_plan_json);

    static_assert(std::is_same_v<decltype(packet::PacketIpKey{}.ipv4_network_order), uint32_t>);
    static_assert(std::is_same_v<decltype(packet::PacketEndpointKey{}.port), uint16_t>);
    static_assert(std::is_same_v<decltype(packet::TransportPairKey{}.transport_protocol), uint8_t>);

    packet::PacketIpKey first_ip;
    first_ip.family = packet::AddressFamily::kIPv4;
    first_ip.ipv4_network_order = 0x0a0200c0u;
    packet::PacketIpKey second_ip;
    second_ip.family = packet::AddressFamily::kIPv6;
    second_ip.ipv6[0] = 0x20;
    second_ip.ipv6[1] = 0x01;
    second_ip.ipv6[15] = 0x20;

    packet::TransportPairKey pair;
    pair.transport_protocol = 6;
    pair.first = {first_ip, 3389};
    pair.second = {first_ip, 52314};
    assert(pair.first.port < pair.second.port);
    assert(pair.first.address.family == packet::AddressFamily::kIPv4);

    packet::PacketFilterRule time_rule;
    time_rule.kind = packet::PacketFilterRuleKind::kTimeRange;
    time_rule.time_range.lower_ns = 1000000001;
    time_rule.time_range.upper_ns = 2000000000;
    time_rule.time_range.lower_inclusive = true;
    assert(time_rule.time_range.lower_ns.value() == 1000000001);
    assert(!time_rule.time_range.upper_inclusive);

    packet::PacketFilterRule header_rule;
    header_rule.kind = packet::PacketFilterRuleKind::kUnsignedRange;
    header_rule.unsigned_range.field = packet::PacketUnsignedField::kSequence;
    header_rule.unsigned_range.lower = 10;
    header_rule.unsigned_range.upper = 20;
    header_rule.unsigned_range.lower_inclusive = true;
    header_rule.unsigned_range.upper_inclusive = true;

    packet::PacketFilterRule mac_rule;
    mac_rule.kind = packet::PacketFilterRuleKind::kMacAnyOf;
    mac_rule.mac_keys.push_back({{0x00, 0x11, 0x22, 0x33, 0x44, 0x55}});
    packet::PacketFilterRule ip_rule;
    ip_rule.kind = packet::PacketFilterRuleKind::kIpAnyOf;
    ip_rule.ip_keys = {first_ip, second_ip};
    packet::PacketFilterRule port_rule;
    port_rule.kind = packet::PacketFilterRuleKind::kPortAnyOf;
    port_rule.ports = {53, 3389};
    packet::PacketFilterRule pair_rule;
    pair_rule.kind = packet::PacketFilterRuleKind::kTransportPairAnyOf;
    pair_rule.transport_pairs = {pair};

    packet::PcapFilterPlan plan;
    plan.root.kind = packet::PacketFilterRuleKind::kAnd;
    plan.root.operands = {
        time_rule, header_rule, mac_rule, ip_rule, port_rule, pair_rule,
    };
    assert(plan.version == packet::kPcapFilterPlanVersion);
    assert(plan.endpoint_scope == packet::EndpointScope::kInnermost);
    assert(plan.root.operands.size() == 6);
    assert(plan.root.operands[2].mac_keys[0].bytes[5] == 0x55);
    assert(plan.root.operands[3].ip_keys[1].ipv6[15] == 0x20);
    assert(plan.root.operands[4].ports == std::vector<uint16_t>({53, 3389}));
    assert(plan.root.operands[5].transport_pairs[0].transport_protocol == 6);

    printf("[PASS] offline filter public contracts\n");
}

// ============================================================
// Test 7.9: offline source reader ABI and ownership contracts
// ============================================================
void test_offline_filter_reader_factory_contracts() {
    printf("[TEST] offline filter reader factory contracts...\n");

    using LegacyGetMethod = IBlockStreamChannel* (IBlockStreamFactory::*)(const char*,
                                                                          const char*);
    using LegacyListMethod = void (IBlockStreamFactory::*)(
        std::function<void(const char*, const char*, IBlockStreamChannel*)>);
    const LegacyGetMethod legacy_get = &IBlockStreamFactory::Get;
    const LegacyListMethod legacy_list = &IBlockStreamFactory::List;
    assert(legacy_get && legacy_list);
    static_assert(!std::is_base_of_v<IBlockStreamFactory, IBlockStreamReaderFactoryV1>);
    static_assert(!std::is_base_of_v<IBlockStreamReaderFactoryV1, IBlockStreamFactory>);
    LegacyBlockStreamFactoryAbiFixture legacy_factory;
    assert(!legacy_factory.Get("pcapfile", "rdp"));
    legacy_factory.List(nullptr);

    TestBlockStreamReaderFactory provider;
    std::string task_one = "task-one";
    std::string category_one = "pcapfile";
    std::string source_one = "rdp";
    std::string plan_one = "{\"version\":1,\"root\":{\"node_id\":7}}";
    const std::string expected_plan_one = plan_one;
    BlockStreamReaderConfigV1 config_one;
    config_one.task_id = task_one.c_str();
    config_one.source_category = category_one.c_str();
    config_one.source_name = source_one.c_str();
    config_one.pushed_filter_plan_json = plan_one.c_str();
    IBlockStreamChannel* reader_one = nullptr;
    assert(provider.CreateReader(config_one, &reader_one) == 0);
    assert(reader_one && provider.live_readers == 1);

    task_one = "caller-task-changed";
    category_one = "caller-category-changed";
    source_one = "caller-source-changed";
    plan_one = "caller-plan-changed";
    auto* copied_one = dynamic_cast<TestExclusiveBlockStreamReader*>(reader_one);
    assert(copied_one);
    assert(copied_one->TaskId() == "task-one");
    assert(std::string(copied_one->Category()) == "pcapfile");
    assert(std::string(copied_one->Name()) == "rdp");
    assert(copied_one->PushedFilterPlanJson() == expected_plan_one);

    IBlockStreamChannel* reader_two = nullptr;
    {
        std::string task_two = "task-two";
        std::string category_two = "pcapfile";
        std::string source_two = "dns";
        std::string plan_two = "{\"version\":1,\"root\":null}";
        BlockStreamReaderConfigV1 config_two;
        config_two.task_id = task_two.c_str();
        config_two.source_category = category_two.c_str();
        config_two.source_name = source_two.c_str();
        config_two.pushed_filter_plan_json = plan_two.c_str();
        assert(provider.CreateReader(config_two, &reader_two) == 0);
    }
    auto* copied_two = dynamic_cast<TestExclusiveBlockStreamReader*>(reader_two);
    assert(copied_two && reader_two != reader_one && provider.live_readers == 2);
    assert(copied_two->TaskId() == "task-two");
    assert(std::string(copied_two->Category()) == "pcapfile");
    assert(std::string(copied_two->Name()) == "dns");
    assert(copied_two->PushedFilterPlanJson() == "{\"version\":1,\"root\":null}");

    assert(reader_one->Open() == 0);
    assert(reader_one->IsOpened() && !reader_two->IsOpened());
    reader_one->Cancel();
    assert(copied_one->IsCancelled() && !copied_two->IsCancelled());
    assert(reader_one->PollBlock(0).kind == BlockPollEvent::kCancelled);
    assert(reader_two->PollBlock(0).kind == BlockPollEvent::kEof);

    BlockStreamReaderConfigV1 rejected_config;
    rejected_config.task_id = "task-rejected";
    rejected_config.source_category = "other";
    rejected_config.source_name = "rdp";
    rejected_config.pushed_filter_plan_json = "{\"version\":1,\"root\":null}";
    auto* rejected = reinterpret_cast<IBlockStreamChannel*>(1);
    assert(provider.CreateReader(rejected_config, &rejected) == ENOTSUP);
    assert(!rejected && provider.live_readers == 2);

    rejected_config.source_category = "pcapfile";
    rejected_config.contract_version = kBlockStreamReaderContractVersionV1 + 1;
    rejected = reinterpret_cast<IBlockStreamChannel*>(1);
    assert(provider.CreateReader(rejected_config, &rejected) == EINVAL);
    assert(!rejected && provider.live_readers == 2);
    rejected_config.contract_version = kBlockStreamReaderContractVersionV1;
    assert(provider.CreateReader(rejected_config, nullptr) == EINVAL);
    assert(provider.live_readers == 2);

    provider.ReleaseReader(reader_one);
    provider.ReleaseReader(reader_two);
    assert(provider.live_readers == 0 && provider.release_calls == 2);

    printf("[PASS] offline filter reader factory contracts\n");
}

class TestBlockTransformTask final : public IBlockTransformTaskV1 {
 public:
    explicit TestBlockTransformTask(const BlockTransformTaskConfigV1& config)
        : contract_version_(config.contract_version),
          task_id_(config.task_id),
          with_params_json_(config.with_params_json),
          pushed_filter_plan_json_(config.pushed_filter_plan_json) {}

    uint32_t ContractVersion() const { return contract_version_; }
    const std::string& TaskId() const { return task_id_; }
    const std::string& WithParamsJson() const { return with_params_json_; }
    const std::string& PushedFilterPlanJson() const {
        return pushed_filter_plan_json_;
    }

    int Open(std::shared_ptr<arrow::Schema> input_schema,
             std::shared_ptr<arrow::Schema>* output_schema) override {
        if (!input_schema || !output_schema) return -1;
        schema_ = std::move(input_schema);
        *output_schema = schema_;
        return 0;
    }

    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input,
                     int64_t ts_ms,
                     std::vector<BlockTransformOutputV1>* outputs) override {
        if (!input || !outputs || !outputs->empty() || cancelled_) return -1;
        outputs->push_back({input, ts_ms});
        return 0;
    }

    int Flush(std::vector<BlockTransformOutputV1>* outputs) override {
        return outputs && outputs->empty() && !cancelled_ ? 0 : -1;
    }

    void Cancel() override { cancelled_ = true; }
    std::string LastError() const override { return ""; }

 private:
    uint32_t contract_version_ = 0;
    std::string task_id_;
    std::string with_params_json_;
    std::string pushed_filter_plan_json_;
    bool cancelled_ = false;
    std::shared_ptr<arrow::Schema> schema_;
};

class TestBlockTransformProvider final : public IBlockTransformOperatorV1 {
 public:
    std::string Category() const override { return "test"; }
    std::string Name() const override { return "transform"; }
    std::string Description() const override { return "interface fixture"; }

    int CreateTask(const BlockTransformTaskConfigV1& config,
                   IBlockTransformTaskV1** task) override {
        if (!task || !config.task_id || !config.with_params_json ||
            !config.pushed_filter_plan_json ||
            config.contract_version != kBlockTransformContractVersionV1) {
            return -1;
        }
        *task = nullptr;
        if (create_return_code != 0) return create_return_code;
        if (return_null_task) return 0;
        *task = new TestBlockTransformTask(config);
        ++live_tasks;
        return 0;
    }

    void ReleaseTask(IBlockTransformTaskV1* task) override {
        delete task;
        --live_tasks;
    }

    int live_tasks = 0;
    int create_return_code = 0;
    bool return_null_task = false;
};

// ============================================================
// Test 7.2: stage filter and block transform interface contracts
// ============================================================
void test_stage_filter_interfaces() {
    printf("[TEST] stage filter interfaces...\n");

    LegacyBlockOperatorAbiFixture legacy_operator;
    assert(legacy_operator.Name() == "legacy_abi");
    using PushdownMethod = int (IFilterPushdownV1::*)(
        const FilterPushdownRequestV1&, FilterPushdownResultV1*) const;
    const PushdownMethod pushdown_method = &IFilterPushdownV1::EvaluatePushdown;
    assert(pushdown_method != nullptr);
    assert(sizeof(IID_FILTER_PUSHDOWN_V1) == sizeof(Guid));
    assert(sizeof(IID_BLOCK_TRANSFORM_OPERATOR_V1) == sizeof(Guid));
    assert(memcmp(&IID_FILTER_PUSHDOWN_V1, &IID_BLOCK_TRANSFORM_OPERATOR_V1, sizeof(Guid)) != 0);

    auto schema = arrow::schema({arrow::field("value", arrow::int64())});
    TestFilterPushdown pushdown;
    FilterPushdownRequestV1 request;
    request.target_kind = FilterPushdownTargetKindV1::kTransform;
    request.target_category = "test";
    request.target_name = "transform";
    request.output_schema = schema;
    request.canonical_plan_json = "{\"version\":1}";
    request.candidate_node_ids = {2, 5};
    FilterPushdownResultV1 result;
    assert(pushdown.EvaluatePushdown(request, &result) == 0);
    assert(result.accepted_node_ids == std::vector<uint32_t>({2}));

    TestBlockTransformProvider provider;
    BlockTransformTaskConfigV1 config;
    config.task_id = "task-1";
    config.with_params_json = "{}";
    config.pushed_filter_plan_json = "{\"version\":1}";
    IBlockTransformTaskV1* task1 = nullptr;
    IBlockTransformTaskV1* task2 = nullptr;
    assert(provider.CreateTask(config, &task1) == 0);
    config.task_id = "task-2";
    assert(provider.CreateTask(config, &task2) == 0);
    assert(task1 && task2 && task1 != task2 && provider.live_tasks == 2);

    std::shared_ptr<arrow::Schema> output_schema;
    assert(task1->Open(schema, &output_schema) == 0);
    assert(output_schema && output_schema->Equals(schema));
    auto batch = arrow::RecordBatch::Make(schema, 0, {std::make_shared<arrow::Int64Array>(0, nullptr)});
    std::vector<BlockTransformOutputV1> outputs;
    assert(task1->ProcessBlock(batch, 123, &outputs) == 0);
    assert(outputs.size() == 1 && outputs[0].batch == batch && outputs[0].ts_ms == 123);
    outputs.clear();
    assert(task1->Flush(&outputs) == 0 && outputs.empty());
    task2->Cancel();
    assert(task2->ProcessBlock(batch, 0, &outputs) < 0);

    provider.ReleaseTask(task1);
    provider.ReleaseTask(task2);
    assert(provider.live_tasks == 0);

    printf("[PASS] stage filter interfaces\n");
}

// ============================================================
// Test 7.8: task-owned exact plan and shared-source isolation
// ============================================================
void test_filter_task_session_isolation() {
    printf("[TEST] filter task session isolation...\n");
    auto schema = arrow::schema({
        arrow::field("id", arrow::int32(), false),
        arrow::field("enabled", arrow::boolean(), true),
    });
    auto bind = [&](const std::string& text) {
        std::shared_ptr<FilterExpr> parsed;
        std::string bind_error;
        assert(ParseFilterExpression(text, &parsed, &bind_error));
        std::shared_ptr<const BoundFilterExpr> bound;
        assert(BindFilterExpression(schema, parsed, &bound, &bind_error) ==
               FilterBindError::kNone);
        assert(bound && bind_error.empty());
        return bound;
    };

    const auto original = bind("id >= 42 AND enabled");
    FilterPushdownTarget target;
    target.kind = FilterPushdownTargetKindV1::kTransform;
    target.category = "test";
    target.name = "transform";
    target.output_schema = schema;
    RecordingFilterPushdownProvider owner;
    owner.return_code = 0;
    owner.accepted_node_ids = {2};
    RecordingFilterPushdownQuerier querier;
    querier.providers = {&owner};
    FilterPushdownNegotiation negotiation;
    std::string error;
    assert(NegotiateFilterPushdown(&querier, target, original, &negotiation, &error) ==
           FilterPlanError::kNone);

    FilterTaskSessionPlan exclusive;
    assert(MaterializeFilterTaskSessionPlan(
               schema,
               original,
               negotiation,
               FilterTaskIsolation::kExclusive,
               &exclusive,
               &error) == FilterPlanError::kNone);
    assert(error.empty() && exclusive.pushdown_enabled);
    assert(exclusive.accepted_node_ids == std::vector<uint32_t>({2}));
    assert(exclusive.pushed_expression && exclusive.pushed_expression->node_id == 2);
    assert(exclusive.residual_expression && exclusive.residual_expression->node_id == 5);
    assert(exclusive.pushed_filter_plan_json != negotiation.canonical_plan_json);
    rapidjson::Document exact_plan;
    exact_plan.Parse(exclusive.pushed_filter_plan_json.c_str());
    assert(!exact_plan.HasParseError() && exact_plan["version"].GetUint() == 1);
    assert(exact_plan["root"].IsObject());
    assert(exact_plan["root"]["node_id"].GetUint() == 2);
    assert(std::string(exact_plan["root"]["kind"].GetString()) == "compare");

    RecordingFilterPushdownProvider empty_owner;
    empty_owner.return_code = 0;
    RecordingFilterPushdownQuerier empty_querier;
    empty_querier.providers = {&empty_owner};
    FilterPushdownNegotiation empty_negotiation;
    assert(NegotiateFilterPushdown(
               &empty_querier, target, original, &empty_negotiation, &error) ==
           FilterPlanError::kNone);
    FilterTaskSessionPlan empty_exclusive;
    assert(MaterializeFilterTaskSessionPlan(
               schema,
               original,
               empty_negotiation,
               FilterTaskIsolation::kExclusive,
               &empty_exclusive,
               &error) == FilterPlanError::kNone);
    assert(!empty_exclusive.pushdown_enabled);
    assert(empty_exclusive.accepted_node_ids.empty());
    assert(!empty_exclusive.pushed_expression);
    assert(empty_exclusive.residual_expression == original);
    assert(empty_exclusive.pushed_filter_plan_json == kEmptyCanonicalFilterPlanV1);
    rapidjson::Document empty_plan;
    empty_plan.Parse(empty_exclusive.pushed_filter_plan_json.c_str());
    assert(!empty_plan.HasParseError() && empty_plan["root"].IsNull());

    FilterTaskSessionPlan shared_first;
    assert(MaterializeFilterTaskSessionPlan(
               schema,
               original,
               negotiation,
               FilterTaskIsolation::kSharedSource,
               &shared_first,
               &error) == FilterPlanError::kNone);
    assert(!shared_first.pushdown_enabled && shared_first.accepted_node_ids.empty());
    assert(!shared_first.pushed_expression && shared_first.residual_expression == original);
    assert(shared_first.pushed_filter_plan_json == kEmptyCanonicalFilterPlanV1);

    const auto other_original = bind("id < 10");
    RecordingFilterPushdownProvider other_owner;
    other_owner.return_code = 0;
    other_owner.accepted_node_ids = {1};
    RecordingFilterPushdownQuerier other_querier;
    other_querier.providers = {&other_owner};
    FilterPushdownNegotiation other_negotiation;
    assert(NegotiateFilterPushdown(
               &other_querier, target, other_original, &other_negotiation, &error) ==
           FilterPlanError::kNone);
    FilterTaskSessionPlan shared_second;
    assert(MaterializeFilterTaskSessionPlan(
               schema,
               other_original,
               other_negotiation,
               FilterTaskIsolation::kSharedSource,
               &shared_second,
               &error) == FilterPlanError::kNone);
    assert(!shared_second.pushdown_enabled && shared_second.accepted_node_ids.empty());
    assert(shared_second.residual_expression == other_original);
    assert(shared_first.residual_expression == original);

    auto expect_cleared = [](const FilterTaskSessionPlan& plan) {
        assert(!plan.pushdown_enabled && plan.pushed_filter_plan_json.empty());
        assert(plan.accepted_node_ids.empty());
        assert(!plan.pushed_expression && !plan.residual_expression);
    };
    auto prime = [&]() {
        FilterTaskSessionPlan plan;
        plan.pushdown_enabled = true;
        plan.pushed_filter_plan_json = "stale";
        plan.accepted_node_ids = {999};
        plan.pushed_expression = original;
        plan.residual_expression = original;
        return plan;
    };
    auto stale_negotiation = negotiation;
    stale_negotiation.canonical_plan_json = "{\"version\":1,\"root\":null}";
    auto rejected = prime();
    assert(MaterializeFilterTaskSessionPlan(
               schema,
               original,
               stale_negotiation,
               FilterTaskIsolation::kExclusive,
               &rejected,
               &error) == FilterPlanError::kInvalidNegotiation);
    expect_cleared(rejected);
    assert(!error.empty());

    stale_negotiation = negotiation;
    stale_negotiation.split.candidate_node_ids = {999};
    rejected = prime();
    assert(MaterializeFilterTaskSessionPlan(
               schema,
               original,
               stale_negotiation,
               FilterTaskIsolation::kExclusive,
               &rejected,
               &error) == FilterPlanError::kInvalidNegotiation);
    expect_cleared(rejected);

    TestBlockTransformProvider transform_provider;
    const std::string exact_json = exclusive.pushed_filter_plan_json;
    IBlockTransformTaskV1* task_one = nullptr;
    assert(CreateBlockTransformTaskSession(
               &transform_provider,
               "task-one",
               "{\"mode\":\"fast\"}",
               exclusive,
               &task_one,
               &error) == FilterPlanError::kNone);
    assert(task_one && transform_provider.live_tasks == 1);
    exclusive.pushed_filter_plan_json = "caller storage changed";
    auto* copied_one = dynamic_cast<TestBlockTransformTask*>(task_one);
    assert(copied_one);
    assert(copied_one->ContractVersion() == kBlockTransformContractVersionV1);
    assert(copied_one->TaskId() == "task-one");
    assert(copied_one->WithParamsJson() == "{\"mode\":\"fast\"}");
    assert(copied_one->PushedFilterPlanJson() == exact_json);

    IBlockTransformTaskV1* task_two = nullptr;
    assert(CreateBlockTransformTaskSession(
               &transform_provider,
               "task-two",
               "{}",
               shared_first,
               &task_two,
               &error) == FilterPlanError::kNone);
    auto* copied_two = dynamic_cast<TestBlockTransformTask*>(task_two);
    assert(copied_two && task_two != task_one && transform_provider.live_tasks == 2);
    assert(copied_two->TaskId() == "task-two");
    assert(copied_two->PushedFilterPlanJson() == kEmptyCanonicalFilterPlanV1);
    assert(copied_one->TaskId() == "task-one" && copied_one->PushedFilterPlanJson() == exact_json);

    transform_provider.ReleaseTask(task_one);
    transform_provider.ReleaseTask(task_two);
    assert(transform_provider.live_tasks == 0);

    transform_provider.create_return_code = EIO;
    task_one = reinterpret_cast<IBlockTransformTaskV1*>(1);
    assert(CreateBlockTransformTaskSession(
               &transform_provider,
               "task-failed",
               "{}",
               shared_first,
               &task_one,
               &error) == FilterPlanError::kTaskSessionCreationFailed);
    assert(!task_one && !error.empty() && transform_provider.live_tasks == 0);

    transform_provider.create_return_code = 0;
    transform_provider.return_null_task = true;
    assert(CreateBlockTransformTaskSession(
               &transform_provider,
               "task-null",
               "{}",
               shared_first,
               &task_one,
               &error) == FilterPlanError::kTaskSessionCreationFailed);
    assert(!task_one && !error.empty() && transform_provider.live_tasks == 0);

    printf("[PASS] filter task session isolation\n");
}

// ============================================================
// Test 7.3: sql_text splitter（Story 14.13）
// ============================================================
void test_sql_text_splitter() {
    printf("[TEST] sql_text splitter...\n");

    {
        std::vector<std::string> sqls;
        SqlTextSplitError err;
        assert(SplitSqlText("SELECT 1; SELECT 2;", &sqls, &err) == 0);
        assert(sqls.size() == 2);
        assert(sqls[0] == "SELECT 1");
        assert(sqls[1] == "SELECT 2");
    }

    {
        std::vector<std::string> sqls;
        SqlTextSplitError err;
        assert(SplitSqlText("SELECT 1\nSELECT 2", &sqls, &err) == 0);
        assert(sqls.size() == 1);
        assert(sqls[0] == "SELECT 1\nSELECT 2");
    }

    {
        std::vector<std::string> sqls;
        SqlTextSplitError err;
        const std::string text =
            "SELECT ';' AS a, \"x;\" AS b, `c;d` AS c;\n"
            "SELECT 2;";
        assert(SplitSqlText(text, &sqls, &err) == 0);
        assert(sqls.size() == 2);
    }

    {
        std::vector<std::string> sqls;
        SqlTextSplitError err;
        const std::string text =
            "SELECT 1 -- ; in line comment\n"
            ";\n"
            "SELECT /* ; in block comment */ 2;";
        assert(SplitSqlText(text, &sqls, &err) == 0);
        assert(sqls.size() == 2);
    }

    {
        std::vector<std::string> sqls;
        SqlTextSplitError err;
        assert(SplitSqlText("SELECT 1;;SELECT 2", &sqls, &err) != 0);
        assert(err.statement_index == 1);
    }

    {
        std::vector<std::string> sqls;
        SqlTextSplitError err;
        assert(SplitSqlText("SELECT 'abc; SELECT 2", &sqls, &err) != 0);
        assert(err.statement_index == 0);
    }

    printf("[PASS] sql_text splitter\n");
}

// ============================================================
// Test 8: IOperator 多输入默认回退（Span -> inputs[0]）
// ============================================================
void test_operator_multi_input_fallback() {
    printf("[TEST] IOperator multi-input fallback...\n");

    class SingleOnlyOperator : public IOperator {
     public:
        std::string Category() override { return "test"; }
        std::string Name() override { return "single_only"; }
        std::string Description() override { return "single input only"; }
        OperatorPosition Position() override { return OperatorPosition::DATA; }
        int Work(IChannel* in, IChannel* out) override {
            last_in = in;
            last_out = out;
            ++work_count;
            return 0;
        }
        int Configure(const char*, const char*) override { return 0; }

        IChannel* last_in = nullptr;
        IChannel* last_out = nullptr;
        int work_count = 0;
    };

    MemoryChannel in1;
    MemoryChannel in2;
    DataFrameChannel out("test", "sink");
    in1.SetIdentity("test", "in1");
    in2.SetIdentity("test", "in2");

    SingleOnlyOperator op;
    IOperator& op_iface = op;
    std::vector<IChannel*> inputs = {&in1, &in2};
    int rc = op_iface.Work(Span<IChannel*>(inputs), &out);
    assert(rc == 0);
    assert(op.work_count == 1);
    assert(op.last_in == &in1);
    assert(op.last_out == &out);

    std::vector<IChannel*> empty_inputs;
    rc = op_iface.Work(Span<IChannel*>(empty_inputs), &out);
    assert(rc != 0);

    printf("[PASS] IOperator multi-input fallback\n");
}

// ============================================================
// Test 9: Span 基础语义（empty + 越界 assert）
// ============================================================
void test_span_safety() {
    printf("[TEST] Span safety...\n");

    std::vector<int> empty_data;
    Span<int> empty_span(empty_data);
    assert(empty_span.empty());

    std::vector<int> one = {7};
    Span<int> one_span(one);
    assert(!one_span.empty());
    assert(one_span[0] == 7);

#ifndef _WIN32
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        volatile int v = one_span[1];
        (void)v;
        _exit(0);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGABRT);
#endif

    printf("[PASS] Span safety\n");
}

// ============================================================
// Test 8: NormalizeFromTableName (Story 4.5)
//
// 注意（TQ-8）：scheduler_plugin.cpp 中的 ExtractTableName/BuildQuery 是
// static 函数，无法从测试直接调用。这里复制了等价实现，并通过对比注释
// 确保与生产代码逻辑完全一致。如后续重构将其提取为公开函数，应改为直接调用。
// ============================================================

// 与 scheduler_plugin.cpp ExtractTableName 逻辑完全一致
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
    printf("[TEST] ExtractTableName (NormalizeFromTableName)...\n");

    // 验证 ExtractTableName_Test 与生产代码 ExtractTableName 逻辑一致
    assert(ExtractTableName_Test("sqlite.mydb.users") == "users");       // 三段式
    assert(ExtractTableName_Test("mydb.users") == "users");              // 两段式
    assert(ExtractTableName_Test("users") == "users");                   // 一段式
    assert(ExtractTableName_Test("catalog.db.table") == "table");        // 三段式
    assert(ExtractTableName_Test("a.b") == "b");                         // 两段式边界
    printf("  ExtractTableName: all cases OK\n");
    printf("[PASS] ExtractTableName (NormalizeFromTableName)\n");
}

// ============================================================
// Test 9: BuildQuery integration test (Story 4.5)
//
// 注意（TQ-9）：scheduler_plugin.cpp 中的 BuildQuery 是 static 函数，
// 无法从测试直接调用。这里复制了等价实现，逻辑与生产代码完全一致。
// 如后续重构将其提取为公开函数，应改为直接调用。
// ============================================================

// 与 scheduler_plugin.cpp BuildQuery 逻辑完全一致
static std::string BuildQuery_Test(const std::string& source_name, const SqlStatement& stmt) {
    std::string sql = stmt.sql_part;
    std::string table = ExtractTableName_Test(source_name);
    std::regex FROM_PATTERN(R"((\bFROM\s+)((?:[\w]+\.)*[\w]+))");
    std::smatch m;
    if (std::regex_search(sql, m, FROM_PATTERN)) {
        sql = sql.substr(0, m.position()) + m[1].str() + table +
              sql.substr(m.position() + m.length());
    }
    return sql;
}

void test_build_query_integration() {
    printf("[TEST] BuildQuery integration (Story 4.5)...\n");
    SqlParser parser;

    // Test 1: Database channel with GROUP BY/ORDER BY
    // sql_part 保留原始表名（不做占位符替换），BuildQuery 负责替换
    {
        auto stmt = parser.Parse("SELECT a, COUNT(*) FROM sqlite.mydb.logs GROUP BY a ORDER BY COUNT(*) DESC USING ml.predict");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT a, COUNT(*) FROM sqlite.mydb.logs GROUP BY a ORDER BY COUNT(*) DESC");

        std::string query = BuildQuery_Test("sqlite.mydb.logs", stmt);
        assert(query == "SELECT a, COUNT(*) FROM logs GROUP BY a ORDER BY COUNT(*) DESC");
        printf("  [PASS] Database channel with GROUP BY/ORDER BY\n");
    }

    // Test 2: Database channel with subquery
    // 只替换第一个 FROM（主查询），子查询 FROM 保持原样
    {
        auto stmt = parser.Parse("SELECT * FROM sqlite.mydb.users WHERE id IN (SELECT user_id FROM sqlite.mydb.orders)");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT * FROM sqlite.mydb.users WHERE id IN (SELECT user_id FROM sqlite.mydb.orders)");

        std::string query = BuildQuery_Test("sqlite.mydb.users", stmt);
        // 只替换主查询的 FROM，子查询 FROM sqlite.mydb.orders 不变
        assert(query == "SELECT * FROM users WHERE id IN (SELECT user_id FROM sqlite.mydb.orders)");
        printf("  [PASS] Database channel with subquery (first FROM only)\n");
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
        assert(stmt.sql_part == "SELECT a, COUNT(*) FROM catalog.db.table WHERE x > 1 GROUP BY a HAVING COUNT(*) > 5 ORDER BY a LIMIT 10");
        assert(stmt.op_category == "ml");
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
// Test 11: ChannelType 常量验证（P3-3 修复）
// ============================================================
void test_channel_type_constants() {
    printf("[TEST] ChannelType constants...\n");
    assert(std::string(ChannelType::kDataFrame) == "dataframe");
    assert(std::string(ChannelType::kDatabase) == "database");

    // DataFrameChannel 应返回 kDataFrame
    DataFrameChannel ch("test", "t");
    assert(std::string(ch.Type()) == ChannelType::kDataFrame);

    printf("[PASS] ChannelType constants\n");
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
// Test 12: JSON Error Builder（Sprint15 P2-2）
// ============================================================
void test_json_error_builder() {
    printf("[TEST] JSON error builder...\n");

    {
        rapidjson::Document d;
        const std::string json = BuildErrorJson("simple");
        d.Parse(json.c_str());
        assert(!d.HasParseError() && d.IsObject());
        assert(d.HasMember("error") && d["error"].IsString());
        assert(std::string(d["error"].GetString()) == "simple");
        assert(!d.HasMember("error_code"));
    }

    {
        rapidjson::Document d;
        const std::string json = BuildExecutionErrorWithSqlIndexJson(
            "exec failed", "OP_EXEC_FAIL", "execute", 3);
        d.Parse(json.c_str());
        assert(!d.HasParseError() && d.IsObject());
        assert(std::string(d["error"].GetString()) == "exec failed");
        assert(std::string(d["error_code"].GetString()) == "OP_EXEC_FAIL");
        assert(std::string(d["error_stage"].GetString()) == "execute");
        assert(d["sql_index"].IsUint64() && d["sql_index"].GetUint64() == 3);
    }

    {
        assert(std::string(ToErrorCode(ErrorCodeId::kOpExecFail)) == "OP_EXEC_FAIL");
        assert(std::string(ToErrorStage(ErrorStageId::kLease)) == "lease");

        rapidjson::Document d;
        const std::string json = BuildExecutionErrorWithSqlIndexJson(
            "typed exec failed",
            ErrorCodeId::kStreamChannelMutating,
            ErrorStageId::kModify,
            1);
        d.Parse(json.c_str());
        assert(!d.HasParseError() && d.IsObject());
        assert(std::string(d["error"].GetString()) == "typed exec failed");
        assert(std::string(d["error_code"].GetString()) == "STREAM_CHANNEL_MUTATING");
        assert(std::string(d["error_stage"].GetString()) == "modify");
        assert(d["sql_index"].IsUint64() && d["sql_index"].GetUint64() == 1);
    }

    {
        rapidjson::Document d;
        const std::string json = BuildErrorWithCodeAndSqlIndexJson(
            "typed parse failed",
            ErrorCodeId::kSqlTextInvalid,
            2);
        d.Parse(json.c_str());
        assert(!d.HasParseError() && d.IsObject());
        assert(std::string(d["error"].GetString()) == "typed parse failed");
        assert(std::string(d["error_code"].GetString()) == "SQL_TEXT_INVALID");
        assert(d["sql_index"].IsUint64() && d["sql_index"].GetUint64() == 2);
    }

    {
        StreamChannelCapabilities source_caps;
        source_caps.channel_type = "ring";
        source_caps.concurrency.put_mode = ProducerMode::SINGLE;
        source_caps.concurrency.poll_mode = ConsumerMode::MULTI;
        source_caps.concurrency.max_producers = 1;
        source_caps.concurrency.max_consumers = 4;
        source_caps.concurrency.lock_free_put = true;
        source_caps.concurrency.lock_free_poll = true;
        source_caps.concurrency.cancel_wakeup_guaranteed = true;

        StreamChannelCapabilities sink_caps;
        sink_caps.channel_type = "ring";
        sink_caps.concurrency.put_mode = ProducerMode::MULTI;
        sink_caps.concurrency.poll_mode = ConsumerMode::SINGLE;
        sink_caps.concurrency.max_producers = 8;
        sink_caps.concurrency.max_consumers = 1;
        sink_caps.concurrency.lock_free_put = true;
        sink_caps.concurrency.lock_free_poll = false;
        sink_caps.concurrency.cancel_wakeup_guaranteed = true;

        rapidjson::Document d;
        const std::string json = BuildCapabilityMismatchJson(
            "cap mismatch", "STREAM_SOURCE_CAPABILITY_MISMATCH", &source_caps, &sink_caps);
        d.Parse(json.c_str());
        assert(!d.HasParseError() && d.IsObject());
        assert(std::string(d["error_stage"].GetString()) == "capability_check");
        assert(d.HasMember("details") && d["details"].IsObject());
        assert(d["details"].HasMember("capabilities") && d["details"]["capabilities"].IsObject());
        assert(d["details"]["capabilities"].HasMember("source"));
        assert(d["details"]["capabilities"].HasMember("sink"));
    }

    {
        rapidjson::Document d;
        const std::string json = BuildSinkCapabilityMismatchJson(
            "sink mismatch", "capability_check", "stream.out", 3, ProducerMode::SINGLE, 1);
        d.Parse(json.c_str());
        assert(!d.HasParseError() && d.IsObject());
        assert(std::string(d["error_code"].GetString()) == "STREAM_GROUP_SINK_CAPABILITY_MISMATCH");
        assert(std::string(d["sink_key"].GetString()) == "stream.out");
        assert(d["required"].IsObject());
        assert(d["required"]["writers"].GetUint() == 3);
        assert(d["actual"].IsObject());
        assert(std::string(d["actual"]["put_mode"].GetString()) == "SINGLE");
    }

    {
        rapidjson::Document d;
        const std::vector<std::string> expected = {"stream.a", "stream.c"};
        const std::vector<std::string> actual = {"stream.d", "stream.c"};
        const std::string json = BuildSourceMismatchErrorJson(
            "source mismatch", "share_set_validate", "s1", "node1", expected, actual);
        d.Parse(json.c_str());
        assert(!d.HasParseError() && d.IsObject());
        assert(std::string(d["error_code"].GetString()) == "STREAM_GROUP_SOURCE_MISMATCH");
        assert(d["missing_keys"].IsArray());
        assert(d["missing_keys"].Size() == 1);
        assert(std::string(d["missing_keys"][0].GetString()) == "stream.a");
        assert(d["extra_keys"].IsArray());
        assert(d["extra_keys"].Size() == 1);
        assert(std::string(d["extra_keys"][0].GetString()) == "stream.d");
    }

    printf("[PASS] JSON error builder\n");
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
    test_dataframe_channel_append();
    test_sql_parser();
    test_filter_expression_parser();
    test_filter_schema_binding();
    test_filter_mask_evaluation();
    test_filter_record_batch();
    test_block_filter_stage();
    test_block_transform_pipeline_runner();
    test_filter_pushdown_split();
    test_filter_pushdown_negotiation();
    test_statement_stage_filters();
    test_offline_filter_public_contracts();
    test_offline_filter_reader_factory_contracts();
    test_stage_filter_interfaces();
    test_filter_task_session_isolation();
    test_sql_text_splitter();
    test_operator_multi_input_fallback();
    test_span_safety();
    test_normalize_from_table_name();
    test_build_query_integration();
    test_channel_adapter_copy();
    test_channel_type_constants();
    test_json_error_builder();

    test_pipeline();

    printf("\n=== All tests passed ===\n");
    return 0;
}
