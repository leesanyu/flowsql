// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_DNS_RESULT_ENCODER_H_
#define FLOWSQL_NPM_DNS_RESULT_ENCODER_H_

#include "npm_dns_udp.h"

#include <memory>
#include <string>

namespace arrow {
class MemoryPool;
}

namespace flowsql::npm {

/** Owned session facts retained for DNS rows that outlive the borrowed session view. */
struct NpmDnsSessionIdentityV1 {
    uint64_t session_id = 0;
    uint64_t observation_domain_id = 0;
    uint8_t transport_protocol = 0;
    std::string a_ip;
    std::string b_ip;
    uint16_t a_port = 0;
    uint16_t b_port = 0;
};

/** Builds and emits one final event row; the temporary module-state charge spans the emitter call. */
int EmitNpmDnsTransactionV1(const NpmDnsUdpResultV1& result, const NpmDnsSessionIdentityV1& session,
                            const std::shared_ptr<INpmTaskBudget>& budget, INpmResultEmitterV1& emitter,
                            arrow::MemoryPool* pool = nullptr);

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_DNS_RESULT_ENCODER_H_
