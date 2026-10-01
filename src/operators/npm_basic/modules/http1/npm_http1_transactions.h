// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_HTTP1_TRANSACTIONS_H_
#define FLOWSQL_NPM_HTTP1_TRANSACTIONS_H_

#include "npm_http1_framer.h"

#include <array>
#include <deque>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace flowsql::npm {

enum class NpmHttp1OutcomeV1 : uint8_t { kMatched, kRequestOnly, kResponseOnly };
enum class NpmHttp1IncompleteReasonV1 : uint8_t {
    kNone,
    kNoPendingRequest,
    kResponseNotObservedByDeadline,
    kPipelineAlignmentLost,
    kRequestPathGap,
    kResponsePathGap,
    kCaptureTruncation,
    kFramingUnsupported,
    kHeaderLimitExceeded,
    kMalformedResponse,
    kResponseStreamEnd,
    kSessionEnd,
};

const char* NpmHttp1OutcomeNameV1(NpmHttp1OutcomeV1 outcome) noexcept;
const char* NpmHttp1IncompleteReasonNameV1(NpmHttp1IncompleteReasonV1 reason) noexcept;

/** Owned transaction facts. The later result encoder supplies normalized endpoint columns. */
struct NpmHttp1TransactionResultV1 {
    uint64_t entity_instance_id = 0;
    uint64_t session_id = 0;
    NpmHttp1OutcomeV1 outcome = NpmHttp1OutcomeV1::kRequestOnly;
    NpmHttp1IncompleteReasonV1 incomplete_reason = NpmHttp1IncompleteReasonV1::kNone;
    int64_t observed_at_ns = 0;
    std::optional<NpmPacketDirection> request_direction;
    std::optional<NpmPacketDirection> response_direction;
    std::optional<NpmHttp1MessageHeadV1> request;
    std::optional<NpmHttp1MessageHeadV1> response;
    std::optional<int64_t> latency_ns;
    uint32_t informational_count = 0;
};

/** Task-private FIFO tracker; successful calls append rows, errors abort the enclosing task. */
class NpmHttp1TransactionTrackerV1 final {
 public:
    explicit NpmHttp1TransactionTrackerV1(NpmHttp1ConfigV1 config, std::shared_ptr<INpmTaskBudget> budget = {})
        : config_(std::move(config)), budget_(std::move(budget)) {}
    ~NpmHttp1TransactionTrackerV1() { Abort(); }

    int OnStreamEvent(uint64_t session_id, NpmPacketDirection direction, NpmTcpStreamOriginV1 origin,
                      const NpmTcpStreamEventV1& event, int64_t observed_at_ns,
                      std::vector<NpmHttp1TransactionResultV1>* results);
    int OnCaptureWatermark(std::optional<int64_t> watermark_ns, std::vector<NpmHttp1TransactionResultV1>* results);
    std::optional<int64_t> NextEventDeadlineNs() const noexcept;
    int OnSessionEnd(uint64_t session_id, NpmSessionEndReason reason, int64_t observed_at_ns,
                     std::vector<NpmHttp1TransactionResultV1>* results);
    void Abort() noexcept;
    void ReleaseResults(std::vector<NpmHttp1TransactionResultV1>* results) noexcept;
    size_t ActiveSessions() const noexcept { return sessions_.size(); }

 private:
    struct Pending {
        uint64_t entity_instance_id = 0;
        uint64_t request_message_id = 0;
        NpmHttp1MessageHeadV1 request;
        bool request_body_complete = false;
        std::optional<int64_t> deadline_ns;
        uint32_t informational_count = 0;
        uint64_t response_message_id = 0;
        std::optional<NpmHttp1MessageHeadV1> response;
        bool response_body_complete = false;
    };
    struct Orphan {
        uint64_t entity_instance_id = 0;
        uint64_t message_id = 0;
        NpmHttp1MessageHeadV1 response;
        bool body_complete = false;
    };
    struct Session {
        explicit Session(uint32_t max_header_bytes)
            : framers{NpmHttp1FramerV1(max_header_bytes), NpmHttp1FramerV1(max_header_bytes)} {}
        std::array<NpmHttp1FramerV1, 2> framers;
        std::optional<NpmPacketDirection> request_direction;
        std::deque<Pending> pending;
        std::optional<Orphan> orphan;
        bool sealed = false;
        bool seal_after_tunnel = false;
        uint64_t reserved_bytes = 0;
    };

    int ProcessMessages(uint64_t session_id, Session* session, NpmPacketDirection direction,
                        const std::vector<NpmHttp1FramedMessageV1>& messages, int64_t observed_at_ns,
                        std::vector<NpmHttp1TransactionResultV1>* results);
    int EmitReady(uint64_t session_id, Session* session, int64_t observed_at_ns,
                  std::vector<NpmHttp1TransactionResultV1>* results);
    void Seal(uint64_t session_id, Session* session, NpmHttp1IncompleteReasonV1 first_reason,
              NpmHttp1IncompleteReasonV1 later_reason, int64_t observed_at_ns,
              std::vector<NpmHttp1TransactionResultV1>* results);
    NpmHttp1ResponseContextV1 ResponseContext(const Session& session,
                                              const std::vector<NpmHttp1FramedMessageV1>& messages) const;
    uint64_t NextId() noexcept;
    bool Reserve(uint64_t bytes);
    void Release(uint64_t bytes) noexcept;
    uint64_t SessionCharge() const noexcept;
    uint64_t PendingCharge() const noexcept;
    uint64_t EventCharge(size_t bytes) const noexcept;

    NpmHttp1ConfigV1 config_;
    std::shared_ptr<INpmTaskBudget> budget_;
    std::map<uint64_t, Session> sessions_;
    uint64_t next_id_ = 1;
    std::optional<int64_t> watermark_ns_;
    bool aborted_ = false;
    uint64_t result_charges_ = 0;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_HTTP1_TRANSACTIONS_H_
