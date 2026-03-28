# C++ 算子插件 Sample

该目录提供一个可直接编译的 C++ 算子插件示例，开发者可以以此为模板开发自己的 `.so` 插件。
示例算子为 `sample.column_stats`，输入 DataFrame 后输出按列统计结果（`count/min/max/mean`）。

## 目录

- `sample_operator.h/.cpp`：`ColumnStatsOperator` 算子实现（`IOperator` 子类）
- `plugin_exports.cpp`：插件导出符号（ABI + 算子工厂）
- `CMakeLists.txt`：独立构建脚本

## ABI 约束

插件必须导出以下 4 个符号（`extern "C"`）：

1. `int flowsql_abi_version()`
2. `int flowsql_operator_count()`
3. `flowsql::IOperator* flowsql_create_operator(int index)`
4. `void flowsql_destroy_operator(flowsql::IOperator* op)`

当前示例使用 ABI 版本 `1`，与运行时 `binaddon` 插件管理器一致。

## 示例算子输出

`sample.column_stats` 输出表结构：

- `column_name`（STRING）
- `data_type`（STRING）
- `row_count`（INT64）
- `numeric_count`（INT64）
- `min`（DOUBLE）
- `max`（DOUBLE）
- `mean`（DOUBLE）

## 编译

在仓库根目录执行：

```bash
cmake -S samples/cpp_operator -B build/sample_cpp_operator -DFLOWSQL_INCLUDE_DIR=$(pwd)/src
cmake --build build/sample_cpp_operator -j
```

若自动探测不到 Arrow 头文件/库，可显式传入：

```bash
cmake -S samples/cpp_operator -B build/sample_cpp_operator \
  -DFLOWSQL_INCLUDE_DIR=$(pwd)/src \
  -DFLOWSQL_ARROW_INCLUDE_DIR=/path/to/arrow/include \
  -DFLOWSQL_ARROW_LIBRARY=/path/to/libarrow.so
```

生成产物：

`build/sample_cpp_operator/libsample_cpp_operator.so`

## 上传与激活（Web API）

1. 上传（`multipart/form-data`，字段：`file`，可附带 `type=cpp`）：

```bash
curl -X POST "http://127.0.0.1:8081/api/operators/upload" \
  -F "type=cpp" \
  -F "file=@build/sample_cpp_operator/libsample_cpp_operator.so"
```

2. 从响应中获取 `plugin_id` 后激活：

```bash
curl -X POST "http://127.0.0.1:8081/api/operators/activate" \
  -H "Content-Type: application/json" \
  -d '{"type":"cpp","plugin_id":"<plugin_id>"}'
```

3. 查询：

```bash
curl "http://127.0.0.1:8081/api/operators/list?type=cpp"
```
