// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_COMMON_ERROR_CODE_H_
#define _FLOWSQL_COMMON_ERROR_CODE_H_

#include <cstdint>

namespace flowsql {
namespace error {

// 业务错误码，由 fnRouterHandler 返回
// RouterAgencyPlugin 负责将其映射为 HTTP 状态码
constexpr int32_t OK             =  0;  // 200 OK
constexpr int32_t BAD_REQUEST    = -1;  // 400 Bad Request
constexpr int32_t NOT_FOUND      = -2;  // 404 Not Found
constexpr int32_t CONFLICT       = -3;  // 409 Conflict
constexpr int32_t INTERNAL_ERROR = -4;  // 500 Internal Server Error
constexpr int32_t UNAVAILABLE    = -5;  // 503 Service Unavailable
constexpr int32_t PAYLOAD_TOO_LARGE = -6;  // 413 Payload Too Large

}  // namespace error
}  // namespace flowsql

#endif  // _FLOWSQL_COMMON_ERROR_CODE_H_
