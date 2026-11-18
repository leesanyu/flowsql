// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_FRAMEWORK_INTERFACES_ICAPTURE_BACKEND_H_
#define FLOWSQL_FRAMEWORK_INTERFACES_ICAPTURE_BACKEND_H_

#include <framework/interfaces/icapture_block_stream_reader.h>
#include <memory>
#include <string>
#include <vector>

namespace flowsql {
// {e2967cc1-cc38-43c6-8f21-553b0ab8e912}
const Guid IID_CAPTURE_BACKEND_PROVIDER = {
    0xe2967cc1, 0xcc38, 0x43c6, {0x8f, 0x21, 0x55, 0x3b, 0x0a, 0xb8, 0xe9, 0x12}};

struct CaptureBackendConfig {
    std::string backend;
    std::vector<std::string> interfaces;
    bool promiscuous = true;
    uint32_t snaplen = 65535;
    uint64_t buffer_bytes = 64ULL * 1024 * 1024;
};

struct CaptureBackendInput {
    std::string interface_name;
    uint32_t ifindex = 0;
    uint32_t queue_id = 0;
    bool physical_queue = false;
    int readiness_fd = -1;
    uint64_t buffer_bytes = 0;
    std::string timestamp_source;
    std::string counter_source;
};

/** Borrowed bytes valid until ReleasePacket; exactly one outstanding view per input. */
struct CapturePacketView {
    const uint8_t* bytes = nullptr;
    uint32_t captured_len = 0;
    uint32_t wire_len = 0;
    int64_t timestamp_ns = 0;
};

interface ICaptureBackendSession {
    virtual ~ICaptureBackendSession() = default;
    virtual const std::vector<CaptureBackendInput>& Inputs() const = 0;
    /** Nonblocking: 0 packet, EAGAIN confirmed empty, other error fails the complete reader. */
    virtual int TryRead(uint32_t input, CapturePacketView * packet) = 0;
    virtual void ReleasePacket(uint32_t input) = 0;
    /** Present/unknown pauses safe progress; called after consuming the current borrowed view. */
    virtual CaptureBacklog Backlog(uint32_t input) const = 0;
    /** Same-domain conservative idle timestamp; used only after TryRead confirms empty. */
    virtual int64_t IdleTimeNs(uint32_t input) const = 0;
    virtual int ReadCounters(uint32_t input, CaptureCounters * counters) = 0;
    /** Thread-safe wake/abort; all other methods are serialized by the reader. */
    virtual void Cancel() = 0;
};

interface ICaptureBackendProvider {
    virtual ~ICaptureBackendProvider() = default;
    virtual const char* Backend() const = 0;
    /** Opens the complete declared scope or rolls back; output null on failure. */
    virtual int Open(const CaptureBackendConfig& config, std::shared_ptr<ICaptureBackendSession>* session,
                     std::string* error) = 0;
};
}  // namespace flowsql
#endif
