// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "workflow.h"

#include <cctype>
#include <cmath>
#include <set>
#include <unordered_map>

namespace flowsql::visualization {
namespace {
using Json = rapidjson::Value;
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
std::string Join(const std::vector<std::string>& items, const std::string& separator) {
    std::string result;
    for (const auto& item : items) {
        if (!result.empty()) result += separator;
        result += item;
    }
    return result;
}
class Compiler {
 public:
    explicit Compiler(const SourceSchemaV1& source) : source_(source) {
        QuoteIdentifier(source.table, source.dialect);
        for (const auto& field : source.fields) fields_.emplace(field.id, field);
    }
    SqlPlanV1 Compile(const Json& input, const std::string& mode, uint32_t maximum) {
        Keys(input, {"workflow", "limit", "offset"});
        if (!input.HasMember("workflow") || !input["workflow"].IsArray() || input["workflow"].Size() > 32)
            Invalid("无效的 workflow");
        if (mode != "complete" && mode != "limited") Invalid("无效的明细模式");
        std::vector<std::string> filters, select, groups, order;
        bool viewed = false, sorted = false;
        std::string histogram_field;
        uint32_t bins = 0;
        for (const auto& step : input["workflow"].GetArray()) {
            const auto type = Text(step, "type");
            if (type == "filter") {
                Keys(step, {"type", "filters"});
                if (viewed || !step.HasMember("filters") || !step["filters"].IsArray())
                    throw QueryFailure("UNSUPPORTED_OPERATION", "仅支持聚合前过滤");
                for (const auto& filter : step["filters"].GetArray()) filters.push_back(Filter(filter, 0));
            } else if (type == "view") {
                Keys(step, {"type", "query"});
                if (viewed || !step.HasMember("query") || !step["query"].IsArray() || step["query"].Size() != 1)
                    Invalid("每次查询必须有一个单表视图");
                viewed = true;
                const auto& query = step["query"][0];
                const auto op = Text(query, "op");
                if (op == "raw") {
                    Keys(query, {"op", "fields"});
                    if (!query.HasMember("fields") || !query["fields"].IsArray()) Invalid("缺少明细字段");
                    for (const auto& id : query["fields"].GetArray()) {
                        const auto& field = Lookup(id);
                        Output(field);
                        select.push_back(Quote(field.name) + " AS " + Quote(field.id));
                    }
                } else if (op == "histogram") {
                    Keys(query, {"op", "field", "bins"});
                    if (!query.HasMember("field")) Invalid("缺少直方图数值字段");
                    const auto& field = Lookup(query["field"]);
                    if (field.type != "number") throw QueryFailure("UNSUPPORTED_TYPE", "直方图需要数值字段");
                    if (!query.HasMember("bins") || !query["bins"].IsUint() || query["bins"].GetUint() < 1 ||
                        query["bins"].GetUint() > 100)
                        Invalid("箱数须为 1～100 的整数");
                    bins = query["bins"].GetUint();
                    histogram_field = Quote(field.name);
                    plan_.aggregate = true;
                    plan_.numeric_inputs.push_back({field, "histogram", ""});
                    for (const auto* id : {"bin_index", "bin_lower", "bin_upper", "bin_count"}) {
                        Output({id, id, "number", "", false});
                        select.push_back(Quote(id));
                    }
                } else if (op == "aggregate") {
                    Keys(query, {"op", "groupBy", "measures"});
                    if (!query.HasMember("groupBy") || !query["groupBy"].IsArray() || !query.HasMember("measures") ||
                        !query["measures"].IsArray())
                        Invalid("无效的聚合视图");
                    plan_.aggregate = true;
                    for (const auto& id : query["groupBy"].GetArray()) {
                        if (id.IsString()) {
                            const auto& field = Lookup(id);
                            Output(field);
                            groups.push_back(Quote(field.name));
                            select.push_back(Quote(field.name) + " AS " + Quote(field.id));
                        } else {
                            Keys(id, {"field", "unit", "asFieldKey"});
                            const TimeBucketV1 bucket{Text(id, "field"), Text(id, "unit"), Text(id, "asFieldKey")};
                            const auto& field = Lookup(id["field"]);
                            if (!SupportsTimeBucket(field, source_.dialect))
                                throw QueryFailure("UNSUPPORTED_TYPE",
                                                   "时间桶需要日期时间字段；SQLite 可用 ISO 日期文本");
                            if (bucket.unit != "day" && bucket.unit != "month")
                                throw QueryFailure("UNSUPPORTED_OPERATION", "时间桶仅支持 day/month");
                            const auto column = Quote(field.name);
                            std::string expression;
                            if (source_.dialect == "sqlite")
                                expression = "date(substr(" + column + ",1,10)" +
                                             (bucket.unit == "month" ? ",'start of month')" : ")");
                            else if (source_.dialect == "mysql")
                                expression =
                                    "DATE_FORMAT(" + column + ",'%Y-%m-" + (bucket.unit == "month" ? "01')" : "%d')");
                            else if (source_.dialect == "postgres")
                                expression =
                                    "TO_CHAR(" + column + ",'YYYY-MM-" + (bucket.unit == "month" ? "01')" : "DD')");
                            else
                                expression = "formatDateTime(" + column + ",'%Y-%m-" +
                                             (bucket.unit == "month" ? "01')" : "%d')");
                            Output({bucket.as_field_key, bucket.as_field_key, "string", "", true});
                            groups.push_back(expression);
                            select.push_back(expression + " AS " + Quote(bucket.as_field_key));
                        }
                    }
                    for (const auto& measure : query["measures"].GetArray()) {
                        Keys(measure, {"field", "agg", "asFieldKey"});
                        const auto id = Text(measure, "field"), agg = Text(measure, "agg"),
                                   alias = Text(measure, "asFieldKey");
                        static const std::unordered_map<std::string, std::string> aggregates = {
                            {"count", "COUNT"}, {"sum", "SUM"}, {"mean", "AVG"},
                            {"min", "MIN"},     {"max", "MAX"}, {"distinctCount", "COUNT"}};
                        const auto found = aggregates.find(agg);
                        if (found == aggregates.end()) throw QueryFailure("UNSUPPORTED_OPERATION", "此聚合尚不支持");
                        std::string argument;
                        FieldV1 output{alias, alias, "number", "", true};
                        if (id == "*" || id == "gw_count_fid") {
                            if (agg != "count") Invalid("行数仅支持 count");
                            argument = "*";
                        } else {
                            const auto& field = Lookup(measure["field"]);
                            if ((agg == "sum" || agg == "mean") && field.type != "number")
                                throw QueryFailure("UNSUPPORTED_TYPE", "该字段不支持数值聚合");
                            if (agg == "min" || agg == "max") output.type = field.type;
                            argument = Quote(field.name);
                            if (field.type == "number" && agg != "count" && agg != "distinctCount")
                                plan_.numeric_inputs.push_back({field, agg, alias});
                        }
                        Output(output);
                        const auto function = agg == "distinctCount"
                                                  ? (source_.dialect == "clickhouse" ? "uniqExact" : "COUNT")
                                              : source_.dialect == "clickhouse" && agg != "count"
                                                  ? (agg == "mean" ? "avgOrNull" : agg + "OrNull")
                                                  : found->second;
                        select.push_back(
                            function + "(" +
                            (agg == "distinctCount" && source_.dialect != "clickhouse" ? "DISTINCT " : "") + argument +
                            ") AS " + Quote(alias));
                    }
                } else
                    throw QueryFailure("UNSUPPORTED_OPERATION", "不支持此视图操作");
            } else if (type == "sort") {
                Keys(step, {"type", "by", "sort"});
                if (!viewed || sorted || !step.HasMember("by") || !step["by"].IsArray()) Invalid("无效的排序");
                sorted = true;
                const auto direction = Text(step, "sort");
                if (direction != "ascending" && direction != "descending") Invalid("无效的排序方向");
                for (const auto& id : step["by"].GetArray()) {
                    if (!id.IsString() || !outputs_.count(id.GetString())) Invalid("排序字段未出现在结果中");
                    order.push_back(Quote(id.GetString()) + (direction == "ascending" ? " ASC" : " DESC"));
                }
            } else
                throw QueryFailure("UNSUPPORTED_OPERATION", "不支持此表达式或工作流操作");
        }
        if (!viewed || select.empty() || select.size() > 256) Invalid("必须选择有效的字段");
        int64_t limit = -1, offset = 0;
        if (input.HasMember("limit")) {
            if (!input["limit"].IsInt64() || input["limit"].GetInt64() < -1) Invalid("无效的行数限制");
            limit = input["limit"].GetInt64();
        }
        if (input.HasMember("offset")) {
            if (!input["offset"].IsInt64() || input["offset"].GetInt64() < 0) Invalid("无效的偏移量");
            offset = input["offset"].GetInt64();
        }
        if (limit > maximum) throw QueryFailure("RESULT_LIMIT", "请求的行数超过服务上限");
        if (limit >= 0 || offset) plan_.scope = "top_n";
        if (!plan_.aggregate && mode == "limited") plan_.scope = "limited_detail";
        if (limit < 0) limit = (!plan_.aggregate && mode == "limited") ? maximum : uint64_t(maximum) + 1;
        plan_.filtered_from =
            " FROM " + (source_.schema.empty() ? "" : Quote(source_.schema) + ".") + Quote(source_.table);
        plan_.filtered = !filters.empty();
        if (!filters.empty()) plan_.filtered_from += " WHERE " + Join(filters, " AND ");
        if (bins) {
            const auto count = std::to_string(bins);
            const auto v = Quote("v"), lo = Quote("lo"), hi = Quote("hi"), index = Quote("bin_index");
            const auto span = "(" + hi + "-" + lo + ")";
            // SQLite needs a real operand; the other dialects retain native Decimal arithmetic.
            const auto ratio = "((" + v + "-" + lo + ")" + (source_.dialect != "clickhouse" ? "*1.0" : "") + "*" +
                               count + "/NULLIF(" + span + ",0))";
            const auto bucket = source_.dialect == "sqlite"       ? "CAST(" + ratio + " AS INTEGER)"
                                : source_.dialect == "clickhouse" ? "toInt64(FLOOR(" + ratio + "))"
                                                                  : "FLOOR(" + ratio + ")";
            const auto window = "SELECT " + histogram_field + " AS " + v + ",MIN(" + histogram_field + ") OVER () AS " +
                                lo + ",MAX(" + histogram_field + ") OVER () AS " + hi + plan_.filtered_from +
                                (plan_.filtered ? " AND " : " WHERE ") + histogram_field + " IS NOT NULL";
            const auto bucketed = "SELECT " + v + "," + lo + "," + hi + ",CASE WHEN " + lo + "=" + hi +
                                  " THEN 0 WHEN " + v + "=" + hi + " THEN " + std::to_string(bins - 1) + " ELSE " +
                                  bucket + " END AS " + index + " FROM (" + window + ") AS " + Quote("h");
            const auto width = span + (source_.dialect != "clickhouse" ? "*1.0" : "") + "/" + count;
            plan_.sql = "SELECT " + index + ",MIN(" + lo + "+" + index + "*(" + width + ")) AS " + Quote("bin_lower") +
                        ",MIN(CASE WHEN " + index + "=" + std::to_string(bins - 1) + " THEN " + hi + " ELSE " + lo +
                        "+(" + index + "+1)*(" + width + ") END) AS " + Quote("bin_upper") + ",COUNT(*) AS " +
                        Quote("bin_count") + " FROM (" + bucketed + ") AS " + Quote("b") + " GROUP BY " + index;
            if (order.empty()) order.push_back(index + " ASC");
        } else {
            plan_.sql = "SELECT " + Join(select, ",") + plan_.filtered_from;
        }
        if (!groups.empty()) plan_.sql += " GROUP BY " + Join(groups, ",");
        if (!order.empty()) plan_.sql += " ORDER BY " + Join(order, ",");
        plan_.sql += " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);
        return std::move(plan_);
    }

 private:
    std::string Quote(const std::string& name) const { return QuoteIdentifier(name, source_.dialect); }
    const FieldV1& Lookup(const Json& id) {
        if (!id.IsString()) Invalid("字段 ID 必须是字符串");
        const auto it = fields_.find(id.GetString());
        if (it == fields_.end()) Invalid("字段不属于所选数据表");
        if (it->second.type == "unsupported") throw QueryFailure("UNSUPPORTED_TYPE", "不支持该字段类型");
        return it->second;
    }
    void Output(const FieldV1& field) {
        if (field.id.empty() || !outputs_.insert(field.id).second) Invalid("结果字段重复");
        plan_.fields.push_back(field);
    }
    std::string Value(const Json& value) {
        if (plan_.parameters.size() >= 512) Invalid("过滤值过多");
        if (value.IsNull())
            plan_.parameters.emplace_back(std::monostate{});
        else if (value.IsBool())
            plan_.parameters.emplace_back(int64_t(value.GetBool()));
        else if (value.IsInt64())
            plan_.parameters.emplace_back(value.GetInt64());
        else if (value.IsDouble() && std::isfinite(value.GetDouble()))
            plan_.parameters.emplace_back(value.GetDouble());
        else if (value.IsString() && value.GetStringLength() <= 65536)
            plan_.parameters.emplace_back(std::string(value.GetString(), value.GetStringLength()));
        else
            Invalid("不支持该过滤值");
        return "?";
    }
    std::string Filter(const Json& filter, int depth) {
        if (depth > 8 || !filter.IsObject()) Invalid("过滤组合过深");
        if (filter.HasMember("op")) {
            Keys(filter, {"op", "filters"});
            const auto op = Text(filter, "op");
            if ((op != "and" && op != "or") || !filter.HasMember("filters") || !filter["filters"].IsArray() ||
                filter["filters"].Empty())
                Invalid("无效的过滤组合");
            std::vector<std::string> parts;
            for (const auto& part : filter["filters"].GetArray()) parts.push_back(Filter(part, depth + 1));
            return "(" + Join(parts, op == "and" ? " AND " : " OR ") + ")";
        }
        Keys(filter, {"fid", "rule"});
        if (!filter.HasMember("fid") || !filter.HasMember("rule")) Invalid("无效的过滤");
        const auto field = Quote(Lookup(filter["fid"]).name);
        const auto& rule = filter["rule"];
        Keys(rule, {"type", "value", "op"});
        const auto type = Text(rule, "type");
        if (type == "is null") return field + " IS NULL";
        if (type == "is not null") return field + " IS NOT NULL";
        if (!rule.HasMember("value")) Invalid("缺少过滤值");
        const auto& value = rule["value"];
        if (type == "one of" || type == "not in") {
            if (!value.IsArray()) Invalid("集合过滤需要数组");
            std::vector<std::string> values;
            bool null = false;
            for (const auto& item : value.GetArray()) {
                if (item.IsNull())
                    null = true;
                else
                    values.push_back(Value(item));
            }
            const bool inverse = type == "not in";
            std::string result = values.empty() ? (inverse ? "1=1" : "1=0")
                                                : field + (inverse ? " NOT IN (" : " IN (") + Join(values, ",") + ")";
            if (null)
                result =
                    "(" + result + (inverse ? " AND " : " OR ") + field + (inverse ? " IS NOT NULL)" : " IS NULL)");
            return result;
        }
        if (type == "range") {
            if (!value.IsArray() || value.Size() != 2) Invalid("范围需要两个边界");
            std::vector<std::string> parts;
            if (!value[0].IsNull()) parts.push_back(field + ">=" + Value(value[0]));
            if (!value[1].IsNull()) parts.push_back(field + "<=" + Value(value[1]));
            return parts.empty() ? "1=1" : "(" + Join(parts, " AND ") + ")";
        }
        if (type == "comparison") {
            static const std::unordered_map<std::string, std::string> comparisons = {
                {"eq", "="}, {"ne", "<>"}, {"lt", "<"}, {"lte", "<="}, {"gt", ">"}, {"gte", ">="}};
            const auto op = comparisons.find(Text(rule, "op"));
            if (op == comparisons.end()) Invalid("无效的比较操作");
            if (value.IsNull()) {
                if (op->first != "eq" && op->first != "ne") Invalid("NULL 仅支持相等或不等比较");
                return field + (op->first == "eq" ? " IS NULL" : " IS NOT NULL");
            }
            return field + op->second + Value(value);
        }
        throw QueryFailure("UNSUPPORTED_OPERATION", "不支持此过滤操作");
    }
    const SourceSchemaV1& source_;
    std::unordered_map<std::string, FieldV1> fields_;
    std::set<std::string> outputs_;
    SqlPlanV1 plan_;
};
}  // namespace
bool SupportsTimeBucket(const FieldV1& field, const std::string& dialect) {
    if (field.type != "string") return false;
    if (dialect == "sqlite") return true;
    auto native = field.native_type;
    for (auto& c : native) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    while ((native.rfind("NULLABLE(", 0) == 0 || native.rfind("LOWCARDINALITY(", 0) == 0) && native.back() == ')')
        native = native.substr(native.find('(') + 1, native.size() - native.find('(') - 2);
    return native == "DATE" || native == "DATE32" || native.rfind("DATETIME", 0) == 0 ||
           native.rfind("TIMESTAMP", 0) == 0;
}

std::string QuoteIdentifier(const std::string& identifier, const std::string& dialect) {
    if (dialect != "sqlite" && dialect != "mysql" && dialect != "postgres" && dialect != "clickhouse")
        throw QueryFailure("UNSUPPORTED_OPERATION", "不支持该数据库方言");
    const char quote = dialect == "mysql" || dialect == "clickhouse" ? '`' : '"';
    std::string result(1, quote);
    for (const auto c : identifier) {
        if (c == '\0') Invalid("无效的标识符");
        if (dialect == "clickhouse" && c == '\\') result += '\\';
        if (c == quote) result += quote;
        result += c;
    }
    return result + quote;
}
SqlPlanV1 CompileWorkflow(const SourceSchemaV1& source, const rapidjson::Value& payload, const std::string& detail_mode,
                          uint32_t max_rows) {
    return Compiler(source).Compile(payload, detail_mode, max_rows);
}
std::vector<DatabaseParameterV1> BindValues(const std::vector<ExplorationValueV1>& values) {
    std::vector<DatabaseParameterV1> parameters;
    for (const auto& value : values) {
        DatabaseParameterV1 parameter;
        if (auto p = std::get_if<int64_t>(&value)) {
            parameter.kind = DatabaseParameterKindV1::kInt64;
            parameter.int64_value = *p;
        } else if (auto p = std::get_if<double>(&value)) {
            parameter.kind = DatabaseParameterKindV1::kDouble;
            parameter.double_value = *p;
        } else if (auto p = std::get_if<std::string>(&value)) {
            parameter.kind = DatabaseParameterKindV1::kString;
            parameter.data = p->data();
            parameter.size = p->size();
        } else if (auto p = std::get_if<std::vector<uint8_t>>(&value)) {
            parameter.kind = DatabaseParameterKindV1::kBlob;
            parameter.data = p->data();
            parameter.size = p->size();
        }
        parameters.push_back(parameter);
    }
    return parameters;
}
}  // namespace flowsql::visualization
