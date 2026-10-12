// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_RESULT_PROJECTOR_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_RESULT_PROJECTOR_H_

#include <operators/npm_basic/npm_analysis_contract.h>

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
    template <typename Address>
    NpmBasicProjectionError ProjectActive(const NpmSessionView& session, int64_t observed_at,
                                          NpmBasicFamilyResult<Address>* output);

    /** Emits the next final revision and forgets its revision state after success. */
    template <typename Address>
    NpmBasicProjectionError ProjectFinal(const NpmSessionView& session, NpmSessionEndReason reason, int64_t observed_at,
                                         NpmBasicFamilyResult<Address>* output);

    NpmBasicProjectionError ProjectAndWrite(const NpmSessionView& session, int64_t observed_at,
                                            std::optional<NpmSessionEndReason> reason, INpmResultWriter& writer,
                                            int* write_error, std::optional<NpmBasicPeriodStats> period = std::nullopt);
    size_t tracked_sessions() const noexcept;

 private:
    template <typename Address>
    NpmBasicProjectionError Project(const NpmSessionView& session, int64_t observed_at, bool is_final,
                                    NpmSessionEndReason reason, NpmBasicFamilyResult<Address>* output);

    const NpmProtocolContext& protocol_context_;
    std::unordered_map<uint64_t, uint64_t> revisions_;
};

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_RESULT_PROJECTOR_H_
