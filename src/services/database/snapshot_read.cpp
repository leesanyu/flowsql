// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "snapshot_read.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

namespace flowsql::database {
bool SnapshotPhysicalType(const std::string& backend, const std::string& physical, arrow::Type::type logical) {
    std::string p = physical;
    std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return std::tolower(c); });
    bool integer = p.find("int") != std::string::npos;
    bool number = integer || p.find("numeric") != std::string::npos || p.find("decimal") != std::string::npos;
    bool real = p.find("float") != std::string::npos || p.find("double") != std::string::npos || p == "real";
    bool text = p.find("text") != std::string::npos || p.find("char") != std::string::npos ||
                p.find("string") != std::string::npos;
    switch (logical) {
        case arrow::Type::INT64:
            return integer && p.find("unsigned") == std::string::npos && p.find("uint") == std::string::npos;
        case arrow::Type::UINT64:
            return number || (backend == "sqlite" && text);
        case arrow::Type::DOUBLE:
            return number || real;
        case arrow::Type::BOOL:
            return p.find("bool") != std::string::npos || integer;
        case arrow::Type::STRING:
            return text;
        default:
            return false;
    }
}
int MakeSnapshotBatch(const std::shared_ptr<arrow::Schema>& schema, const SnapshotRows& rows, uint64_t max_bytes,
                      std::shared_ptr<arrow::RecordBatch>* output, std::string* error) {
    output->reset();
    std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
    for (const auto& field : schema->fields()) {
        auto builder = arrow::MakeBuilder(field->type());
        if (!builder.ok()) {
            *error = builder.status().ToString();
            return -1;
        }
        builders.push_back(std::move(builder).ValueOrDie());
    }
    uint64_t size = 0;
    for (const auto& row : rows) {
        if (row.size() != builders.size()) {
            *error = "snapshot column count mismatch";
            return -1;
        }
        for (size_t i = 0; i < row.size(); ++i) {
            size += 16 + (row[i] ? row[i]->size() : 0);
            if (size > max_bytes) {
                *error = "snapshot page byte budget exceeded";
                return -1;
            }
            arrow::Status status;
            if (!row[i])
                status = builders[i]->AppendNull();
            else {
                const auto& value = *row[i];
                const char* begin = value.data();
                const char* end = begin + value.size();
                switch (schema->field(i)->type()->id()) {
                    case arrow::Type::INT64: {
                        int64_t v;
                        auto parsed = std::from_chars(begin, end, v);
                        if (parsed.ec != std::errc{} || parsed.ptr != end) {
                            *error = "invalid/lossy int64";
                            return -1;
                        }
                        status = static_cast<arrow::Int64Builder*>(builders[i].get())->Append(v);
                        break;
                    }
                    case arrow::Type::UINT64: {
                        uint64_t v;
                        auto parsed = std::from_chars(begin, end, v);
                        if (parsed.ec != std::errc{} || parsed.ptr != end) {
                            *error = "invalid/lossy uint64";
                            return -1;
                        }
                        status = static_cast<arrow::UInt64Builder*>(builders[i].get())->Append(v);
                        break;
                    }
                    case arrow::Type::DOUBLE: {
                        double v;
                        auto parsed = std::from_chars(begin, end, v);
                        if (parsed.ec != std::errc{} || parsed.ptr != end) {
                            *error = "invalid double in " + schema->field(i)->name() +
                                     " (length=" + std::to_string(value.size()) + ")";
                            return -1;
                        }
                        status = static_cast<arrow::DoubleBuilder*>(builders[i].get())->Append(v);
                        break;
                    }
                    case arrow::Type::BOOL:
                        if (value != "0" && value != "1" && value != "t" && value != "f" && value != "true" &&
                            value != "false") {
                            *error = "invalid boolean";
                            return -1;
                        }
                        status = static_cast<arrow::BooleanBuilder*>(builders[i].get())
                                     ->Append(value == "1" || value == "t" || value == "true");
                        break;
                    case arrow::Type::STRING:
                        status = static_cast<arrow::StringBuilder*>(builders[i].get())->Append(value);
                        break;
                    default:
                        *error = "unsupported logical type";
                        return -1;
                }
            }
            if (!status.ok()) {
                *error = status.ToString();
                return -1;
            }
        }
    }
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    for (auto& builder : builders) {
        auto array = builder->Finish();
        if (!array.ok()) {
            *error = array.status().ToString();
            return -1;
        }
        arrays.push_back(std::move(array).ValueOrDie());
    }
    *output = arrow::RecordBatch::Make(schema, rows.size(), arrays);
    return 0;
}
}  // namespace flowsql::database
