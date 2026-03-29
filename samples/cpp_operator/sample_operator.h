#ifndef _FLOWSQL_SAMPLES_CPP_OPERATOR_SAMPLE_OPERATOR_H_
#define _FLOWSQL_SAMPLES_CPP_OPERATOR_SAMPLE_OPERATOR_H_

#include <framework/interfaces/ioperator.h>

#include <string>

class SampleOperatorBase : public flowsql::IOperator {
 public:
    int Configure(const char* key, const char* value) override;
    std::string LastError() override { return last_error_; }

 protected:
    void SetLastError(const std::string& msg) { last_error_ = msg; }

 private:
    std::string last_error_;
};

class PassThroughOperator : public SampleOperatorBase {
 public:
    std::string Category() override;
    std::string Name() override;
    std::string Description() override;
    flowsql::OperatorPosition Position() override;
    int Work(flowsql::IChannel* in, flowsql::IChannel* out) override;
};

class RowCountOperator : public SampleOperatorBase {
 public:
    std::string Category() override;
    std::string Name() override;
    std::string Description() override;
    flowsql::OperatorPosition Position() override;
    int Work(flowsql::IChannel* in, flowsql::IChannel* out) override;
};

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
