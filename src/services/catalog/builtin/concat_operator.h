#ifndef _FLOWSQL_SERVICES_CATALOG_BUILTIN_CONCAT_OPERATOR_H_
#define _FLOWSQL_SERVICES_CATALOG_BUILTIN_CONCAT_OPERATOR_H_

#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/idataframe.h>

#include <string>
#include <vector>

namespace flowsql {
namespace catalog {

// ConcatOperator：按行拼接多个 DataFrame（schema 必须一致）
class ConcatOperator : public IOperator {
 public:
    ConcatOperator() = default;
    ~ConcatOperator() override = default;

    std::string Category() override { return "builtin"; }
    std::string Name() override { return "concat"; }
    std::string Description() override { return "Concatenate multiple DataFrames by rows"; }
    OperatorPosition Position() override { return OperatorPosition::DATA; }

    int Work(IChannel* in, IChannel* out) override;
    int Work(Span<IChannel*> inputs, IChannel* out) override;
    int Configure(const char* key, const char* value) override;
    std::string LastError() override { return last_error_; }

 private:
    static bool SchemaCompatible(const std::vector<Field>& lhs, const std::vector<Field>& rhs);

    std::string last_error_;
};

}  // namespace catalog
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_CATALOG_BUILTIN_CONCAT_OPERATOR_H_
