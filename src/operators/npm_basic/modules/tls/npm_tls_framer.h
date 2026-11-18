// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_TLS_FRAMER_H_
#define FLOWSQL_NPM_TLS_FRAMER_H_

#include "npm_tls_hello.h"

#include <array>
#include <optional>
#include <vector>

namespace flowsql::npm {

enum class NpmTlsFramerError : uint8_t {
    kNone,
    kNotTls,
    kMidstream,
    kGap,
    kCaptureTruncation,
    kMalformedRecord,
    kMalformedHello,
    kHelloLimit,
    kUnsupportedFraming,
    kIncomplete,
    kInvalidInput,
    kAllocationFailed,
};

enum class NpmTlsFramedKind : uint8_t {
    kClientHello,
    kServerHello,
    kOtherHandshake,
    kChangeCipherSpec,
    kApplicationData,
    kAlert,
};

struct NpmTlsFramedEvent {
    NpmTlsFramedKind kind = NpmTlsFramedKind::kOtherHandshake;
    NpmPacketDirection direction = NpmPacketDirection::kAToB;
    uint8_t message_type = 0;
    uint16_t record_length = 0;
    std::array<uint8_t, 2> control_bytes{};
    std::optional<int64_t> first_byte_at_ns;
    std::optional<int64_t> complete_at_ns;
    std::optional<NpmTlsHelloFacts> hello;
};

/** One base session. Borrows Data spans only during Consume; retained body is bounded to one Hello per direction. */
class NpmTlsFramer final {
 public:
    explicit NpmTlsFramer(uint32_t max_hello_bytes);

    NpmTlsFramerError Consume(const NpmTcpStreamContext& context, const NpmTcpStreamEvent& event,
                              std::vector<NpmTlsFramedEvent>* output);
    bool HasCandidate() const noexcept { return candidate_; }
    bool Closed() const noexcept { return closed_; }
    NpmPacketDirection ClientDirection() const noexcept { return client_direction_; }
    std::optional<int64_t> FirstClientByteAtNs() const noexcept { return first_client_byte_at_ns_; }
    size_t RetainedBytes() const noexcept;

 private:
    struct Direction {
        NpmTlsDirectionState framing;
        uint64_t next_offset = 0;
        uint8_t record_type = 0;
        uint16_t record_length = 0;
        uint8_t message_type = 0;
        bool first_message_seen = false;
        bool disabled = false;
        bool message_times_known = true;
        bool record_times_known = true;
        std::optional<int64_t> message_first_at_ns;
        std::optional<int64_t> message_max_at_ns;
        std::optional<int64_t> record_max_at_ns;
        std::array<uint8_t, 2> control_bytes{};
        uint8_t control_bytes_seen = 0;
    };

    NpmTlsFramerError Fail(NpmTlsFramerError error) noexcept;
    NpmTlsFramerError NoCandidate(Direction* direction, NpmTlsFramerError error) noexcept;
    NpmTlsFramerError FinishMessage(Direction* direction, NpmPacketDirection packet_direction,
                                    std::vector<NpmTlsFramedEvent>* output);
    NpmTlsFramerError FinishRecord(Direction* direction, NpmPacketDirection packet_direction,
                                   std::vector<NpmTlsFramedEvent>* output);
    NpmTlsFramerError ConsumeData(Direction* direction, NpmPacketDirection packet_direction,
                                  const NpmTcpStreamEvent& event, std::vector<NpmTlsFramedEvent>* output);

    uint32_t max_hello_bytes_;
    std::array<Direction, 2> directions_{};
    bool candidate_ = false;
    bool closed_ = false;
    NpmPacketDirection client_direction_ = NpmPacketDirection::kAToB;
    std::optional<int64_t> first_client_byte_at_ns_;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_TLS_FRAMER_H_
