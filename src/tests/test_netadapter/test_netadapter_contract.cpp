// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/netadapter/netadapter_config.h>
#include <framework/core/capture_progress_tracker.h>
#include <cassert>
#include <type_traits>
using namespace flowsql;
using namespace flowsql::channels::netadapter;
int main() {
    static_assert(std::is_abstract_v<ICaptureBlockStreamReader>);
    CaptureBackendConfig config;
    std::string normalized, error;
    auto parse = [&](const char* text) { return ParseNetAdapterConfig(text, &config, &normalized, &error); };
    assert(parse(R"({"backend":"af_packet","interfaces":["eth1","eth2"]})") == 0);
    assert(config.snaplen == 65535 && config.promiscuous && config.buffer_bytes == 64 * kMiB);
    for (const char* invalid :
         {"{}", R"({"backend":"wrong","interfaces":["eth1"]})", R"({"backend":"af_packet","interfaces":[]})",
          R"({"backend":"af_packet","interfaces":["eth1","eth1"]})",
          R"({"backend":"af_packet","interfaces":["eth1"],"extra":1})",
          R"({"backend":"af_packet","interfaces":["eth1"],"snaplen":0})",
          R"({"backend":"af_packet","interfaces":["eth1"],"snaplen":65536})",
          R"({"backend":"af_packet","interfaces":["eth1"],"promiscuous":1})",
          R"({"backend":"af_packet","interfaces":["eth1"],"buffer_mib":2})",
          R"({"backend":"af_packet","interfaces":["eth1"],"buffer_mib":64,"buffer_mib":64})"})
        assert(parse(invalid) != 0);
    auto sources = DefaultSourceSet();
    CaptureQueueIdentity a;
    a.source_name = "eth1";
    a.generation = 1;
    a.link_type = 1;
    a.observation_domain_id = 7;
    auto b = a;
    b.source_name = "eth2";
    b.source_id = 1;
    sources.inputs = {a, b};
    assert(ValidateCaptureSourceSet(sources) == CaptureDescriptionError::kNone);
    sources.inputs[1].generation = 2;
    assert(ValidateCaptureSourceSet(sources) != CaptureDescriptionError::kNone);
    sources.inputs[1] = a;
    assert(ValidateCaptureSourceSet(sources) != CaptureDescriptionError::kNone);
    sources.inputs = {a};
    assert(ValidateCaptureSourceSet(sources) == CaptureDescriptionError::kNone);
    sources.max_poll_work_ms = 0;
    assert(ValidateCaptureSourceSet(sources) == CaptureDescriptionError::kInvalidLimits);
    sources = DefaultSourceSet();
    sources.inputs = {a, b};
    MultiInputCaptureProgressTracker tracker(sources);
    CaptureProgress fa;
    fa.generation = 1;
    fa.fact_sequence = 1;
    fa.capture_time_ns = 100;
    fa.source_idle_confirmed = true;
    fa.backlog = CaptureBacklog::kEmpty;
    auto fb = fa;
    fb.source_id = 1;
    fb.capture_time_ns = 50;
    assert(tracker.Observe({fa}) == CaptureProgressError::kNone);
    assert(!tracker.CommonCandidateNs());
    assert(tracker.Observe({fb}) == CaptureProgressError::kNone);
    assert(tracker.CommonCandidateNs() == 50);
    fa.fact_sequence = 2;
    fa.capture_time_ns = 200;
    fb.fact_sequence = 2;
    fb.generation = 2;
    assert(tracker.Observe({fa, fb}) != CaptureProgressError::kNone);
    assert(tracker.CommonCandidateNs() == 50);
    fb.generation = 1;
    fb.backlog = CaptureBacklog::kPresent;
    assert(tracker.Observe({fa, fb}) == CaptureProgressError::kNone);
    assert(!tracker.CommonCandidateNs());
    fb.fact_sequence = 3;
    fb.capture_time_ns = 150;
    fb.backlog = CaptureBacklog::kEmpty;
    assert(tracker.Observe({fb}) == CaptureProgressError::kNone);
    assert(tracker.CommonCandidateNs() == 150);
    fb.fact_sequence = 4;
    fb.capture_time_ns = 120;
    assert(tracker.Observe({fb}) == CaptureProgressError::kNone);
    assert(!tracker.CommonCandidateNs());
}
