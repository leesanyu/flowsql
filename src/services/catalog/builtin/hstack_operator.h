#ifndef _FLOWSQL_SERVICES_CATALOG_BUILTIN_HSTACK_OPERATOR_H_
#define _FLOWSQL_SERVICES_CATALOG_BUILTIN_HSTACK_OPERATOR_H_

#include <framework/interfaces/ioperator.h>

#include <string>

namespace flowsql {
namespace catalog {

// HstackOperator：按列拼接多个 DataFrame（行数必须一致）
class HstackOperator : public IOperator {
 public:
    HstackOperator() = default;
    ~HstackOperator() override = default;

    std::string Catelog() override { return "builtin"; }
    std::string Name() override { return "hstack"; }
    std::string Description() override { return "Concatenate multiple DataFrames by columns"; }
    OperatorPosition Position() override { return OperatorPosition::DATA; }

    int Work(IChannel* in, IChannel* out) override;
    int Work(Span<IChannel*> inputs, IChannel* out) override;
    int Configure(const char* key, const char* value) override;
    std::string LastError() override { return last_error_; }

 private:
    std::string last_error_;
};

}  // namespace catalog
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_CATALOG_BUILTIN_HSTACK_OPERATOR_H_
