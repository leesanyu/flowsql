// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_HTTP1_FRAMER_H_
#define FLOWSQL_NPM_HTTP1_FRAMER_H_

#include "npm_http1_contract.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flowsql::npm {

enum class NpmHttp1FramerError : uint8_t {
    kNone,
    kMidstream,
    kGap,
    kMalformed,
    kUnsupported,
    kHeaderLimit,
    kIncomplete,
    kInvalidOutput,
    kAllocationFailed,
};

/** Header is emitted as soon as validated; body completion is a separate event with the same message ID. */
struct NpmHttp1FramedMessage {
    uint64_t message_id = 0;
    NpmHttp1MessageHead head;
    bool headers_complete = true;
    bool body_complete = false;
};

/** Response framing facts supplied by the future FIFO owner at each response header. */
struct NpmHttp1ResponseContext {
    bool request_is_head = false;
    bool request_is_connect = false;
};

/** One TCP direction. Borrowed input is consumed synchronously and never retained. */
class NpmHttp1Framer final {
 public:
    explicit NpmHttp1Framer(uint32_t max_header_bytes);

    /** response_context is consulted for each response header, including multiple headers in one Data event. */
    NpmHttp1FramerError Consume(NpmTcpStreamOrigin origin, std::string_view bytes,
                                std::optional<int64_t> captured_at_ns, std::vector<NpmHttp1FramedMessage>* messages,
                                const std::function<NpmHttp1ResponseContext()>& response_context = {});
    NpmHttp1FramerError Consume(const NpmTcpStreamEvent& event, NpmTcpStreamOrigin origin,
                                std::vector<NpmHttp1FramedMessage>* messages,
                                const std::function<NpmHttp1ResponseContext()>& response_context = {});
    NpmHttp1FramerError OnGap() noexcept;
    NpmHttp1FramerError OnEnd(std::vector<NpmHttp1FramedMessage>* messages) noexcept;
    bool Disabled() const noexcept { return stage_ == Stage::kDisabled; }
    size_t RetainedBytes() const noexcept { return header_.size() + line_.size(); }

 private:
    enum class Stage : uint8_t {
        kHeaders,
        kFixedBody,
        kChunkLine,
        kChunkBody,
        kChunkCr,
        kChunkLf,
        kTrailers,
        kCloseBody,
        kDisabled,
    };

    NpmHttp1FramerError ParseHeaders(const std::function<NpmHttp1ResponseContext()>& response_context,
                                     std::vector<NpmHttp1FramedMessage>* messages);
    NpmHttp1FramerError FinishMessage(std::vector<NpmHttp1FramedMessage>* messages);
    NpmHttp1FramerError AppendControlByte(char byte, std::string* target);
    NpmHttp1FramerError Fail(NpmHttp1FramerError error) noexcept;

    uint32_t max_header_bytes_;
    Stage stage_ = Stage::kHeaders;
    bool started_ = false;
    uint32_t control_bytes_seen_ = 0;
    uint64_t remaining_ = 0;
    uint64_t next_message_id_ = 1;
    uint64_t current_message_id_ = 0;
    bool stop_after_current_ = false;
    std::string header_;
    std::string line_;
    NpmHttp1MessageHead current_;
    bool header_times_known_ = true;
    std::optional<int64_t> max_header_time_ns_;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_HTTP1_FRAMER_H_
