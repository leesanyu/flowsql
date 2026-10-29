// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/idata_semantics_registry.h>

namespace flowsql {
inline const Guid IID_PYTHON_OPERATOR_PUBLISHER_V1 = {
    0x1686d582, 0xb056, 0x4ddd, {0xa5, 0x35, 0xc0, 0x46, 0xc5, 0x15, 0xc0, 0x45}};
// Control-plane publication. Worker observes the actual source bytes; Catalog
// validates every declaration before switching the component's association.
interface IPythonOperatorPublisherV1 {
    virtual ~IPythonOperatorPublisherV1() = default;
    virtual int ReadPythonSource(const char* category, const char* name, std::string* response_json,
                                 DataSemanticsErrorV1* error) = 0;
    virtual int PublishPythonSource(const char* filename, const char* source, const char* expected_component_revision,
                                    std::string* response_json, DataSemanticsErrorV1* error) = 0;
};
}  // namespace flowsql
