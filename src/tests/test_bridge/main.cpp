#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <arrow/api.h>

#include "services/bridge/arrow_ipc_serializer.h"
#include "framework/core/dataframe.h"
#include "framework/interfaces/idata_entity.h"

using namespace flowsql;
using namespace flowsql::bridge;

// ============================================================
// 测试 1: Arrow IPC 序列化/反序列化
// ============================================================
int test_arrow_ipc_serializer() {
    printf("\n=== Test: Arrow IPC Serializer ===\n");

    // 构建测试 RecordBatch
    auto schema = arrow::schema({
        arrow::field("id", arrow::int32()),
        arrow::field("name", arrow::utf8()),
        arrow::field("value", arrow::float64()),
    });

    arrow::Int32Builder id_builder;
    arrow::StringBuilder name_builder;
    arrow::DoubleBuilder value_builder;

    id_builder.AppendValues({1, 2, 3});
    name_builder.AppendValues({"alpha", "beta", "gamma"});
    value_builder.AppendValues({1.1, 2.2, 3.3});

    std::shared_ptr<arrow::Array> id_arr, name_arr, value_arr;
    id_builder.Finish(&id_arr);
    name_builder.Finish(&name_arr);
    value_builder.Finish(&value_arr);

    auto batch = arrow::RecordBatch::Make(schema, 3, {id_arr, name_arr, value_arr});

    // 序列化
    std::string ipc_data;
    int ret = ArrowIpcSerializer::Serialize(batch, &ipc_data);
    if (ret != 0) {
        printf("FAIL: Serialize returned %d\n", ret);
        return -1;
    }
    printf("Serialized %d rows to %zu bytes\n", batch->num_rows(), ipc_data.size());

    // 反序列化
    std::shared_ptr<arrow::RecordBatch> restored;
    ret = ArrowIpcSerializer::Deserialize(ipc_data, &restored);
    if (ret != 0) {
        printf("FAIL: Deserialize returned %d\n", ret);
        return -1;
    }

    // 验证
    if (restored->num_rows() != 3) {
        printf("FAIL: expected 3 rows, got %lld\n", (long long)restored->num_rows());
        return -1;
    }
    if (restored->num_columns() != 3) {
        printf("FAIL: expected 3 columns, got %d\n", restored->num_columns());
        return -1;
    }
    if (!restored->Equals(*batch)) {
        printf("FAIL: restored batch != original batch\n");
        return -1;
    }

    printf("PASS: Arrow IPC round-trip OK (3 rows, %zu bytes)\n", ipc_data.size());
    return 0;
}

// ============================================================
// 测试 2: DataFrame → Arrow IPC → DataFrame 往返
// ============================================================
int test_dataframe_ipc_roundtrip() {
    printf("\n=== Test: DataFrame IPC Round-trip ===\n");

    // 创建源 DataFrame
    DataFrame src;
    src.SetSchema({
        {"id", DataType::INT32, 0, ""},
        {"protocol", DataType::STRING, 0, ""},
        {"bytes", DataType::INT64, 0, ""},
    });
    src.AppendRow({int32_t(1), std::string("HTTP"), int64_t(1024)});
    src.AppendRow({int32_t(2), std::string("DNS"), int64_t(64)});
    src.AppendRow({int32_t(3), std::string("TLS"), int64_t(2048)});

    // DataFrame → Arrow → IPC bytes
    auto batch = src.ToArrow();
    if (!batch) {
        printf("FAIL: ToArrow() returned null\n");
        return -1;
    }

    std::string ipc_data;
    if (ArrowIpcSerializer::Serialize(batch, &ipc_data) != 0) {
        printf("FAIL: Serialize failed\n");
        return -1;
    }

    // IPC bytes → Arrow → DataFrame
    std::shared_ptr<arrow::RecordBatch> restored_batch;
    if (ArrowIpcSerializer::Deserialize(ipc_data, &restored_batch) != 0) {
        printf("FAIL: Deserialize failed\n");
        return -1;
    }

    DataFrame dst;
    dst.FromArrow(restored_batch);

    // 验证
    if (dst.RowCount() != 3) {
        printf("FAIL: expected 3 rows, got %d\n", dst.RowCount());
        return -1;
    }

    auto row0 = dst.GetRow(0);
    if (std::get<int32_t>(row0[0]) != 1) {
        printf("FAIL: row0[0] != 1\n");
        return -1;
    }
    if (std::get<std::string>(row0[1]) != "HTTP") {
        printf("FAIL: row0[1] != HTTP\n");
        return -1;
    }
    if (std::get<int64_t>(row0[2]) != 1024) {
        printf("FAIL: row0[2] != 1024\n");
        return -1;
    }

    printf("PASS: DataFrame IPC round-trip OK (3 rows)\n");
    return 0;
}

// ============================================================
// 测试 3: 空 RecordBatch 序列化
// ============================================================
int test_empty_batch() {
    printf("\n=== Test: Empty RecordBatch ===\n");

    auto schema = arrow::schema({arrow::field("x", arrow::int32())});
    auto batch = arrow::RecordBatch::Make(schema, 0, {std::make_shared<arrow::Int32Array>(0, nullptr)});

    // 构建空数组
    arrow::Int32Builder builder;
    std::shared_ptr<arrow::Array> arr;
    builder.Finish(&arr);
    batch = arrow::RecordBatch::Make(schema, 0, {arr});

    std::string ipc_data;
    if (ArrowIpcSerializer::Serialize(batch, &ipc_data) != 0) {
        printf("FAIL: Serialize empty batch failed\n");
        return -1;
    }

    std::shared_ptr<arrow::RecordBatch> restored;
    if (ArrowIpcSerializer::Deserialize(ipc_data, &restored) != 0) {
        printf("FAIL: Deserialize empty batch failed\n");
        return -1;
    }

    if (restored->num_rows() != 0) {
        printf("FAIL: expected 0 rows, got %lld\n", (long long)restored->num_rows());
        return -1;
    }

    printf("PASS: Empty RecordBatch round-trip OK\n");
    return 0;
}

// ============================================================
// 测试 4: 错误处理
// ============================================================
int test_error_handling() {
    printf("\n=== Test: Error Handling ===\n");

    // null batch
    std::string out;
    if (ArrowIpcSerializer::Serialize(nullptr, &out) != -1) {
        printf("FAIL: Serialize(null) should return -1\n");
        return -1;
    }

    // null output
    auto schema = arrow::schema({arrow::field("x", arrow::int32())});
    arrow::Int32Builder builder;
    builder.Append(42);
    std::shared_ptr<arrow::Array> arr;
    builder.Finish(&arr);
    auto batch = arrow::RecordBatch::Make(schema, 1, {arr});
    if (ArrowIpcSerializer::Serialize(batch, nullptr) != -1) {
        printf("FAIL: Serialize(batch, null) should return -1\n");
        return -1;
    }

    // empty data
    std::shared_ptr<arrow::RecordBatch> restored;
    if (ArrowIpcSerializer::Deserialize("", &restored) != -1) {
        printf("FAIL: Deserialize('') should return -1\n");
        return -1;
    }

    // invalid data
    if (ArrowIpcSerializer::Deserialize("not arrow ipc", &restored) != -1) {
        printf("FAIL: Deserialize(garbage) should return -1\n");
        return -1;
    }

    printf("PASS: Error handling OK\n");
    return 0;
}

// ============================================================
int main() {
    printf("========================================\n");
    printf("  FlowSQL Bridge Tests\n");
    printf("========================================\n");

    int failures = 0;
    if (test_arrow_ipc_serializer() != 0) failures++;
    if (test_dataframe_ipc_roundtrip() != 0) failures++;
    if (test_empty_batch() != 0) failures++;
    if (test_error_handling() != 0) failures++;

    printf("\n========================================\n");
    if (failures == 0) {
        printf("  All tests PASSED\n");
    } else {
        printf("  %d test(s) FAILED\n", failures);
    }
    printf("========================================\n");

    return failures;
}
