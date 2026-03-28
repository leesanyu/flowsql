#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IOPERATOR_CATALOG_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IOPERATOR_CATALOG_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <cstdint>
#include <string>
#include <vector>

namespace flowsql {

// {0x0e5b00fd-6383-4c8e-a959-4f2ea3d43812}
const Guid IID_OPERATOR_CATALOG = {
    0x0e5b00fd, 0x6383, 0x4c8e, {0xa9, 0x59, 0x4f, 0x2e, 0xa3, 0xd4, 0x38, 0x12}};

enum class OperatorStatus : int32_t {
    kNotFound = 0,
    kDeactivated = 1,
    kActive = 2,
};

struct OperatorMeta {
    std::string category;
    std::string name;
    std::string type;
    std::string source;
    std::string description;
    std::string position;
};

struct UpsertResult {
    int32_t success_count = 0;
    int32_t failed_count = 0;
    std::string error_message;
};

interface IOperatorCatalog {
    virtual ~IOperatorCatalog() = default;

    virtual OperatorStatus QueryStatus(const std::string& category, const std::string& name) = 0;
    virtual UpsertResult UpsertBatch(const std::vector<OperatorMeta>& operators) = 0;
    virtual int SetActive(const std::string& category, const std::string& name, bool active) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IOPERATOR_CATALOG_H_
