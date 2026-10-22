// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "dataframe_workflow.h"

#include <arrow/compute/api.h>
#include <arrow/ipc/writer.h>
#include <arrow/vendored/datetime/tz.h>
#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace flowsql::visualization {
namespace {
using Json = rapidjson::Value;
constexpr int64_t kSafeInteger = 9007199254740991LL;
constexpr size_t kWorkMemoryBytes = 128ULL * 1024 * 1024;

[[noreturn]] void Invalid(const std::string& message) { throw QueryFailure("INVALID_WORKFLOW", message); }
void Keys(const Json& value, std::initializer_list<const char*> allowed) {
    if (!value.IsObject()) Invalid("操作必须是对象");
    const std::set<std::string> names(allowed.begin(), allowed.end());
    for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it)
        if (!names.count(it->name.GetString())) throw QueryFailure("UNSUPPORTED_OPERATION", "不支持此操作参数");
}
std::string Text(const Json& value, const char* key) {
    if (!value.IsObject() || !value.HasMember(key) || !value[key].IsString() || value[key].GetStringLength() > 256)
        Invalid("缺少有效的字段或操作名");
    return {value[key].GetString(), value[key].GetStringLength()};
}

std::string TypeClass(const std::shared_ptr<arrow::DataType>& type) {
    switch (type->id()) {
        case arrow::Type::INT8:
        case arrow::Type::INT16:
        case arrow::Type::INT32:
        case arrow::Type::INT64:
        case arrow::Type::UINT8:
        case arrow::Type::UINT16:
        case arrow::Type::UINT32:
        case arrow::Type::UINT64:
        case arrow::Type::HALF_FLOAT:
        case arrow::Type::FLOAT:
        case arrow::Type::DOUBLE:
        case arrow::Type::DECIMAL:
            return "number";
        case arrow::Type::BOOL:
            return "boolean";
        case arrow::Type::STRING:
        case arrow::Type::LARGE_STRING:
        case arrow::Type::DATE32:
        case arrow::Type::DATE64:
        case arrow::Type::TIMESTAMP:
            return "string";
        default:
            return "unsupported";
    }
}

bool IsTemporal(const std::shared_ptr<arrow::DataType>& type) {
    return type->id() == arrow::Type::DATE32 || type->id() == arrow::Type::DATE64 ||
           type->id() == arrow::Type::TIMESTAMP;
}

// A comparison literal keeps its JSON type; NULL never reaches comparison (it maps to IS [NOT] NULL).
struct Literal {
    enum class Kind { kNull, kInteger, kReal, kBoolean, kText } kind = Kind::kNull;
    int64_t integer = 0;
    double real = 0;
    std::string text;
};

Literal ParseLiteral(const Json& value) {
    Literal literal;
    if (value.IsNull()) return literal;
    if (value.IsBool()) {
        literal.kind = Literal::Kind::kBoolean;
        literal.integer = value.GetBool() ? 1 : 0;
        return literal;
    }
    if (value.IsInt64()) {
        literal.kind = Literal::Kind::kInteger;
        literal.integer = value.GetInt64();
        return literal;
    }
    if (value.IsDouble()) {
        if (!std::isfinite(value.GetDouble())) throw QueryFailure("UNSUPPORTED_TYPE", "非有限数值不能比较");
        literal.kind = Literal::Kind::kReal;
        literal.real = value.GetDouble();
        return literal;
    }
    if (value.IsString() && value.GetStringLength() <= 65536) {
        literal.kind = Literal::Kind::kText;
        literal.text.assign(value.GetString(), value.GetStringLength());
        return literal;
    }
    Invalid("不支持该过滤值");
}

// Exact numeric comparison of canonical decimal text; never routes through floating point.
int CompareDecimalText(const std::string& left, const std::string& right) {
    struct Parsed {
        bool negative = false;
        std::string digits;
        int64_t point = 0;
    };
    const auto parse = [](const std::string& text) {
        Parsed parsed;
        size_t pos = 0;
        if (!text.empty() && (text[0] == '-' || text[0] == '+')) {
            parsed.negative = text[0] == '-';
            pos = 1;
        }
        bool fraction = false;
        for (; pos < text.size(); ++pos) {
            if (text[pos] == '.') {
                if (fraction) throw QueryFailure("UNSUPPORTED_TYPE", "Decimal 文本无效");
                fraction = true;
                continue;
            }
            if (text[pos] < '0' || text[pos] > '9') throw QueryFailure("UNSUPPORTED_TYPE", "Decimal 文本无效");
            parsed.digits.push_back(text[pos]);
            if (!fraction) ++parsed.point;
        }
        const auto first = parsed.digits.find_first_not_of('0');
        if (first == std::string::npos) {
            parsed.digits.clear();
            parsed.point = 0;
            parsed.negative = false;
        } else {
            parsed.digits.erase(0, first);
            parsed.point -= static_cast<int64_t>(first);
        }
        return parsed;
    };
    const auto a = parse(left), b = parse(right);
    if (a.digits.empty() || b.digits.empty()) {
        if (a.digits.empty() && b.digits.empty()) return 0;
        // Zero above or below the other value depending on the other operand's sign.
        if (a.digits.empty()) return b.negative ? 1 : -1;
        return a.negative ? -1 : 1;
    }
    if (a.negative != b.negative) return a.negative ? -1 : 1;
    const int sign = a.negative ? -1 : 1;
    if (a.point != b.point) return a.point < b.point ? -sign : sign;
    const auto shared = std::min(a.digits.size(), b.digits.size());
    const auto order = a.digits.compare(0, shared, b.digits, 0, shared);
    if (order) return order < 0 ? -sign : sign;
    if (a.digits.size() == b.digits.size()) return 0;
    return a.digits.size() < b.digits.size() ? -sign : sign;
}

std::string DoubleText(double value) {
    std::ostringstream text;
    text << std::setprecision(17) << value;
    return text.str();
}

// One materialised column keeps its Arrow type class, so text is compared by bytes and
// integers stay exact instead of being widened to double for filtering or grouping.
struct Column {
    FieldV1 field;
    std::shared_ptr<arrow::Array> array;
    std::vector<bool> valid;
    std::vector<int64_t> integers;
    std::vector<double> reals;
    std::vector<std::string> texts;
    std::vector<std::string> decimals;
    bool text_backed = false;
    bool decimal = false;
    bool temporal = false;
    int32_t scale = 0;
    int64_t length = 0;
};

enum class CellKind { kNull, kInteger, kReal, kText };
struct Cell {
    CellKind kind = CellKind::kNull;
    int64_t integer = 0;
    double real = 0;
    std::string text;
};

class Executor {
 public:
    Executor(const std::shared_ptr<arrow::RecordBatch>& batch, const std::vector<FieldV1>& fields, uint64_t max_bytes,
             const std::chrono::steady_clock::time_point& deadline)
        : batch_(batch), fields_(fields), max_bytes_(max_bytes), deadline_(deadline) {
        for (size_t i = 0; i < static_cast<size_t>(batch_->num_columns()); ++i)
            columns_.emplace(fields_[i].id, Materialise(static_cast<int>(i), fields_[i]));
    }

    DataFrameResultV1 Run(const Json& input, const std::string& mode, uint32_t maximum) {
        Keys(input, {"workflow", "limit", "offset"});
        if (!input.HasMember("workflow") || !input["workflow"].IsArray() || input["workflow"].Size() > 32)
            Invalid("无效的 workflow");
        if (mode != "complete" && mode != "limited") Invalid("无效的明细模式");
        std::vector<int64_t> rows(static_cast<size_t>(batch_->num_rows()));
        for (int64_t i = 0; i < batch_->num_rows(); ++i) rows[static_cast<size_t>(i)] = i;
        bool viewed = false, sorted = false;
        std::vector<std::string> project, group_fields, group_units, group_aliases, sort_ids;
        std::vector<const rapidjson::Value*> measures;
        std::string histogram_field, sort_direction;
        uint32_t bins = 0;
        for (const auto& step : input["workflow"].GetArray()) {
            Checkpoint();
            const auto type = Text(step, "type");
            if (type == "filter") {
                Keys(step, {"type", "filters"});
                if (viewed || !step.HasMember("filters") || !step["filters"].IsArray())
                    throw QueryFailure("UNSUPPORTED_OPERATION", "仅支持聚合前过滤");
                for (const auto& filter : step["filters"].GetArray()) FilterRows(filter, 0, &rows);
            } else if (type == "view") {
                Keys(step, {"type", "query"});
                if (viewed || !step.HasMember("query") || !step["query"].IsArray() || step["query"].Size() != 1)
                    Invalid("每次查询必须有一个单表视图");
                viewed = true;
                const auto& query = step["query"][0];
                const auto op = Text(query, "op");
                if (op == "raw") {
                    Keys(query, {"op", "fields"});
                    if (!query.HasMember("fields") || !query["fields"].IsArray() || query["fields"].Empty())
                        Invalid("缺少明细字段");
                    for (const auto& id : query["fields"].GetArray()) project.push_back(Lookup(id).field.id);
                } else if (op == "histogram") {
                    Keys(query, {"op", "field", "bins"});
                    if (!query.HasMember("field")) Invalid("缺少直方图数值字段");
                    const auto& column = Lookup(query["field"]);
                    if (column.field.type != "number") throw QueryFailure("UNSUPPORTED_TYPE", "直方图需要数值字段");
                    if (!query.HasMember("bins") || !query["bins"].IsUint() || query["bins"].GetUint() < 1 ||
                        query["bins"].GetUint() > 100)
                        Invalid("箱数须为 1～100 的整数");
                    bins = query["bins"].GetUint();
                    histogram_field = column.field.id;
                } else if (op == "aggregate") {
                    Keys(query, {"op", "groupBy", "measures"});
                    if (!query.HasMember("groupBy") || !query["groupBy"].IsArray() || !query.HasMember("measures") ||
                        !query["measures"].IsArray())
                        Invalid("无效的聚合视图");
                    for (const auto& id : query["groupBy"].GetArray()) {
                        if (id.IsString()) {
                            const auto& column = Lookup(id);
                            group_fields.push_back(column.field.id);
                            group_units.emplace_back();
                            group_aliases.push_back(column.field.id);
                            continue;
                        }
                        Keys(id, {"field", "unit", "asFieldKey"});
                        const auto unit = Text(id, "unit");
                        const auto alias = Text(id, "asFieldKey");
                        if (unit != "day" && unit != "month")
                            throw QueryFailure("UNSUPPORTED_OPERATION", "时间桶仅支持 day/month");
                        const auto& column = Lookup(id["field"]);
                        if (!column.temporal) throw QueryFailure("UNSUPPORTED_TYPE", "时间桶需要日期或时间戳字段");
                        BucketArray(column, unit);
                        group_fields.push_back(column.field.id);
                        group_units.push_back(unit);
                        group_aliases.push_back(alias);
                    }
                    for (const auto& measure : query["measures"].GetArray()) measures.push_back(&measure);
                } else {
                    throw QueryFailure("UNSUPPORTED_OPERATION", "不支持此视图操作");
                }
            } else if (type == "sort") {
                Keys(step, {"type", "by", "sort"});
                if (!viewed || sorted || !step.HasMember("by") || !step["by"].IsArray()) Invalid("无效的排序");
                sorted = true;
                sort_direction = Text(step, "sort");
                if (sort_direction != "ascending" && sort_direction != "descending") Invalid("无效的排序方向");
                for (const auto& id : step["by"].GetArray()) {
                    if (!id.IsString() || id.GetStringLength() > 256) Invalid("排序字段必须是字符串");
                    sort_ids.emplace_back(id.GetString(), id.GetStringLength());
                }
            } else {
                throw QueryFailure("UNSUPPORTED_OPERATION", "不支持此表达式或工作流操作");
            }
        }
        if (!viewed || (project.empty() && group_fields.empty() && measures.empty() && !bins))
            Invalid("必须选择有效的字段");
        if (project.size() > 256) Invalid("结果字段过多");
        int64_t limit = -1, offset = 0;
        if (input.HasMember("limit")) {
            if (!input["limit"].IsInt64() || input["limit"].GetInt64() < -1) Invalid("无效的行数限制");
            limit = input["limit"].GetInt64();
        }
        if (input.HasMember("offset")) {
            if (!input["offset"].IsInt64() || input["offset"].GetInt64() < 0) Invalid("无效的偏移量");
            offset = input["offset"].GetInt64();
        }
        if (limit > static_cast<int64_t>(maximum)) throw QueryFailure("RESULT_LIMIT", "请求的行数超过服务上限");
        const bool aggregate = !group_fields.empty() || !measures.empty();
        DataFrameResultV1 result;
        result.scope = (limit >= 0 || offset)              ? "top_n"
                       : (!aggregate && mode == "limited") ? "limited_detail"
                                                           : "complete";
        if (bins) {
            Histogram(rows, histogram_field, bins, limit, offset, &result);
            return result;
        }
        if (aggregate) {
            Aggregate(rows, group_fields, group_units, group_aliases, measures, sort_ids, sort_direction, limit, offset,
                      &result);
            return result;
        }
        Project(rows, project, sort_ids, sort_direction, limit, offset, &result);
        return result;
    }

 private:
    void Checkpoint() const {
        if (std::chrono::steady_clock::now() >= deadline_) throw QueryFailure("TIMEOUT", "查询超过计算期限");
    }

    Column Materialise(int index, const FieldV1& field) {
        Column column;
        column.field = field;
        column.array = batch_->column(index);
        column.length = batch_->num_rows();
        column.valid.assign(static_cast<size_t>(column.length), false);
        const auto type = column.array->type();
        column.temporal = IsTemporal(type);
        column.decimal = type->id() == arrow::Type::DECIMAL;
        if (column.decimal) column.scale = static_cast<const arrow::DecimalType&>(*type).scale();
        const bool numeric = field.type == "number";
        const size_t cost = static_cast<size_t>(column.length) * (column.decimal ? 48 : numeric ? 8 : 16);
        if (budget_ + cost > kWorkMemoryBytes) throw QueryFailure("RESULT_LIMIT", "工作内存超过计算预算");
        budget_ += cost;
        switch (type->id()) {
            case arrow::Type::INT8:
            case arrow::Type::INT16:
            case arrow::Type::INT32:
            case arrow::Type::INT64:
            case arrow::Type::UINT8:
            case arrow::Type::UINT16:
            case arrow::Type::UINT32:
            case arrow::Type::UINT64:
            case arrow::Type::DATE32:
            case arrow::Type::DATE64: {
                column.integers.resize(static_cast<size_t>(column.length));
                for (int64_t row = 0; row < column.length; ++row) {
                    if (column.array->IsNull(row)) continue;
                    column.valid[static_cast<size_t>(row)] = true;
                    if (type->id() == arrow::Type::DATE32)
                        column.integers[static_cast<size_t>(row)] =
                            static_cast<arrow::Date32Array&>(*column.array).Value(row);
                    else if (type->id() == arrow::Type::DATE64)
                        column.integers[static_cast<size_t>(row)] =
                            static_cast<arrow::Date64Array&>(*column.array).Value(row);
                    else
                        column.integers[static_cast<size_t>(row)] = IntegerAt(column.array, row);
                }
                break;
            }
            case arrow::Type::HALF_FLOAT:
            case arrow::Type::FLOAT:
            case arrow::Type::DOUBLE: {
                column.reals.resize(static_cast<size_t>(column.length));
                for (int64_t row = 0; row < column.length; ++row) {
                    if (column.array->IsNull(row)) continue;
                    column.valid[static_cast<size_t>(row)] = true;
                    column.reals[static_cast<size_t>(row)] = RealAt(column.array, row);
                }
                break;
            }
            case arrow::Type::BOOL:
            case arrow::Type::STRING:
            case arrow::Type::LARGE_STRING:
            case arrow::Type::TIMESTAMP: {
                column.text_backed = true;
                column.texts.resize(static_cast<size_t>(column.length));
                for (int64_t row = 0; row < column.length; ++row) {
                    if (column.array->IsNull(row)) continue;
                    column.valid[static_cast<size_t>(row)] = true;
                    column.texts[static_cast<size_t>(row)] = TextAt(column.array, row);
                }
                break;
            }
            case arrow::Type::DECIMAL: {
                column.decimals.resize(static_cast<size_t>(column.length));
                for (int64_t row = 0; row < column.length; ++row) {
                    if (column.array->IsNull(row)) continue;
                    column.valid[static_cast<size_t>(row)] = true;
                    column.decimals[static_cast<size_t>(row)] = DecimalAt(column.array, row, column.scale);
                }
                break;
            }
            default:
                break;
        }
        return column;
    }

    static int64_t IntegerAt(const std::shared_ptr<arrow::Array>& array, int64_t row) {
        const auto id = array->type()->id();
        switch (id) {
            case arrow::Type::INT8:
                return static_cast<arrow::Int8Array&>(*array).Value(row);
            case arrow::Type::INT16:
                return static_cast<arrow::Int16Array&>(*array).Value(row);
            case arrow::Type::INT32:
                return static_cast<arrow::Int32Array&>(*array).Value(row);
            case arrow::Type::INT64:
                return static_cast<arrow::Int64Array&>(*array).Value(row);
            case arrow::Type::UINT8:
                return static_cast<arrow::UInt8Array&>(*array).Value(row);
            case arrow::Type::UINT16:
                return static_cast<arrow::UInt16Array&>(*array).Value(row);
            case arrow::Type::UINT32:
                return static_cast<arrow::UInt32Array&>(*array).Value(row);
            case arrow::Type::UINT64: {
                const auto value = static_cast<arrow::UInt64Array&>(*array).Value(row);
                if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                    throw QueryFailure("UNSUPPORTED_TYPE", "整数超出可表示范围");
                return static_cast<int64_t>(value);
            }
            default:
                throw QueryFailure("UNSUPPORTED_TYPE", "不支持的整数类型");
        }
    }
    static double RealAt(const std::shared_ptr<arrow::Array>& array, int64_t row) {
        if (array->type()->id() == arrow::Type::HALF_FLOAT)
            return static_cast<double>(static_cast<arrow::HalfFloatArray&>(*array).Value(row));
        if (array->type()->id() == arrow::Type::FLOAT)
            return static_cast<double>(static_cast<arrow::FloatArray&>(*array).Value(row));
        return static_cast<arrow::DoubleArray&>(*array).Value(row);
    }
    static std::string TextAt(const std::shared_ptr<arrow::Array>& array, int64_t row) {
        if (array->type()->id() == arrow::Type::BOOL)
            return static_cast<arrow::BooleanArray&>(*array).Value(row) ? "true" : "false";
        if (array->type()->id() == arrow::Type::TIMESTAMP) {
            const auto& stamp = static_cast<const arrow::TimestampType&>(*array->type());
            const auto value = static_cast<arrow::TimestampArray&>(*array).Value(row);
            int64_t millis = value;
            if (stamp.unit() == arrow::TimeUnit::SECOND) millis = value * 1000;
            if (stamp.unit() == arrow::TimeUnit::MICRO) millis = value / 1000;
            if (stamp.unit() == arrow::TimeUnit::NANO) millis = value / 1000000;
            const std::time_t seconds = static_cast<std::time_t>(millis >= 0 ? millis / 1000 : (millis - 999) / 1000);
            std::tm broken{};
            // A zoned column renders in its declared zone; a zone-less one keeps the zone-less calendar.
            if (stamp.timezone().empty()) {
                gmtime_r(&seconds, &broken);
            } else {
                localtime_r(&seconds, &broken);
            }
            char text[40];
            std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &broken);
            return text;
        }
        if (array->type()->id() == arrow::Type::LARGE_STRING)
            return std::string(static_cast<arrow::LargeStringArray&>(*array).GetView(row));
        return std::string(static_cast<arrow::StringArray&>(*array).GetView(row));
    }
    static std::string DecimalAt(const std::shared_ptr<arrow::Array>& array, int64_t row, int32_t scale) {
        const auto* type = static_cast<const arrow::DecimalType*>(array->type().get());
        // Canonical text keeps comparisons exact and removes declared-scale padding consistently.
        if (type->bit_width() == 128)
            return CanonicalDecimal(static_cast<arrow::Decimal128Array&>(*array).FormatValue(row));
        if (type->bit_width() == 256)
            return CanonicalDecimal(static_cast<arrow::Decimal256Array&>(*array).FormatValue(row));
        throw QueryFailure("UNSUPPORTED_TYPE", "不支持的 Decimal 位宽");
        (void)scale;
    }

    const Column& Lookup(const Json& id) {
        if (!id.IsString()) Invalid("字段 ID 必须是字符串");
        const auto found = columns_.find(std::string(id.GetString(), id.GetStringLength()));
        if (found == columns_.end()) Invalid("字段不属于所选数据快照");
        if (found->second.field.type == "unsupported") throw QueryFailure("UNSUPPORTED_TYPE", "不支持该字段类型");
        return found->second;
    }
    const Column& Lookup(const std::string& id) {
        const auto found = columns_.find(id);
        if (found == columns_.end()) Invalid("字段不属于所选数据快照");
        return found->second;
    }

    // Day/month buckets: a zoned timestamp uses its own declared calendar (IANA rules), a zone-less
    // timestamp keeps the zone-less calendar, and the host zone is never consulted.
    void BucketArray(const Column& column, const std::string& unit) {
        const auto key = column.field.id + ":" + unit;
        if (buckets_.count(key)) return;
        const auto type = column.array->type();
        const arrow::TimestampType* stamp = nullptr;
        if (type->id() == arrow::Type::TIMESTAMP) stamp = static_cast<const arrow::TimestampType*>(type.get());
        std::vector<std::string> values(static_cast<size_t>(column.length));
        std::vector<bool> valid(static_cast<size_t>(column.length), false);
        for (int64_t row = 0; row < column.length; ++row) {
            if (!column.valid[static_cast<size_t>(row)]) continue;
            int64_t millis = 0;
            if (type->id() == arrow::Type::DATE32)
                millis = static_cast<int64_t>(static_cast<arrow::Date32Array&>(*column.array).Value(row)) * 86400000LL;
            else if (type->id() == arrow::Type::DATE64)
                millis = static_cast<arrow::Date64Array&>(*column.array).Value(row);
            else {
                const auto value = static_cast<arrow::TimestampArray&>(*column.array).Value(row);
                // Floor division keeps pre-epoch timestamps in the correct earlier day.
                const auto floor_div = [](int64_t number, int64_t divisor) {
                    return number >= 0 ? number / divisor : -((-number + divisor - 1) / divisor);
                };
                switch (stamp->unit()) {
                    case arrow::TimeUnit::SECOND:
                        millis = value * 1000;
                        break;
                    case arrow::TimeUnit::MILLI:
                        millis = value;
                        break;
                    case arrow::TimeUnit::MICRO:
                        millis = floor_div(value, 1000);
                        break;
                    default:
                        millis = floor_div(value, 1000000);
                        break;
                }
            }
            int64_t year = 0, month = 0, day = 0;
            if (stamp && !stamp->timezone().empty() && stamp->timezone() != "UTC") {
                try {
                    const auto* zone = arrow_vendored::date::locate_zone(std::string_view(stamp->timezone()));
                    const arrow_vendored::date::zoned_time<std::chrono::seconds> local(
                        zone, arrow_vendored::date::sys_seconds{std::chrono::seconds{millis / 1000}});
                    const auto ymd = arrow_vendored::date::year_month_day{
                        arrow_vendored::date::floor<arrow_vendored::date::days>(local.get_local_time())};
                    year = int(ymd.year());
                    month = unsigned(ymd.month());
                    day = unsigned(ymd.day());
                } catch (const std::exception&) {
                    throw QueryFailure("UNSUPPORTED_TYPE", "时间字段的时区标识无法解析");
                }
            } else {
                // Civil-from-days (Howard Hinnant) for the zone-less calendar.
                const int64_t days = millis >= 0 ? millis / 86400000LL : (millis - 86399999LL) / 86400000LL;
                const int64_t z = days + 719468;
                const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
                const auto doe = static_cast<uint64_t>(z - era * 146097);
                const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
                const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
                const auto mp = (5 * doy + 2) / 153;
                day = static_cast<int64_t>(doy) - static_cast<int64_t>((153 * mp + 2) / 5) + 1;
                month = static_cast<int64_t>(mp) + (mp < 10 ? 3 : -9);
                year = static_cast<int64_t>(yoe) + era * 400 + (month <= 2 ? 1 : 0);
            }
            char text[24];
            if (unit == "month")
                std::snprintf(text, sizeof(text), "%04lld-%02lld-01", static_cast<long long>(year),
                              static_cast<long long>(month));
            else
                std::snprintf(text, sizeof(text), "%04lld-%02lld-%02lld", static_cast<long long>(year),
                              static_cast<long long>(month), static_cast<long long>(day));
            values[static_cast<size_t>(row)] = text;
            valid[static_cast<size_t>(row)] = true;
        }
        buckets_.emplace(key, std::make_pair(std::move(values), std::move(valid)));
    }

    Cell Value(const Column& column, int64_t row) const {
        Cell cell;
        if (row < 0 || row >= column.length || !column.valid[static_cast<size_t>(row)]) return cell;
        if (column.field.type == "number") {
            if (column.decimal) {
                cell.kind = CellKind::kText;
                cell.text = column.decimals[static_cast<size_t>(row)];
            } else if (column.reals.empty()) {
                cell.kind = CellKind::kInteger;
                cell.integer = column.integers[static_cast<size_t>(row)];
            } else {
                cell.kind = CellKind::kReal;
                cell.real = column.reals[static_cast<size_t>(row)];
                if (!std::isfinite(cell.real)) throw QueryFailure("UNSUPPORTED_TYPE", "非有限数值不能比较");
            }
            return cell;
        }
        cell.kind = CellKind::kText;
        cell.text = column.texts[static_cast<size_t>(row)];
        return cell;
    }

    // int order: -1/0/1. NULL sorts last in every ordering.
    static int CompareCells(const Column& column, const Cell& left, const Cell& right) {
        if (left.kind == CellKind::kNull || right.kind == CellKind::kNull)
            return left.kind == right.kind ? 0 : (left.kind == CellKind::kNull ? 1 : -1);
        if (column.field.type == "number" && column.decimal) return CompareDecimalText(left.text, right.text);
        if (left.kind == CellKind::kText || right.kind == CellKind::kText) return left.text.compare(right.text);
        const double a = left.kind == CellKind::kInteger ? static_cast<double>(left.integer) : left.real;
        const double b = right.kind == CellKind::kInteger ? static_cast<double>(right.integer) : right.real;
        return a == b ? 0 : (a < b ? -1 : 1);
    }

    static int CompareToLiteral(const Column& column, const Cell& cell, const Literal& literal) {
        if (cell.kind == CellKind::kNull || literal.kind == Literal::Kind::kNull)
            return cell.kind == CellKind::kNull && literal.kind == Literal::Kind::kNull
                       ? 0
                       : (cell.kind == CellKind::kNull ? 1 : -1);
        if (column.field.type == "number" && column.decimal) {
            switch (literal.kind) {
                case Literal::Kind::kText:
                    return CompareDecimalText(cell.text, literal.text);
                case Literal::Kind::kInteger:
                    return CompareDecimalText(cell.text, std::to_string(literal.integer));
                case Literal::Kind::kReal:
                    return CompareDecimalText(cell.text, DoubleText(literal.real));
                default:
                    throw QueryFailure("UNSUPPORTED_TYPE", "Decimal 只能与数值比较");
            }
        }
        if (cell.kind == CellKind::kText) {
            if (literal.kind != Literal::Kind::kText)
                throw QueryFailure("UNSUPPORTED_TYPE", "文本只会按 UTF-8 字节比较");
            return cell.text.compare(literal.text);
        }
        if (column.field.type == "boolean" || literal.kind == Literal::Kind::kBoolean) {
            if (literal.kind != Literal::Kind::kBoolean)
                throw QueryFailure("UNSUPPORTED_TYPE", "布尔字段只与布尔值比较");
            return cell.integer == literal.integer ? 0 : (cell.integer < literal.integer ? -1 : 1);
        }
        if (literal.kind != Literal::Kind::kInteger && literal.kind != Literal::Kind::kReal)
            throw QueryFailure("UNSUPPORTED_TYPE", "数值字段只与数值比较");
        if (cell.kind == CellKind::kInteger && literal.kind == Literal::Kind::kInteger)
            return cell.integer == literal.integer ? 0 : (cell.integer < literal.integer ? -1 : 1);
        const double left = cell.kind == CellKind::kInteger ? static_cast<double>(cell.integer) : cell.real;
        const double right =
            literal.kind == Literal::Kind::kInteger ? static_cast<double>(literal.integer) : literal.real;
        return left == right ? 0 : (left < right ? -1 : 1);
    }

    static bool Matches(int comparison, const std::string& op) {
        if (op == "eq") return comparison == 0;
        if (op == "ne") return comparison != 0;
        if (op == "lt") return comparison < 0;
        if (op == "lte") return comparison <= 0;
        if (op == "gt") return comparison > 0;
        if (op == "gte") return comparison >= 0;
        Invalid("无效的比较操作");
        return false;
    }

    // AND/OR preserve the incoming row order so unsorted detail stays in snapshot order.
    void FilterRows(const Json& filter, int depth, std::vector<int64_t>* rows) {
        if (depth > 8 || !filter.IsObject()) Invalid("过滤组合过深");
        if (filter.HasMember("op")) {
            Keys(filter, {"op", "filters"});
            const auto op = Text(filter, "op");
            if ((op != "and" && op != "or") || !filter.HasMember("filters") || !filter["filters"].IsArray() ||
                filter["filters"].Empty())
                Invalid("无效的过滤组合");
            std::vector<std::vector<int64_t>> members;
            for (const auto& part : filter["filters"].GetArray()) {
                std::vector<int64_t> subset = *rows;
                FilterRows(part, depth + 1, &subset);
                members.push_back(std::move(subset));
            }
            if (op == "and") {
                *rows = std::move(members.front());
                for (size_t i = 1; i < members.size(); ++i) {
                    std::vector<int64_t> intersection;
                    std::set_intersection(rows->begin(), rows->end(), members[i].begin(), members[i].end(),
                                          std::back_inserter(intersection));
                    *rows = std::move(intersection);
                }
            } else {
                std::set<int64_t> merged;
                for (const auto& member : members) merged.insert(member.begin(), member.end());
                rows->assign(merged.begin(), merged.end());
                std::vector<int64_t> ordered;
                for (const auto row : merged) ordered.push_back(row);
                std::vector<int64_t> previous = *rows;
                (void)previous;
                *rows = std::move(ordered);
            }
            return;
        }
        Keys(filter, {"fid", "rule"});
        if (!filter.HasMember("fid") || !filter.HasMember("rule")) Invalid("无效的过滤");
        const auto& column = Lookup(filter["fid"]);
        const auto& rule = filter["rule"];
        Keys(rule, {"type", "value", "op"});
        const auto type = Text(rule, "type");
        std::vector<int64_t> kept;
        if (type == "is null" || type == "is not null") {
            const bool want_null = type == "is null";
            for (const auto row : *rows)
                if (column.valid[static_cast<size_t>(row)] != want_null) kept.push_back(row);
            *rows = std::move(kept);
            return;
        }
        if (!rule.HasMember("value")) Invalid("缺少过滤值");
        const auto& value = rule["value"];
        if (type == "one of" || type == "not in") {
            if (!value.IsArray()) Invalid("集合过滤需要数组");
            bool has_null = false;
            std::vector<Literal> candidates;
            for (const auto& item : value.GetArray()) {
                if (item.IsNull()) {
                    has_null = true;
                    continue;
                }
                candidates.push_back(ParseLiteral(item));
            }
            const bool inverse = type == "not in";
            for (const auto row : *rows) {
                bool match = false;
                if (!column.valid[static_cast<size_t>(row)]) {
                    match = has_null;
                } else {
                    const auto cell = Value(column, row);
                    for (const auto& candidate : candidates) {
                        if (CompareToLiteral(column, cell, candidate) == 0) {
                            match = true;
                            break;
                        }
                    }
                }
                if (match != inverse) kept.push_back(row);
            }
            *rows = std::move(kept);
            return;
        }
        if (type == "range") {
            if (!value.IsArray() || value.Size() != 2) Invalid("范围需要两个边界");
            const auto lower = ParseLiteral(value[0]);
            const auto upper = ParseLiteral(value[1]);
            for (const auto row : *rows) {
                if (!column.valid[static_cast<size_t>(row)]) continue;
                const auto cell = Value(column, row);
                if (lower.kind != Literal::Kind::kNull && CompareToLiteral(column, cell, lower) < 0) continue;
                if (upper.kind != Literal::Kind::kNull && CompareToLiteral(column, cell, upper) > 0) continue;
                kept.push_back(row);
            }
            *rows = std::move(kept);
            return;
        }
        if (type == "comparison") {
            const auto op = Text(rule, "op");
            if (op != "eq" && op != "ne" && op != "lt" && op != "lte" && op != "gt" && op != "gte")
                Invalid("无效的比较操作");
            if (value.IsNull()) {
                if (op != "eq" && op != "ne") Invalid("NULL 仅支持相等或不等比较");
                const bool want_null = op == "eq";
                for (const auto row : *rows)
                    if (column.valid[static_cast<size_t>(row)] != want_null) kept.push_back(row);
                *rows = std::move(kept);
                return;
            }
            const auto literal = ParseLiteral(value);
            for (const auto row : *rows) {
                if (!column.valid[static_cast<size_t>(row)]) continue;
                if (Matches(CompareToLiteral(column, Value(column, row), literal), op)) kept.push_back(row);
            }
            *rows = std::move(kept);
            return;
        }
        throw QueryFailure("UNSUPPORTED_OPERATION", "不支持此过滤操作");
    }

    struct KeyPart {
        bool null = false;
        CellKind kind = CellKind::kNull;
        int64_t integer = 0;
        double real = 0;
        std::string text;
    };
    using GroupKey = std::vector<KeyPart>;

    static int ComparePart(const KeyPart& left, const KeyPart& right) {
        if (left.null || right.null) return left.null == right.null ? 0 : (left.null ? 1 : -1);
        if (left.kind == CellKind::kText || right.kind == CellKind::kText) return left.text.compare(right.text);
        const double a = left.kind == CellKind::kInteger ? static_cast<double>(left.integer) : left.real;
        const double b = right.kind == CellKind::kInteger ? static_cast<double>(right.integer) : right.real;
        return a == b ? 0 : (a < b ? -1 : 1);
    }
    struct KeyLess {
        bool operator()(const GroupKey& left, const GroupKey& right) const {
            for (size_t i = 0; i < left.size() && i < right.size(); ++i) {
                const auto order = ComparePart(left[i], right[i]);
                if (order) return order < 0;
            }
            return left.size() < right.size();
        }
    };

    KeyPart Part(const Column& column, int64_t row) const {
        KeyPart part;
        if (row < 0 || !column.valid[static_cast<size_t>(row)]) {
            part.null = true;
            return part;
        }
        if (column.field.type == "number") {
            if (column.decimal) {
                part.kind = CellKind::kText;
                part.text = column.decimals[static_cast<size_t>(row)];
            } else if (column.reals.empty()) {
                part.kind = CellKind::kInteger;
                part.integer = column.integers[static_cast<size_t>(row)];
            } else {
                part.kind = CellKind::kReal;
                part.real = column.reals[static_cast<size_t>(row)];
                if (!std::isfinite(part.real)) throw QueryFailure("UNSUPPORTED_TYPE", "非有限数值不能分组");
            }
            return part;
        }
        part.kind = CellKind::kText;
        part.text = column.texts[static_cast<size_t>(row)];
        return part;
    }

    KeyPart BucketPart(const Column& column, const std::string& unit, int64_t row) const {
        KeyPart part;
        const auto& bucket = buckets_.at(column.field.id + ":" + unit);
        if (!bucket.second[static_cast<size_t>(row)]) {
            part.null = true;
            return part;
        }
        part.kind = CellKind::kText;
        part.text = bucket.first[static_cast<size_t>(row)];
        return part;
    }

    ExplorationValueV1 CellValue(const Column& column, int64_t row) const {
        if (row < 0 || row >= column.length || !column.valid[static_cast<size_t>(row)]) return std::monostate{};
        if (column.field.type == "number") {
            if (column.decimal) {
                ExplorationDecimalV1 decimal;
                decimal.text = column.decimals[static_cast<size_t>(row)];
                return decimal;
            }
            if (column.reals.empty()) return column.integers[static_cast<size_t>(row)];
            return column.reals[static_cast<size_t>(row)];
        }
        return column.texts[static_cast<size_t>(row)];
    }

    ExplorationValueV1 KeyValue(const KeyPart& part) const {
        if (part.null) return std::monostate{};
        if (part.kind == CellKind::kInteger) return part.integer;
        if (part.kind == CellKind::kReal) return part.real;
        return part.text;
    }

    void Project(const std::vector<int64_t>& rows, const std::vector<std::string>& project,
                 const std::vector<std::string>& sort_ids, const std::string& direction, int64_t limit, int64_t offset,
                 DataFrameResultV1* result) {
        for (const auto& id : project) result->fields.push_back(Lookup(id).field);
        for (const auto& id : sort_ids)
            if (std::find(project.begin(), project.end(), id) == project.end()) Invalid("排序字段未出现在结果中");
        std::vector<int64_t> ordering = rows;
        if (!sort_ids.empty()) {
            std::stable_sort(ordering.begin(), ordering.end(), [&](int64_t left, int64_t right) {
                for (const auto& id : sort_ids) {
                    const auto& column = Lookup(id);
                    const auto order = CompareCells(column, Value(column, left), Value(column, right));
                    if (order) return direction == "ascending" ? order < 0 : order > 0;
                }
                return false;
            });
        }
        const int64_t begin = std::min<int64_t>(offset, static_cast<int64_t>(ordering.size()));
        const int64_t end = limit < 0 ? static_cast<int64_t>(ordering.size())
                                      : std::min<int64_t>(begin + limit, static_cast<int64_t>(ordering.size()));
        for (int64_t position = begin; position < end; ++position) {
            if ((position & 0x3ff) == 0) Checkpoint();
            const auto row = ordering[static_cast<size_t>(position)];
            std::vector<ExplorationValueV1> values;
            for (const auto& id : project) values.push_back(CellValue(Lookup(id), row));
            result->rows.push_back(std::move(values));
        }
    }

    struct Accumulator {
        uint64_t count = 0;
        uint64_t non_null = 0;
        bool integer = true;
        __int128 integer_sum = 0;
        double real_sum = 0;
        bool finite = true;
        bool has_value = false;
        ExplorationValueV1 minimum = std::monostate{};
        ExplorationValueV1 maximum = std::monostate{};
        std::set<std::string> distinct;
        std::string decimal_sum = "0";
        bool decimal = false;
    };

    static Cell CellFrom(const ExplorationValueV1& value) {
        Cell cell;
        if (auto integer = std::get_if<int64_t>(&value)) {
            cell.kind = CellKind::kInteger;
            cell.integer = *integer;
        } else if (auto real = std::get_if<double>(&value)) {
            cell.kind = CellKind::kReal;
            cell.real = *real;
        } else if (auto text = std::get_if<std::string>(&value)) {
            cell.kind = CellKind::kText;
            cell.text = *text;
        } else if (auto decimal = std::get_if<ExplorationDecimalV1>(&value)) {
            cell.kind = CellKind::kText;
            cell.text = decimal->text;
        }
        return cell;
    }

    void Accumulate(const Column& column, const std::string& aggregate, int64_t row, Accumulator* accumulator) const {
        if (!column.valid[static_cast<size_t>(row)]) return;
        ++accumulator->non_null;
        if (aggregate == "count") return;
        accumulator->has_value = true;
        if (aggregate == "distinctCount") {
            const auto cell = Value(column, row);
            accumulator->distinct.insert(std::string(1, static_cast<char>('0' + static_cast<int>(cell.kind))) +
                                         (cell.kind == CellKind::kInteger ? std::to_string(cell.integer) : "") +
                                         (cell.kind == CellKind::kReal ? DoubleText(cell.real) : "") +
                                         (cell.kind == CellKind::kText ? cell.text : ""));
            return;
        }
        if (aggregate == "min" || aggregate == "max") {
            const auto cell = Value(column, row);
            const auto& current = aggregate == "min" ? accumulator->minimum : accumulator->maximum;
            const bool empty = std::holds_alternative<std::monostate>(current);
            const auto order = empty ? 0 : CompareCells(column, cell, CellFrom(current));
            if (empty || (aggregate == "min" ? order < 0 : order > 0)) {
                if (aggregate == "min")
                    accumulator->minimum = CellValue(column, row);
                else
                    accumulator->maximum = CellValue(column, row);
            }
            return;
        }
        if (column.decimal) {
            // Exact textual accumulation; min/max above already returned with their own value.
            accumulator->decimal = true;
            accumulator->decimal_sum =
                AddExactDecimal(accumulator->decimal_sum, column.decimals[static_cast<size_t>(row)]);
            return;
        }
        const auto cell = Value(column, row);
        if (cell.kind == CellKind::kInteger) {
            accumulator->integer_sum += cell.integer;
            accumulator->real_sum += static_cast<double>(cell.integer);
        } else {
            accumulator->integer = false;
            if (!std::isfinite(cell.real)) accumulator->finite = false;
            accumulator->real_sum += cell.real;
        }
    }

    // Exact decimal addition over canonical text; keeps up to 38 significant digits.
    static std::string AddExactDecimal(const std::string& left, const std::string& right) {
        const auto split = [](const std::string& text, int* sign, std::string* digits, size_t* scale) {
            *sign = 1;
            size_t pos = 0;
            if (!text.empty() && (text[0] == '-' || text[0] == '+')) {
                *sign = text[0] == '-' ? -1 : 1;
                pos = 1;
            }
            digits->clear();
            *scale = 0;
            bool fraction = false;
            for (; pos < text.size(); ++pos) {
                if (text[pos] == '.') {
                    fraction = true;
                    continue;
                }
                if (text[pos] < '0' || text[pos] > '9') throw QueryFailure("UNSUPPORTED_TYPE", "Decimal 文本无效");
                digits->push_back(text[pos]);
                if (fraction) ++*scale;
            }
        };
        int left_sign = 1, right_sign = 1;
        std::string left_digits, right_digits;
        size_t left_scale = 0, right_scale = 0;
        split(left, &left_sign, &left_digits, &left_scale);
        split(right, &right_sign, &right_digits, &right_scale);
        const auto scale = std::max(left_scale, right_scale);
        left_digits += std::string(scale - left_scale, '0');
        right_digits += std::string(scale - right_scale, '0');
        const auto length = std::max(left_digits.size(), right_digits.size()) + 1;
        std::vector<int> sum(length, 0);
        for (size_t i = 0; i < left_digits.size(); ++i)
            sum[i] += (left_digits[left_digits.size() - 1 - i] - '0') * left_sign;
        for (size_t i = 0; i < right_digits.size(); ++i)
            sum[i] += (right_digits[right_digits.size() - 1 - i] - '0') * right_sign;
        const auto normalise = [&sum](int sign) {
            for (size_t i = 0; i + 1 < sum.size(); ++i) {
                if (sum[i] < 0) {
                    sum[i] += 10;
                    --sum[i + 1];
                } else if (sum[i] > 9) {
                    sum[i] -= 10;
                    ++sum[i + 1];
                }
            }
            return sum.back() < 0 ? -1 : 1;
        };
        int sign = normalise(1);
        if (sum.back() < 0) {
            for (auto& digit : sum) digit = -digit;
            sign = normalise(1) < 0 ? -1 : (sign);
            // Flipping signs yields the magnitude of a negative result.
            if (std::any_of(sum.begin(), sum.end(), [](int digit) { return digit != 0; })) sign = -1;
        }
        std::string digits;
        for (auto it = sum.rbegin(); it != sum.rend(); ++it) digits.push_back(static_cast<char>('0' + *it));
        const auto first = digits.find_first_not_of('0');
        digits = first == std::string::npos ? "0" : digits.substr(first);
        if (digits.size() > 38) throw QueryFailure("UNSUPPORTED_TYPE", "Decimal 求和超出 38 位精度");
        if (digits == "0") return "0";
        if (!scale) return (sign < 0 ? "-" : "") + digits;
        if (digits.size() <= scale) digits = std::string(scale - digits.size() + 1, '0') + digits;
        return (sign < 0 ? "-" : "") + digits.substr(0, digits.size() - scale) + "." +
               digits.substr(digits.size() - scale);
    }

    ExplorationValueV1 MeasureValue(const Column* column, const std::string& aggregate,
                                    const Accumulator& accumulator) const {
        if (aggregate == "count") return static_cast<int64_t>(column ? accumulator.non_null : accumulator.count);
        if (aggregate == "distinctCount") return static_cast<int64_t>(accumulator.distinct.size());
        if (!accumulator.has_value) return std::monostate{};
        if (aggregate == "min") return accumulator.minimum;
        if (aggregate == "max") return accumulator.maximum;
        if (aggregate == "sum") {
            if (accumulator.decimal) {
                ExplorationDecimalV1 decimal;
                decimal.text = CanonicalDecimal(accumulator.decimal_sum);
                return decimal;
            }
            if (accumulator.integer) {
                if (accumulator.integer_sum > static_cast<__int128>(kSafeInteger) ||
                    accumulator.integer_sum < -static_cast<__int128>(kSafeInteger))
                    throw QueryFailure("UNSUPPORTED_TYPE", "整数聚合超出浏览器安全数值范围");
                return static_cast<int64_t>(accumulator.integer_sum);
            }
            if (!accumulator.finite) throw QueryFailure("UNSUPPORTED_TYPE", "非有限数值不能聚合");
            return accumulator.real_sum;
        }
        if (aggregate == "mean") {
            if (!accumulator.decimal && !accumulator.integer && !accumulator.finite)
                throw QueryFailure("UNSUPPORTED_TYPE", "非有限数值不能聚合");
            return accumulator.real_sum / static_cast<double>(accumulator.non_null);
        }
        throw QueryFailure("UNSUPPORTED_OPERATION", "此聚合尚不支持");
    }

    void Aggregate(const std::vector<int64_t>& rows, const std::vector<std::string>& group_fields,
                   const std::vector<std::string>& group_units, const std::vector<std::string>& aliases,
                   const std::vector<const rapidjson::Value*>& measures, const std::vector<std::string>& sort_ids,
                   const std::string& direction, int64_t limit, int64_t offset, DataFrameResultV1* result) {
        struct MeasureSpec {
            std::string id;
            std::string aggregate;
            const Column* column = nullptr;
            FieldV1 output;
        };
        std::vector<MeasureSpec> specs;
        for (const auto* measure : measures) {
            const auto id = Text(*measure, "field"), aggregate = Text(*measure, "agg"),
                       alias = Text(*measure, "asFieldKey");
            static const std::set<std::string> supported = {"count", "sum", "mean", "min", "max", "distinctCount"};
            if (!supported.count(aggregate)) throw QueryFailure("UNSUPPORTED_OPERATION", "此聚合尚不支持");
            MeasureSpec spec{alias, aggregate, nullptr, {alias, alias, "number", "", true}};
            if (id != "*" && id != "gw_count_fid") {
                const auto& column = Lookup((*measure)["field"]);
                spec.column = &column;
                if ((aggregate == "sum" || aggregate == "mean") && column.field.type != "number")
                    throw QueryFailure("UNSUPPORTED_TYPE", "该字段不支持数值聚合");
                if (aggregate == "min" || aggregate == "max") {
                    spec.output.type = column.field.type;
                    spec.output.native_type = column.field.native_type;
                }
                if (column.decimal && (aggregate == "mean" || aggregate == "sum")) spec.output.native_type = "decimal";
            } else if (aggregate != "count") {
                Invalid("行数仅支持 count");
            }
            for (const auto& other : specs)
                if (other.id == alias) Invalid("结果字段重复");
            specs.push_back(std::move(spec));
        }
        std::map<GroupKey, std::vector<Accumulator>, KeyLess> groups;
        const auto make_key = [&](int64_t row) {
            GroupKey key;
            for (size_t i = 0; i < group_fields.size(); ++i) {
                const auto& column = Lookup(group_fields[i]);
                key.push_back(group_units[i].empty() ? Part(column, row) : BucketPart(column, group_units[i], row));
            }
            return key;
        };
        // Ungrouped aggregation always produces exactly one row, even for an empty input.
        if (group_fields.empty()) groups.emplace(GroupKey{}, std::vector<Accumulator>(specs.size()));
        size_t processed = 0;
        for (const auto row : rows) {
            if ((++processed & 0x3ff) == 0) Checkpoint();
            auto key = make_key(row);
            auto found = groups.find(key);
            if (found == groups.end())
                found = groups.emplace(std::move(key), std::vector<Accumulator>(specs.size())).first;
            for (size_t i = 0; i < specs.size(); ++i) {
                auto& accumulator = found->second[i];
                ++accumulator.count;
                if (specs[i].column) Accumulate(*specs[i].column, specs[i].aggregate, row, &accumulator);
            }
        }
        for (size_t i = 0; i < group_fields.size(); ++i) {
            if (!group_units[i].empty()) {
                result->fields.push_back({aliases[i], aliases[i], "string", "", true});
            } else {
                auto field = Lookup(group_fields[i]).field;
                field.id = aliases[i];
                field.name = aliases[i];
                result->fields.push_back(field);
            }
        }
        for (const auto& spec : specs) result->fields.push_back(spec.output);
        struct Row {
            GroupKey key;
            std::vector<ExplorationValueV1> values;
        };
        std::vector<Row> output;
        for (const auto& [key, accumulators] : groups) {
            Row row{key, {}};
            for (const auto& part : key) row.values.push_back(KeyValue(part));
            for (size_t i = 0; i < specs.size(); ++i)
                row.values.push_back(MeasureValue(specs[i].column, specs[i].aggregate, accumulators[i]));
            output.push_back(std::move(row));
        }
        if (!sort_ids.empty()) {
            for (const auto& id : sort_ids) {
                const auto found = std::find_if(result->fields.begin(), result->fields.end(),
                                                [&](const FieldV1& field) { return field.id == id; });
                if (found == result->fields.end()) Invalid("排序字段未出现在结果中");
            }
            const auto index_of = [&](const std::string& id) {
                return static_cast<size_t>(std::find_if(result->fields.begin(), result->fields.end(),
                                                        [&](const FieldV1& field) { return field.id == id; }) -
                                           result->fields.begin());
            };
            std::stable_sort(output.begin(), output.end(), [&](const Row& left, const Row& right) {
                for (const auto& id : sort_ids) {
                    const auto index = index_of(id);
                    const auto a = CellFrom(left.values[index]);
                    const auto b = CellFrom(right.values[index]);
                    int order;
                    if (a.kind == CellKind::kNull || b.kind == CellKind::kNull)
                        order = a.kind == b.kind ? 0 : (a.kind == CellKind::kNull ? 1 : -1);
                    else if (a.kind == CellKind::kText || b.kind == CellKind::kText)
                        order = a.text.compare(b.text);
                    else if (result->fields[index].native_type == "decimal")
                        order = CompareDecimalText(
                            a.kind == CellKind::kText
                                ? a.text
                                : DoubleText(a.kind == CellKind::kInteger ? static_cast<double>(a.integer) : a.real),
                            b.kind == CellKind::kText
                                ? b.text
                                : DoubleText(b.kind == CellKind::kInteger ? static_cast<double>(b.integer) : b.real));
                    else {
                        const double x = a.kind == CellKind::kInteger ? static_cast<double>(a.integer) : a.real;
                        const double y = b.kind == CellKind::kInteger ? static_cast<double>(b.integer) : b.real;
                        order = x == y ? 0 : (x < y ? -1 : 1);
                    }
                    if (order) return direction == "ascending" ? order < 0 : order > 0;
                }
                return false;
            });
        }
        const int64_t begin = std::min<int64_t>(offset, static_cast<int64_t>(output.size()));
        const int64_t end = limit < 0 ? static_cast<int64_t>(output.size())
                                      : std::min<int64_t>(begin + limit, static_cast<int64_t>(output.size()));
        for (int64_t position = begin; position < end; ++position)
            result->rows.push_back(std::move(output[static_cast<size_t>(position)].values));
        for (const auto& spec : specs) {
            if (!spec.column || !spec.column->decimal || spec.aggregate != "mean") continue;
            NumericDisplayV1 display;
            display.field_id = spec.id;
            display.mode = "approximate";
            display.origin = "arrow_decimal_mean";
            result->displays.push_back(std::move(display));
        }
    }

    // Fixed-width bins over the filtered non-NULL values: [lower, upper) and a closed last bin.
    void Histogram(const std::vector<int64_t>& rows, const std::string& field_id, uint32_t bins, int64_t limit,
                   int64_t offset, DataFrameResultV1* result) {
        const auto& column = Lookup(field_id);
        std::vector<double> values;
        double minimum = 0, maximum = 0;
        bool seeded = false;
        for (const auto row : rows) {
            if (!column.valid[static_cast<size_t>(row)]) continue;
            double value = 0;
            if (column.decimal) {
                value = std::stod(CanonicalDecimal(column.decimals[static_cast<size_t>(row)]));
            } else {
                const auto cell = Value(column, row);
                if (cell.kind == CellKind::kInteger) {
                    if (cell.integer > kSafeInteger || cell.integer < -kSafeInteger)
                        throw QueryFailure("UNSUPPORTED_TYPE", "整数超出浏览器安全数值范围，不能定量绘图");
                    value = static_cast<double>(cell.integer);
                } else {
                    value = cell.real;
                }
            }
            if (!std::isfinite(value)) throw QueryFailure("UNSUPPORTED_TYPE", "非有限数值不能绘制直方图");
            values.push_back(value);
            if (!seeded) {
                minimum = maximum = value;
                seeded = true;
            } else {
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
            }
        }
        for (const auto* id : {"bin_index", "bin_lower", "bin_upper", "bin_count"})
            result->fields.push_back({id, id, "number", "", false});
        if (values.empty()) return;
        const bool constant = minimum == maximum;
        const double width = constant ? 0 : (maximum - minimum) / static_cast<double>(bins);
        std::vector<uint64_t> counts(bins, 0);
        for (const auto value : values) {
            uint32_t index = bins - 1;
            if (!constant) {
                const auto ratio = std::floor((value - minimum) / width);
                index = ratio >= static_cast<double>(bins) ? bins - 1 : static_cast<uint32_t>(ratio);
            }
            ++counts[index];
        }
        for (uint32_t index = 0; index < bins; ++index) {
            if (!counts[index]) continue;
            const double lower = constant ? minimum : minimum + width * static_cast<double>(index);
            const double upper = index + 1 == bins ? maximum : minimum + width * static_cast<double>(index + 1);
            result->rows.push_back({static_cast<int64_t>(index), lower, upper, static_cast<int64_t>(counts[index])});
        }
        if (column.decimal) {
            NumericDisplayV1 display;
            display.field_id = "bin_lower";
            display.mode = "approximate";
            display.origin = "arrow_decimal_histogram";
            result->displays.push_back(std::move(display));
        }
        if (offset || limit >= 0) {
            const int64_t begin = std::min<int64_t>(offset, static_cast<int64_t>(result->rows.size()));
            const int64_t end = limit < 0 ? static_cast<int64_t>(result->rows.size())
                                          : std::min<int64_t>(begin + limit, static_cast<int64_t>(result->rows.size()));
            std::vector<std::vector<ExplorationValueV1>> kept(result->rows.begin() + begin, result->rows.begin() + end);
            result->rows = std::move(kept);
        }
    }

    std::shared_ptr<arrow::RecordBatch> batch_;
    std::vector<FieldV1> fields_;
    std::unordered_map<std::string, Column> columns_;
    std::unordered_map<std::string, std::pair<std::vector<std::string>, std::vector<bool>>> buckets_;
    uint64_t max_bytes_ = 0;
    size_t budget_ = 0;
    std::chrono::steady_clock::time_point deadline_;
};
}  // namespace

std::vector<FieldV1> DescribeArrowSchema(const arrow::Schema& schema) {
    std::vector<FieldV1> fields;
    for (int i = 0; i < schema.num_fields(); ++i) {
        const auto& field = schema.field(i);
        fields.push_back({"f" + std::to_string(i), field->name(), TypeClass(field->type()), field->type()->ToString(),
                          field->nullable()});
    }
    return fields;
}

std::string ArrowSchemaRevision(const arrow::Schema& schema) {
    const auto encoded = arrow::ipc::SerializeSchema(schema);
    if (!encoded.ok()) return {};
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((*encoded)->data(), static_cast<size_t>((*encoded)->size()), digest);
    std::ostringstream result;
    for (auto byte : digest) result << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
    return result.str();
}

DataFrameResultV1 ExecuteDataFrameWorkflow(const std::shared_ptr<arrow::RecordBatch>& batch,
                                           const std::vector<FieldV1>& fields, const rapidjson::Value& payload,
                                           const std::string& detail_mode, uint32_t max_rows, uint64_t max_bytes,
                                           const std::chrono::steady_clock::time_point& deadline) {
    if (!batch || !batch->schema()) Invalid("快照数据无效");
    if (fields.size() != static_cast<size_t>(batch->num_columns())) Invalid("快照字段与数据不一致");
    return Executor(batch, fields, max_bytes, deadline).Run(payload, detail_mode, max_rows);
}

}  // namespace flowsql::visualization
