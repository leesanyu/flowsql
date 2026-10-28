// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <common/guid.h>
#include <common/typedef.h>
#include <memory>
#include <string>

namespace flowsql {
const Guid IID_DATA_SEMANTICS_REGISTRY_V1 = {
    0x57b13e82, 0x05ab, 0x451c, {0xa6, 0xf1, 0x33, 0x98, 0x81, 0x70, 0x41, 0x2d}};
struct DataSemanticsErrorV1 {
    std::string code, message, field_path;
};
struct DataSemanticsSnapshotV1 {
    std::string exact_reference, schema_id, sha256;
    std::shared_ptr<const std::string> document_json;
};
interface IDataSemanticsRegistryV1 {
    virtual ~IDataSemanticsRegistryV1() = default;
    virtual int Resolve(const char* exact_reference, DataSemanticsSnapshotV1* out, DataSemanticsErrorV1* error) = 0;
};
}  // namespace flowsql
