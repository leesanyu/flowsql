// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_NPM_BASIC_NPM_BASIC_RESULT_PROJECTOR_H_
#define _FLOWSQL_PLUGINS_NPM_BASIC_NPM_BASIC_RESULT_PROJECTOR_H_

#include "npm_analysis_contract.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace flowsql::npm {

class NpmProtocolContext;

enum class NpmBasicProjectionError : uint8_t {
    kNone = 0,
    kNullOutput,
    kInvalidSession,
    kInvalidAddress,
    kProtocolNameUnavailable,
    kRevisionExhausted,
    kInvalidResult,
    kAllocationFailed,
};

/** Task-private, single-threaded projector for one ordered stream of session results. */
class NpmBasicResultProjector final {
 public:
    explicit NpmBasicResultProjector(const NpmProtocolContext& protocol_context);

    NpmBasicResultProjector(const NpmBasicResultProjector&) = delete;
    NpmBasicResultProjector& operator=(const NpmBasicResultProjector&) = delete;
    NpmBasicResultProjector(NpmBasicResultProjector&&) = delete;
    NpmBasicResultProjector& operator=(NpmBasicResultProjector&&) = delete;

    /** Emits the next non-final revision. Output and revision state are unchanged on error. */
    NpmBasicProjectionError ProjectActive(const NpmSessionView& session,
                                          int64_t observed_at,
                                          NpmBasicResult* output);

    /** Emits the next final revision and forgets its revision state after success. */
    NpmBasicProjectionError ProjectFinal(const NpmSessionView& session,
                                         NpmSessionEndReason reason,
                                         int64_t observed_at,
                                         NpmBasicResult* output);

    size_t tracked_sessions() const noexcept;

 private:
    NpmBasicProjectionError Project(const NpmSessionView& session,
                                    int64_t observed_at,
                                    bool is_final,
                                    NpmSessionEndReason reason,
                                    NpmBasicResult* output);

    const NpmProtocolContext& protocol_context_;
    std::unordered_map<uint64_t, uint64_t> revisions_;
};

}  // namespace flowsql::npm

#endif  // _FLOWSQL_PLUGINS_NPM_BASIC_NPM_BASIC_RESULT_PROJECTOR_H_
