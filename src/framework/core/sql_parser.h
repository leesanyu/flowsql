#ifndef _FLOWSQL_FRAMEWORK_CORE_SQL_PARSER_H_
#define _FLOWSQL_FRAMEWORK_CORE_SQL_PARSER_H_

#include <string>
#include <unordered_map>
#include <vector>

namespace flowsql {

struct OperatorRef {
    std::string catelog;
    std::string name;
};

// SQL 解析结果
struct SqlStatement {
    std::string source;       // FROM 后的源通道名
    std::vector<std::string> sources;  // FROM 后的全部源通道名（source=sources[0]）
    std::string op_catelog;   // USING 后的算子 catelog（可选，空表示无算子）
    std::string op_name;      // USING 后的算子 name（可选）
    std::vector<OperatorRef> operators;  // USING/THEN 算子链
    std::vector<std::unordered_map<std::string, std::string>> operator_with_params;  // 与 operators 对齐
    std::unordered_map<std::string, std::string> with_params;  // WITH key=val,...
    std::string dest;         // INTO 后的目标通道名（可选，空表示直接返回结果）
    std::vector<std::string> columns;  // SELECT 后的列名（空表示 *）
    std::string where_clause; // WHERE 后的过滤条件（可选，空表示无过滤）
    std::string sql_part;     // 完整 SQL 部分（不含 USING/WITH/INTO），数据库通道直接使用
    std::string error;        // 解析错误信息（空表示成功）

    // 是否有算子
    bool HasOperator() const {
        if (!operators.empty()) return true;
        return !op_catelog.empty() && !op_name.empty();
    }
};

// 递归下降 SQL 解析器
// 语法：SELECT [* | col1, col2, ...] FROM <source> [WHERE <condition>]
//       [USING <catelog.name> [WITH key=val,...] (THEN <catelog.name> [WITH ...])*] [INTO <dest>]
class SqlParser {
 public:
    SqlStatement Parse(const std::string& sql);

    // 验证 WHERE 子句安全性（拒绝 SQL 注入关键字）
    static bool ValidateWhereClause(const std::string& clause);

 private:
    // 词法辅助
    void SkipWhitespace();
    bool MatchKeyword(const char* keyword);
    std::string ReadIdentifier();
    std::string ReadValue();

    const char* pos_ = nullptr;
    const char* end_ = nullptr;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_SQL_PARSER_H_
