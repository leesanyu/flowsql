#include "sample_operator.h"

#include <framework/core/dataframe.h>
#include <framework/interfaces/idataframe_channel.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace {

const char* DataTypeName(flowsql::DataType t) {
    using flowsql::DataType;
    switch (t) {
        case DataType::INT32: return "INT32";
        case DataType::INT64: return "INT64";
        case DataType::UINT32: return "UINT32";
        case DataType::UINT64: return "UINT64";
        case DataType::FLOAT: return "FLOAT";
        case DataType::DOUBLE: return "DOUBLE";
        case DataType::STRING: return "STRING";
        case DataType::BYTES: return "BYTES";
        case DataType::TIMESTAMP: return "TIMESTAMP";
        case DataType::BOOLEAN: return "BOOLEAN";
        default: return "UNKNOWN";
    }
}

bool TryToDouble(const flowsql::FieldValue& value, double* out) {
    if (!out) return false;
    if (std::holds_alternative<int32_t>(value)) {
        *out = static_cast<double>(std::get<int32_t>(value));
        return true;
    }
    if (std::holds_alternative<int64_t>(value)) {
        *out = static_cast<double>(std::get<int64_t>(value));
        return true;
    }
    if (std::holds_alternative<uint32_t>(value)) {
        *out = static_cast<double>(std::get<uint32_t>(value));
        return true;
    }
    if (std::holds_alternative<uint64_t>(value)) {
        *out = static_cast<double>(std::get<uint64_t>(value));
        return true;
    }
    if (std::holds_alternative<float>(value)) {
        *out = static_cast<double>(std::get<float>(value));
        return true;
    }
    if (std::holds_alternative<double>(value)) {
        *out = std::get<double>(value);
        return true;
    }
    if (std::holds_alternative<bool>(value)) {
        *out = std::get<bool>(value) ? 1.0 : 0.0;
        return true;
    }
    return false;
}

}  // namespace

int SampleOperatorBase::Configure(const char*, const char*) {
    return 0;
}

std::string PassThroughOperator::Category() {
    return "sample";
}

std::string PassThroughOperator::Name() {
    return "passthrough";
}

std::string PassThroughOperator::Description() {
    return "Pass through input DataFrame without modifications";
}

flowsql::OperatorPosition PassThroughOperator::Position() {
    return flowsql::OperatorPosition::DATA;
}

int PassThroughOperator::Work(flowsql::IChannel* in, flowsql::IChannel* out) {
    SetLastError("");

    auto* df_in = dynamic_cast<flowsql::IDataFrameChannel*>(in);
    auto* df_out = dynamic_cast<flowsql::IDataFrameChannel*>(out);
    if (!df_in || !df_out) {
        SetLastError("passthrough requires dataframe input/output channel");
        return -1;
    }

    flowsql::DataFrame input;
    if (df_in->Read(&input) != 0) {
        SetLastError("failed to read input dataframe");
        return -1;
    }
    if (df_out->Write(&input) != 0) {
        SetLastError("failed to write output dataframe");
        return -1;
    }
    return 0;
}

std::string RowCountOperator::Category() {
    return "sample";
}

std::string RowCountOperator::Name() {
    return "row_count";
}

std::string RowCountOperator::Description() {
    return "Return row count and column count for input DataFrame";
}

flowsql::OperatorPosition RowCountOperator::Position() {
    return flowsql::OperatorPosition::DATA;
}

int RowCountOperator::Work(flowsql::IChannel* in, flowsql::IChannel* out) {
    SetLastError("");

    auto* df_in = dynamic_cast<flowsql::IDataFrameChannel*>(in);
    auto* df_out = dynamic_cast<flowsql::IDataFrameChannel*>(out);
    if (!df_in || !df_out) {
        SetLastError("row_count requires dataframe input/output channel");
        return -1;
    }

    flowsql::DataFrame input;
    if (df_in->Read(&input) != 0) {
        SetLastError("failed to read input dataframe");
        return -1;
    }

    const int64_t row_count = static_cast<int64_t>(input.RowCount());
    const int64_t column_count = static_cast<int64_t>(input.GetSchema().size());

    flowsql::DataFrame output;
    output.SetSchema({
        {"row_count", flowsql::DataType::INT64, 0, ""},
        {"column_count", flowsql::DataType::INT64, 0, ""},
    });
    if (output.AppendRow({row_count, column_count}) != 0) {
        SetLastError("failed to append result row");
        return -1;
    }
    if (df_out->Write(&output) != 0) {
        SetLastError("failed to write output dataframe");
        return -1;
    }
    return 0;
}

std::string ColumnStatsOperator::Category() {
    return "sample";
}

std::string ColumnStatsOperator::Name() {
    return "column_stats";
}

std::string ColumnStatsOperator::Description() {
    return "Compute per-column count/min/max/mean for DataFrame input";
}

flowsql::OperatorPosition ColumnStatsOperator::Position() {
    return flowsql::OperatorPosition::DATA;
}

int ColumnStatsOperator::Work(flowsql::IChannel* in, flowsql::IChannel* out) {
    last_error_.clear();

    auto* df_in = dynamic_cast<flowsql::IDataFrameChannel*>(in);
    auto* df_out = dynamic_cast<flowsql::IDataFrameChannel*>(out);
    if (!df_in || !df_out) {
        last_error_ = "column_stats requires dataframe input/output channel";
        return -1;
    }

    flowsql::DataFrame input;
    if (df_in->Read(&input) != 0) {
        last_error_ = "failed to read input dataframe";
        return -1;
    }

    const std::vector<flowsql::Field> input_schema = input.GetSchema();
    const int64_t rows = static_cast<int64_t>(input.RowCount());

    flowsql::DataFrame output;
    output.SetSchema({
        {"column_name", flowsql::DataType::STRING, 0, ""},
        {"data_type", flowsql::DataType::STRING, 0, ""},
        {"row_count", flowsql::DataType::INT64, 0, ""},
        {"numeric_count", flowsql::DataType::INT64, 0, ""},
        {"min", flowsql::DataType::DOUBLE, 0, ""},
        {"max", flowsql::DataType::DOUBLE, 0, ""},
        {"mean", flowsql::DataType::DOUBLE, 0, ""},
    });

    for (size_t c = 0; c < input_schema.size(); ++c) {
        int64_t numeric_count = 0;
        double sum = 0.0;
        double min_value = std::numeric_limits<double>::max();
        double max_value = std::numeric_limits<double>::lowest();
        bool has_numeric = false;

        for (int64_t r = 0; r < rows; ++r) {
            const auto row = input.GetRow(static_cast<int32_t>(r));
            if (c >= row.size()) continue;
            double v = 0.0;
            if (!TryToDouble(row[c], &v)) continue;
            ++numeric_count;
            sum += v;
            min_value = std::min(min_value, v);
            max_value = std::max(max_value, v);
            has_numeric = true;
        }

        const double mean = (numeric_count > 0) ? (sum / static_cast<double>(numeric_count)) : 0.0;
        if (!has_numeric) {
            min_value = 0.0;
            max_value = 0.0;
        }

        if (output.AppendRow({
                input_schema[c].name,
                std::string(::DataTypeName(input_schema[c].type)),
                rows,
                numeric_count,
                min_value,
                max_value,
                mean,
            }) != 0) {
            last_error_ = "failed to append stats row";
            return -1;
        }
    }

    if (df_out->Write(&output) != 0) {
        last_error_ = "failed to write output dataframe";
        return -1;
    }
    return 0;
}

int ColumnStatsOperator::Configure(const char*, const char*) {
    return 0;
}
