// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <common/guid.h>
#include <common/typedef.h>
#include <string>

namespace flowsql {
inline const Guid IID_METADATA_DESCRIPTION_V1 = {
    0x448b8139, 0x63dc, 0x497c, {0xb4, 0xd8, 0x70, 0x95, 0x32, 0x1d, 0x8f, 0x61}};
// Thread safe, owned JSON envelopes as specified by semantics/api.schema.json.
// Reads never load business rows or execute operators. Saves require captured basis.
// This capability is independent of all execution and provider virtual tables.
interface IMetadataDescriptionV1 {
    virtual ~IMetadataDescriptionV1() = default;
    virtual int Describe(const char* request_json, std::string* response_json) = 0;
    virtual int SaveChannel(const char* request_json, std::string* response_json) = 0;
    virtual int SaveOperator(const char* request_json, std::string* response_json) = 0;
};
}  // namespace flowsql
