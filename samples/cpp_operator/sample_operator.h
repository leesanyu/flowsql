#ifndef _FLOWSQL_SAMPLES_CPP_OPERATOR_SAMPLE_OPERATOR_H_
#define _FLOWSQL_SAMPLES_CPP_OPERATOR_SAMPLE_OPERATOR_H_

#include <framework/interfaces/ioperator.h>

#include <string>

class ColumnStatsOperator : public flowsql::IOperator {
 public:
    std::string Category() override;
    std::string Name() override;
    std::string Description() override;
    flowsql::OperatorPosition Position() override;
    int Work(flowsql::IChannel* in, flowsql::IChannel* out) override;
    int Configure(const char* key, const char* value) override;
    std::string LastError() override { return last_error_; }

 private:
    std::string last_error_;
};

#endif  // _FLOWSQL_SAMPLES_CPP_OPERATOR_SAMPLE_OPERATOR_H_
