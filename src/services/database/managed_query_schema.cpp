// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "managed_query_schema.h"

#include "db_session.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <regex>
#include <unordered_map>

namespace flowsql::database {
namespace {
class CanonicalReader {
 public:
    explicit CanonicalReader(std::string_view text) : text_(text) {}
    bool Take(std::string_view expected) {
        if (text_.substr(0, expected.size()) != expected) return false;
        text_.remove_prefix(expected.size());
        return true;
    }
    bool Number(size_t* output, char separator) {
        const auto result = std::from_chars(text_.data(), text_.data() + text_.size(), *output);
        if (result.ec != std::errc() || result.ptr == text_.data() + text_.size() || *result.ptr != separator)
            return false;
        text_.remove_prefix(result.ptr - text_.data() + 1);
        return true;
    }
    bool String(std::string* output) {
        size_t size = 0;
        if (!Number(&size, ':') || size > text_.size()) return false;
        output->assign(text_.substr(0, size));
        text_.remove_prefix(size);
        return true;
    }
    bool Metadata(std::shared_ptr<arrow::KeyValueMetadata>* output) {
        size_t count = 0;
        if (!Number(&count, ':') || count > text_.size()) return false;
        std::vector<std::string> keys, values;
        for (size_t i = 0; i < count; ++i) {
            std::string key, value;
            if (!String(&key) || !String(&value)) return false;
            keys.push_back(std::move(key));
            values.push_back(std::move(value));
        }
        *output = count ? arrow::key_value_metadata(std::move(keys), std::move(values)) : nullptr;
        return true;
    }
    bool empty() const { return text_.empty(); }

 private:
    std::string_view text_;
};

std::shared_ptr<arrow::DataType> CanonicalType(const std::string& name) {
    const std::unordered_map<std::string, std::shared_ptr<arrow::DataType>> types = {
        {"bool", arrow::boolean()},   {"int8", arrow::int8()},
        {"int16", arrow::int16()},    {"int32", arrow::int32()},
        {"int64", arrow::int64()},    {"uint8", arrow::uint8()},
        {"uint16", arrow::uint16()},  {"uint32", arrow::uint32()},
        {"uint64", arrow::uint64()},  {"float", arrow::float32()},
        {"double", arrow::float64()}, {"string", arrow::utf8()},
        {"binary", arrow::binary()},  {"fixed_size_binary[16]", arrow::fixed_size_binary(16)}};
    const auto found = types.find(name);
    return found == types.end() ? nullptr : found->second;
}

arrow::Result<std::shared_ptr<arrow::Schema>> ReadCanonical(std::string_view text) {
    CanonicalReader reader(text);
    size_t count = 0;
    if (!reader.Take("fields=") || !reader.Number(&count, ';') || count > text.size())
        return arrow::Status::Invalid("invalid managed Schema field count");
    std::vector<std::shared_ptr<arrow::Field>> fields;
    for (size_t i = 0; i < count; ++i) {
        std::string name, type;
        size_t nullable = 0;
        std::shared_ptr<arrow::KeyValueMetadata> metadata;
        if (!reader.String(&name) || !reader.Take(";") || !reader.String(&type) || !reader.Take(";") ||
            !reader.Number(&nullable, ';') || nullable > 1 || !reader.Metadata(&metadata) || !reader.Take(";"))
            return arrow::Status::Invalid("invalid managed Schema field");
        auto data_type = CanonicalType(type);
        if (!data_type) return arrow::Status::NotImplemented("unsupported managed Schema type: ", type);
        fields.push_back(arrow::field(name, data_type, nullable != 0, metadata));
    }
    std::shared_ptr<arrow::KeyValueMetadata> metadata;
    if (!reader.Take("schema_metadata=") || !reader.Metadata(&metadata) || !reader.empty())
        return arrow::Status::Invalid("invalid managed Schema metadata");
    return arrow::schema(std::move(fields), std::move(metadata));
}

struct Token {
    std::string text;
    bool identifier = false;
    bool Keyword(std::string_view keyword) const {
        if (identifier || text.size() != keyword.size()) return false;
        for (size_t i = 0; i < text.size(); ++i)
            if (std::toupper(static_cast<unsigned char>(text[i])) != keyword[i]) return false;
        return true;
    }
};

// Only inspect the existing single-relation SELECT shape. SQL execution remains owned by the driver.
std::vector<Token> Tokens(std::string_view sql) {
    std::vector<Token> output;
    for (size_t i = 0; i < sql.size();) {
        const auto ch = static_cast<unsigned char>(sql[i]);
        if (std::isspace(ch)) {
            ++i;
            continue;
        }
        if (sql[i] == '"' || sql[i] == '`' || sql[i] == '\'') {
            const char quote = sql[i++];
            std::string value;
            bool closed = false;
            while (i < sql.size()) {
                const char next = sql[i++];
                if (next == quote) {
                    if (i < sql.size() && sql[i] == quote) {
                        value += quote;
                        ++i;
                    } else {
                        closed = true;
                        break;
                    }
                } else
                    value += next;
            }
            if (!closed) return {};
            output.push_back({quote == '\'' ? "'" : value, quote != '\''});
        } else if (std::isalnum(ch) || sql[i] == '_') {
            const size_t start = i++;
            while (i < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_')) ++i;
            output.push_back({std::string(sql.substr(start, i - start)), false});
        } else {
            output.push_back({std::string(1, sql[i++]), false});
        }
    }
    return output;
}
}  // namespace

arrow::Result<std::shared_ptr<arrow::Schema>> ManagedQuerySchema(IDbSession* session, std::string_view backend,
                                                                 std::string_view query,
                                                                 const std::shared_ptr<arrow::Schema>& physical) {
    const auto tokens = Tokens(query);
    if (tokens.empty() || !tokens[0].Keyword("SELECT")) return physical;
    size_t from = 1;
    int depth = 0;
    for (; from < tokens.size(); ++from) {
        if (tokens[from].text == "(") ++depth;
        if (tokens[from].text == ")") --depth;
        if (!depth && tokens[from].Keyword("FROM")) break;
    }
    if (from + 1 >= tokens.size()) return physical;
    const auto& relation = tokens[from + 1].text;
    static const std::regex managed("npm_[a-z][a-z0-9_]*_(history_v[0-9]+|latest_v[0-9]+|final_v[0-9]+|v[0-9]+_data)");
    if (!std::regex_match(relation, managed)) return physical;
    // Joins and subqueries require column provenance and are outside this single-relation contract.
    depth = 0;
    for (size_t i = from + 2; i < tokens.size(); ++i) {
        if (tokens[i].text == "(") ++depth;
        if (tokens[i].text == ")") --depth;
        if (!depth && (tokens[i].Keyword("JOIN") || tokens[i].Keyword("UNION") || tokens[i].text == ","))
            return physical;
        if (!depth && (tokens[i].Keyword("WHERE") || tokens[i].Keyword("ORDER") || tokens[i].Keyword("GROUP") ||
                       tokens[i].Keyword("LIMIT")))
            break;
    }
    const auto sql = "SELECT schema_canonical FROM npm_result_entities" +
                     std::string(backend == "clickhouse" ? " FINAL" : "") + " WHERE data_table='" + relation +
                     "' OR history_relation='" + relation + "' OR latest_relation='" + relation +
                     "' OR final_relation='" + relation + "'";
    std::string canonical;
    if (backend == "clickhouse") {
        std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
        if (session->ExecuteQueryArrow(sql.c_str(), &batches) != 0)
            return arrow::Status::IOError(session->GetLastError());
        int64_t rows = 0;
        for (const auto& batch : batches) {
            rows += batch->num_rows();
            if (batch->num_rows()) {
                auto scalar = batch->column(0)->GetScalar(0);
                if (!scalar.ok()) return scalar.status();
                canonical = (*scalar)->ToString();
            }
        }
        if (rows != 1) return arrow::Status::Invalid("managed relation requires exactly one Schema contract");
    } else {
        IResultSet* raw = nullptr;
        if (session->ExecuteQuery(sql.c_str(), &raw) != 0) return arrow::Status::IOError(session->GetLastError());
        std::unique_ptr<IResultSet> result(raw);
        const char* text = nullptr;
        size_t size = 0;
        if (!result || !result->Next() || result->GetString(0, &text, &size) != 0 || !text)
            return arrow::Status::Invalid("managed relation Schema contract unavailable");
        canonical.assign(text, size);
        if (result->Next()) return arrow::Status::Invalid("ambiguous managed relation Schema contract");
    }
    ARROW_ASSIGN_OR_RAISE(auto schema, ReadCanonical(canonical));
    std::vector<std::shared_ptr<arrow::Field>> fields = physical->fields();
    size_t column = 0;
    for (size_t begin = 1; begin < from;) {
        size_t end = begin;
        int expression_depth = 0;
        for (; end < from; ++end) {
            if (tokens[end].text == "(") ++expression_depth;
            if (tokens[end].text == ")") --expression_depth;
            if (!expression_depth && tokens[end].text == ",") break;
        }
        size_t name = begin;
        if (name + 2 < end && tokens[name + 1].text == ".") name += 2;
        if (name + 1 == end && tokens[name].text == "*") {
            for (; column < fields.size(); ++column) {
                auto expected = schema->GetFieldByName(fields[column]->name());
                if (expected) fields[column] = expected;
            }
        } else {
            const bool direct =
                name + 1 == end || name + 2 == end || (name + 3 == end && tokens[name + 1].Keyword("AS"));
            if (direct && column < fields.size()) {
                auto expected = schema->GetFieldByName(tokens[name].text);
                if (expected) fields[column] = expected->WithName(fields[column]->name());
            }
            ++column;
        }
        begin = end + 1;
    }
    return arrow::schema(std::move(fields), schema->metadata());
}
}  // namespace flowsql::database
