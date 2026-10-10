// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "dataframe.h"

#include <arrow/builder.h>
#include <arrow/type.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <stdexcept>

namespace flowsql {

std::shared_ptr<arrow::DataType> ToArrowType(DataType type) {
    switch (type) {
        case DataType::INT32:     return arrow::int32();
        case DataType::INT64:     return arrow::int64();
        case DataType::UINT32:    return arrow::uint32();
        case DataType::UINT64:    return arrow::uint64();
        case DataType::FLOAT:     return arrow::float32();
        case DataType::DOUBLE:    return arrow::float64();
        case DataType::STRING:    return arrow::utf8();
        case DataType::BYTES:     return arrow::binary();
        case DataType::TIMESTAMP: return arrow::int64();
        case DataType::BOOLEAN:   return arrow::boolean();
        default:                  return arrow::utf8();
    }
}

DataType FromArrowType(const std::shared_ptr<arrow::DataType>& type) {
    switch (type->id()) {
        case arrow::Type::INT32:   return DataType::INT32;
        case arrow::Type::INT64:   return DataType::INT64;
        case arrow::Type::UINT32:  return DataType::UINT32;
        case arrow::Type::UINT64:  return DataType::UINT64;
        case arrow::Type::FLOAT:   return DataType::FLOAT;
        case arrow::Type::DOUBLE:  return DataType::DOUBLE;
        case arrow::Type::STRING:  return DataType::STRING;
        case arrow::Type::BINARY:  return DataType::BYTES;
        case arrow::Type::BOOL:    return DataType::BOOLEAN;
        default:                   return DataType::STRING;
    }
}

// --- Schema ---

std::vector<Field> DataFrame::GetSchema() const { return schema_; }

void DataFrame::SetSchema(const std::vector<Field>& schema) {
    schema_ = schema;
    batch_ = nullptr;  // 旧数据失效
    std::vector<std::shared_ptr<arrow::Field>> arrow_fields;
    arrow_fields.reserve(schema.size());
    for (auto& f : schema_) {
        arrow_fields.push_back(arrow::field(f.name, ToArrowType(f.type)));
    }
    arrow_schema_ = arrow::schema(arrow_fields);
    InitBuilders();
}

void DataFrame::InitBuilders() const {
    builders_.clear();
    if (!arrow_schema_) return;
    auto pool = arrow::default_memory_pool();
    for (int i = 0; i < arrow_schema_->num_fields(); ++i) {
        std::unique_ptr<arrow::ArrayBuilder> builder;
        auto status = arrow::MakeBuilder(pool, arrow_schema_->field(i)->type(), &builder);
        if (!status.ok()) {
            throw std::runtime_error("Failed to create Arrow builder: " + status.ToString());
        }
        builders_.push_back(std::move(builder));
    }
    pending_rows_ = 0;
}

// --- 行操作 ---

int32_t DataFrame::RowCount() const {
    if (pending_rows_ > 0) {
        Finalize();
    }
    return batch_ ? static_cast<int32_t>(batch_->num_rows()) : 0;
}

int DataFrame::AppendRow(const std::vector<FieldValue>& row) {
    if (builders_.empty()) return -1;
    if (static_cast<int>(row.size()) != static_cast<int>(schema_.size())) return -1;
    batch_ = nullptr;  // 进入构建期
    for (size_t i = 0; i < row.size(); ++i) {
        AppendValueToBuilder(static_cast<int>(i), row[i]);
    }
    ++pending_rows_;
    return 0;
}

std::vector<FieldValue> DataFrame::GetRow(int32_t index) const {
    if (pending_rows_ > 0) {
        Finalize();
    }
    std::vector<FieldValue> row;
    if (!batch_ || index < 0 || index >= batch_->num_rows()) return row;
    row.reserve(batch_->num_columns());
    for (int c = 0; c < batch_->num_columns(); ++c) {
        row.push_back(ExtractValue(batch_->column(c), index));
    }
    return row;
}

// --- 列操作 ---

std::vector<FieldValue> DataFrame::GetColumn(const std::string& name) const {
    if (pending_rows_ > 0) {
        Finalize();
    }
    std::vector<FieldValue> col;
    if (!batch_) return col;
    auto arr = batch_->GetColumnByName(name);
    if (!arr) return col;
    col.reserve(arr->length());
    for (int64_t i = 0; i < arr->length(); ++i) {
        col.push_back(ExtractValue(arr, static_cast<int>(i)));
    }
    return col;
}

// --- Arrow 互操作 ---

std::shared_ptr<arrow::RecordBatch> DataFrame::ToArrow() const {
    if (pending_rows_ > 0 || (!batch_ && arrow_schema_)) {
        Finalize();
    }
    return batch_;
}

void DataFrame::FromArrow(std::shared_ptr<arrow::RecordBatch> batch) {
    batch_ = std::move(batch);
    pending_rows_ = 0;
    builders_.clear();
    if (!batch_) {
        schema_.clear();
        arrow_schema_ = nullptr;
        return;
    }
    arrow_schema_ = batch_->schema();
    schema_.clear();
    schema_.reserve(arrow_schema_->num_fields());
    for (int i = 0; i < arrow_schema_->num_fields(); ++i) {
        auto& af = arrow_schema_->field(i);
        Field f;
        f.name = af->name();
        f.type = FromArrowType(af->type());
        schema_.push_back(std::move(f));
    }
}

// --- 序列化 ---

static const char* DataTypeToString(DataType t) {
    switch (t) {
        case DataType::INT32:     return "INT32";
        case DataType::INT64:     return "INT64";
        case DataType::UINT32:    return "UINT32";
        case DataType::UINT64:    return "UINT64";
        case DataType::FLOAT:     return "FLOAT";
        case DataType::DOUBLE:    return "DOUBLE";
        case DataType::STRING:    return "STRING";
        case DataType::BYTES:     return "BYTES";
        case DataType::TIMESTAMP: return "TIMESTAMP";
        case DataType::BOOLEAN:   return "BOOLEAN";
        default:                  return "STRING";
    }
}

static DataType StringToDataType(const std::string& s) {
    if (s == "INT32")     return DataType::INT32;
    if (s == "INT64")     return DataType::INT64;
    if (s == "UINT32")    return DataType::UINT32;
    if (s == "UINT64")    return DataType::UINT64;
    if (s == "FLOAT")     return DataType::FLOAT;
    if (s == "DOUBLE")    return DataType::DOUBLE;
    if (s == "STRING")    return DataType::STRING;
    if (s == "BYTES")     return DataType::BYTES;
    if (s == "TIMESTAMP") return DataType::TIMESTAMP;
    if (s == "BOOLEAN")   return DataType::BOOLEAN;
    return DataType::STRING;
}

std::string DataFrame::ToJson() const {
    if (pending_rows_ > 0) {
        Finalize();
    }
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();

    // columns
    w.Key("columns");
    w.StartArray();
    for (auto& f : schema_) w.String(f.name.c_str());
    w.EndArray();

    // types
    w.Key("types");
    w.StartArray();
    for (auto& f : schema_) w.String(DataTypeToString(f.type));
    w.EndArray();

    // data
    w.Key("data");
    w.StartArray();
    int32_t rows = batch_ ? static_cast<int32_t>(batch_->num_rows()) : 0;
    for (int32_t r = 0; r < rows; ++r) {
        w.StartArray();
        for (size_t c = 0; c < schema_.size(); ++c) {
            auto val = ExtractValue(batch_->column(static_cast<int>(c)), r);
            std::visit([&w](auto&& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, int32_t>) w.Int(v);
                else if constexpr (std::is_same_v<T, int64_t>) w.Int64(v);
                else if constexpr (std::is_same_v<T, uint32_t>) w.Uint(v);
                else if constexpr (std::is_same_v<T, uint64_t>) w.Uint64(v);
                else if constexpr (std::is_same_v<T, float>) w.Double(v);
                else if constexpr (std::is_same_v<T, double>) w.Double(v);
                else if constexpr (std::is_same_v<T, std::string>) w.String(v.c_str());
                else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) w.String(reinterpret_cast<const char*>(v.data()), v.size());
                else if constexpr (std::is_same_v<T, bool>) w.Bool(v);
            }, val);
        }
        w.EndArray();
    }
    w.EndArray();
    w.EndObject();
    return sb.GetString();
}

bool DataFrame::FromJson(const std::string& json) {
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    if (doc.HasParseError() || !doc.IsObject()) return false;
    if (!doc.HasMember("columns") || !doc.HasMember("types") || !doc.HasMember("data")) return false;

    auto& cols = doc["columns"];
    auto& types = doc["types"];
    auto& data = doc["data"];
    if (!cols.IsArray() || !types.IsArray() || !data.IsArray()) return false;
    if (cols.Size() != types.Size()) return false;

    std::vector<Field> schema;
    schema.reserve(cols.Size());
    for (rapidjson::SizeType i = 0; i < cols.Size(); ++i) {
        Field f;
        f.name = cols[i].GetString();
        f.type = StringToDataType(types[i].GetString());
        schema.push_back(std::move(f));
    }
    SetSchema(schema);

    for (rapidjson::SizeType r = 0; r < data.Size(); ++r) {
        auto& row_arr = data[r];
        if (!row_arr.IsArray() || row_arr.Size() != cols.Size()) continue;
        std::vector<FieldValue> row;
        row.reserve(schema_.size());
        for (rapidjson::SizeType c = 0; c < row_arr.Size(); ++c) {
            auto& v = row_arr[c];
            switch (schema_[c].type) {
                case DataType::INT32:     row.emplace_back(v.GetInt()); break;
                case DataType::INT64:
                case DataType::TIMESTAMP: row.emplace_back(v.GetInt64()); break;
                case DataType::UINT32:    row.emplace_back(v.GetUint()); break;
                case DataType::UINT64:    row.emplace_back(v.GetUint64()); break;
                case DataType::FLOAT:     row.emplace_back(static_cast<float>(v.GetDouble())); break;
                case DataType::DOUBLE:    row.emplace_back(v.GetDouble()); break;
                case DataType::STRING:    row.emplace_back(std::string(v.GetString())); break;
                case DataType::BYTES: {
                    auto s = v.GetString();
                    auto len = v.GetStringLength();
                    row.emplace_back(std::vector<uint8_t>(s, s + len));
                    break;
                }
                case DataType::BOOLEAN:   row.emplace_back(v.GetBool()); break;
                default:                  row.emplace_back(std::string(v.GetString())); break;
            }
        }
        AppendRow(row);
    }
    return true;
}

// --- Clear ---

void DataFrame::Clear() {
    batch_ = nullptr;
    pending_rows_ = 0;
    if (arrow_schema_) {
        InitBuilders();
    }
}

// --- Finalize: builders_ → batch_ ---

void DataFrame::Finalize() const {
    if ((pending_rows_ == 0 && batch_) || !arrow_schema_) return;
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    arrays.reserve(builders_.size());
    for (size_t idx = 0; idx < builders_.size(); ++idx) {
        auto& b = builders_[idx];
        std::shared_ptr<arrow::Array> arr;
        auto status = b->Finish(&arr);
        if (!status.ok()) {
            throw std::runtime_error("Arrow builder Finish failed: " + status.ToString());
        }
        arrays.push_back(std::move(arr));
    }
    batch_ = arrow::RecordBatch::Make(arrow_schema_, pending_rows_, std::move(arrays));
    pending_rows_ = 0;
    InitBuilders();  // 重新初始化 builders 以备下次使用
}

// --- AppendValueToBuilder ---

void DataFrame::AppendValueToBuilder(int col, const FieldValue& value) {
    auto& builder = builders_[col];
    auto type = schema_[col].type;
    arrow::Status status;
    switch (type) {
        case DataType::INT32:
            status = static_cast<arrow::Int32Builder*>(builder.get())->Append(std::get<int32_t>(value));
            break;
        case DataType::INT64:
        case DataType::TIMESTAMP:
            status = static_cast<arrow::Int64Builder*>(builder.get())->Append(std::get<int64_t>(value));
            break;
        case DataType::UINT32:
            status = static_cast<arrow::UInt32Builder*>(builder.get())->Append(std::get<uint32_t>(value));
            break;
        case DataType::UINT64:
            status = static_cast<arrow::UInt64Builder*>(builder.get())->Append(std::get<uint64_t>(value));
            break;
        case DataType::FLOAT:
            status = static_cast<arrow::FloatBuilder*>(builder.get())->Append(std::get<float>(value));
            break;
        case DataType::DOUBLE:
            status = static_cast<arrow::DoubleBuilder*>(builder.get())->Append(std::get<double>(value));
            break;
        case DataType::STRING:
            status = static_cast<arrow::StringBuilder*>(builder.get())->Append(std::get<std::string>(value));
            break;
        case DataType::BYTES: {
            auto& bytes = std::get<std::vector<uint8_t>>(value);
            status = static_cast<arrow::BinaryBuilder*>(builder.get())->Append(bytes.data(), static_cast<int32_t>(bytes.size()));
            break;
        }
        case DataType::BOOLEAN:
            status = static_cast<arrow::BooleanBuilder*>(builder.get())->Append(std::get<bool>(value));
            break;
    }
    if (!status.ok()) {
        throw std::runtime_error("Arrow append failed: " + status.ToString());
    }
}

// --- ExtractValue ---

FieldValue DataFrame::ExtractValue(const std::shared_ptr<arrow::Array>& array, int row) const {
    // null 值返回类型对应的默认值
    if (array->IsNull(row)) {
        switch (array->type_id()) {
            case arrow::Type::INT32:   return int32_t(0);
            case arrow::Type::INT64:   return int64_t(0);
            case arrow::Type::UINT32:  return uint32_t(0);
            case arrow::Type::UINT64:  return uint64_t(0);
            case arrow::Type::FLOAT:   return float(0);
            case arrow::Type::DOUBLE:  return double(0);
            case arrow::Type::BOOL:    return false;
            default:                   return std::string{};
        }
    }
    switch (array->type_id()) {
        case arrow::Type::INT32:
            return std::static_pointer_cast<arrow::Int32Array>(array)->Value(row);
        case arrow::Type::INT64:
            return std::static_pointer_cast<arrow::Int64Array>(array)->Value(row);
        case arrow::Type::UINT32:
            return std::static_pointer_cast<arrow::UInt32Array>(array)->Value(row);
        case arrow::Type::UINT64:
            return std::static_pointer_cast<arrow::UInt64Array>(array)->Value(row);
        case arrow::Type::FLOAT:
            return std::static_pointer_cast<arrow::FloatArray>(array)->Value(row);
        case arrow::Type::DOUBLE:
            return std::static_pointer_cast<arrow::DoubleArray>(array)->Value(row);
        case arrow::Type::STRING: {
            auto str_arr = std::static_pointer_cast<arrow::StringArray>(array);
            return std::string(str_arr->GetView(row));
        }
        case arrow::Type::BINARY: {
            auto bin_arr = std::static_pointer_cast<arrow::BinaryArray>(array);
            auto view = bin_arr->GetView(row);
            return std::vector<uint8_t>(view.begin(), view.end());
        }
        case arrow::Type::BOOL:
            return std::static_pointer_cast<arrow::BooleanArray>(array)->Value(row);
        default:
            return std::string{};
    }
}

// --- Filter 实现 ---
// 解析简单条件表达式并过滤行
// 支持: column=value, column>value, column<value, column>=value, column<=value, column!=value
int DataFrame::Filter(const char* condition) {
    if (!condition || !*condition) return -1;

    Finalize();
    if (!batch_ || batch_->num_rows() == 0) return 0;

    // 解析条件：找到操作符
    std::string cond(condition);
    std::string col_name, op, value;

    // 支持的操作符（按长度降序匹配）
    static const char* ops[] = {">=", "<=", "!=", "=", ">", "<"};
    size_t op_pos = std::string::npos;
    for (const char* o : ops) {
        op_pos = cond.find(o);
        if (op_pos != std::string::npos) {
            op = o;
            col_name = cond.substr(0, op_pos);
            value = cond.substr(op_pos + op.size());
            break;
        }
    }

    if (op_pos == std::string::npos) return -1;

    // 去除空白
    while (!col_name.empty() && std::isspace(col_name.back())) col_name.pop_back();
    while (!value.empty() && std::isspace(value.front())) value.erase(value.begin());

    // 去除值的引号（前后引号必须匹配）
    if (value.size() >= 2 && (value.front() == '\'' || value.front() == '"') &&
        value.back() == value.front()) {
        value = value.substr(1, value.size() - 2);
    }

    // 查找列索引
    int col_idx = -1;
    for (size_t i = 0; i < schema_.size(); ++i) {
        if (schema_[i].name == col_name) {
            col_idx = static_cast<int>(i);
            break;
        }
    }
    if (col_idx < 0) return -1;  // 列不存在

    // 逐行过滤，保留匹配的行
    std::vector<std::vector<FieldValue>> kept_rows;
    int32_t nrows = batch_->num_rows();

    for (int32_t r = 0; r < nrows; ++r) {
        auto row_val = ExtractValue(batch_->column(col_idx), r);
        bool match = false;

        // 根据类型比较
        std::visit([&](auto&& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, int64_t> ||
                          std::is_same_v<T, uint32_t> || std::is_same_v<T, uint64_t>) {
                int64_t lhs = static_cast<int64_t>(v);
                int64_t rhs = 0;
                try { rhs = std::stoll(value); } catch (...) { return; }
                if (op == "=") match = (lhs == rhs);
                else if (op == ">") match = (lhs > rhs);
                else if (op == "<") match = (lhs < rhs);
                else if (op == ">=") match = (lhs >= rhs);
                else if (op == "<=") match = (lhs <= rhs);
                else if (op == "!=") match = (lhs != rhs);
            } else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
                double lhs = static_cast<double>(v);
                double rhs = 0;
                try { rhs = std::stod(value); } catch (...) { return; }
                if (op == "=") match = (lhs == rhs);
                else if (op == ">") match = (lhs > rhs);
                else if (op == "<") match = (lhs < rhs);
                else if (op == ">=") match = (lhs >= rhs);
                else if (op == "<=") match = (lhs <= rhs);
                else if (op == "!=") match = (lhs != rhs);
            } else if constexpr (std::is_same_v<T, std::string>) {
                if (op == "=") match = (v == value);
                else if (op == "!=") match = (v != value);
                else if (op == ">") match = (v > value);
                else if (op == "<") match = (v < value);
                else if (op == ">=") match = (v >= value);
                else if (op == "<=") match = (v <= value);
            } else if constexpr (std::is_same_v<T, bool>) {
                bool rhs = (value == "true" || value == "1");
                if (op == "=") match = (v == rhs);
                else if (op == "!=") match = (v != rhs);
            }
        }, row_val);

        if (match) {
            // 收集整行
            std::vector<FieldValue> row;
            for (int c = 0; c < batch_->num_columns(); ++c) {
                row.push_back(ExtractValue(batch_->column(c), r));
            }
            kept_rows.push_back(std::move(row));
        }
    }

    // 重建 DataFrame
    auto saved_schema = schema_;
    Clear();
    SetSchema(saved_schema);
    for (auto& row : kept_rows) {
        AppendRow(row);
    }

    return 0;
}

}  // namespace flowsql
