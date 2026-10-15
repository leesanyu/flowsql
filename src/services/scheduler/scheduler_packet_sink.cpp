// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arrow/api.h>
#include <framework/core/filter_planner.h>
#include <framework/core/pipeline.h>
#include <framework/interfaces/iblock_stream_schema.h>
#include <framework/interfaces/ipacket_family.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <cerrno>
#include <chrono>
#include "scheduler_plugin.h"
namespace flowsql::scheduler {
namespace {
using Json = rapidjson::Document;
using Allocator = rapidjson::Document::AllocatorType;
void Text(rapidjson::Value& v, const char* key, const std::string& value, Allocator& a) {
    v.AddMember(rapidjson::Value(key, a), rapidjson::Value(value.data(), value.size(), a), a);
}
std::string Encode(const rapidjson::Value& v) {
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    v.Accept(w);
    return b.GetString();
}
rapidjson::Value Counts(const PacketSinkCountsV1& c, Allocator& a) {
    rapidjson::Value v(rapidjson::kObjectType);
    v.AddMember("input", c.input, a);
    v.AddMember("confirmed", c.confirmed, a);
    v.AddMember("not_sent", c.not_sent, a);
    v.AddMember("unknown", c.unknown, a);
    return v;
}
rapidjson::Value Counters(ICaptureBlockStreamReaderV2* reader, const CaptureSourceSetV2& sources, Allocator& a) {
    rapidjson::Value rows(rapidjson::kArrayType);
    for (const auto& input : sources.inputs) {
        rapidjson::Value row(rapidjson::kObjectType);
        CaptureCountersV1 c;
        const int rc = reader ? reader->ReadInputCounters(input.source_id, &c) : ENOTSUP;
        if (rc || c.generation != input.generation) c.available_mask = 0;
        row.AddMember("source_id", input.source_id, a);
        row.AddMember("queue_id", input.queue_id, a);
        row.AddMember("generation", c.generation, a);
        row.AddMember("available_mask", c.available_mask, a);
        row.AddMember("read_code", rc, a);
        auto field = [&](const char* key, uint32_t bit, uint64_t value) {
            rapidjson::Value v;
            if (c.available_mask & bit) v.SetUint64(value);
            row.AddMember(rapidjson::Value(key, a), v, a);
        };
        field("received_packets", kCaptureReceivedPacketsAvailable, c.received_packets);
        field("delivered_packets", kCaptureDeliveredPacketsAvailable, c.delivered_packets);
        field("delivered_bytes", kCaptureDeliveredBytesAvailable, c.delivered_bytes);
        field("source_dropped_packets", kCaptureSourceDroppedPacketsAvailable, c.source_dropped_packets);
        field("queue_dropped_packets", kCaptureQueueDroppedPacketsAvailable, c.queue_dropped_packets);
        field("backpressure_events", kCaptureBackpressureEventsAvailable, c.backpressure_events);
        rows.PushBack(row, a);
    }
    return rows;
}
}  // namespace
int32_t SchedulerPlugin::TryExecutePacketSink(const SourceResolveResult& resolved, const SqlStatement& stmt,
                                              std::string& response, bool* handled) {
    *handled = false;
    std::unique_ptr<IPacketSinkWriterV1> writer;
    PacketSinkReportV1 fallback;
    fallback.state = "failed";
    fallback.code = "PACKET_SINK_UNAVAILABLE";
    fallback.stage = "sink_resolve";
    const auto control = SchedulerBatchRuntime::CurrentControl();
    std::shared_ptr<IBlockStreamChannel> reader;
    ICaptureBlockStreamReaderV2* capture = nullptr;
    CaptureSourceSetV2 sources;
    bool source_failed = false, stopped = false;
    uint64_t delivery_waits = 0, delivery_wait_ns = 0;
    auto publication = [&] {
        auto report = writer ? writer->Report() : fallback;
        if (source_failed) {
            report.state = "failed";
            report.code = "PACKET_SOURCE_CAPTURE_FAILED";
            report.stage = "source_capture";
        } else if (fallback.code != "PACKET_SINK_UNAVAILABLE") {
            report.state = fallback.state;
            report.code = fallback.code;
            report.stage = fallback.stage;
        }
        Json summary;
        summary.SetObject();
        auto& a = summary.GetAllocator();
        rapidjson::Value p(rapidjson::kObjectType);
        Text(p, "target", stmt.dest, a);
        Text(p, "state", report.state, a);
        Text(p, "code", report.code, a);
        Text(p, "stage", report.stage, a);
        p.AddMember("messages", Counts(report.messages, a), a);
        p.AddMember("packets", Counts(report.packets, a), a);
        Json binding;
        binding.Parse(report.binding_json.c_str());
        p.AddMember("binding",
                    binding.IsObject() ? rapidjson::Value(binding, a) : rapidjson::Value(rapidjson::kObjectType), a);
        p.AddMember("capture_counters", Counters(capture, sources, a), a);
        p.AddMember("delivery_waits", delivery_waits, a);
        p.AddMember("delivery_wait_ns", delivery_wait_ns, a);
        summary.AddMember("publication", p, a);
        return summary;
    };
    auto finish = [&](int rc, const char* status) {
        auto summary = publication();
        const auto packets = summary["publication"]["packets"]["confirmed"].GetUint64();
        if (control) control->Publish(Encode(summary));
        Json out;
        out.SetObject();
        auto& a = out.GetAllocator();
        Text(out, "status", status, a);
        out.AddMember("rows", packets, a);
        out.AddMember("rows_affected", packets, a);
        out.AddMember("result_row_count", packets, a);
        Text(out, "result_target", stmt.dest, a);
        if (rc) {
            Text(out, "error", summary["publication"]["code"].GetString(), a);
            Text(out, "error_code", summary["publication"]["code"].GetString(), a);
            Text(out, "error_stage", summary["publication"]["stage"].GetString(), a);
        }
        out.AddMember("result", rapidjson::Value(summary, a), a);
        response = Encode(out);
        return rc ? error::INTERNAL_ERROR : error::OK;
    };
    auto reject = [&](const char* code, int rc = EINVAL) {
        fallback.code = code;
        *handled = true;
        return finish(rc, "failed");
    };
    int create = ENOTSUP;
    try {
        if (querier_)
            querier_->Traverse(IID_PACKET_SINK_FACTORY_V1, [&](void* p) {
                create =
                    static_cast<IPacketSinkFactoryV1*>(p)->CreateWriter({1, stmt.dest, NextStreamTaskId()}, &writer);
                return create == ENOTSUP ? 0 : -1;
            });
        const bool netadapter = resolved.channels.size() == 1 && resolved.channels[0] &&
                                resolved.channels[0]->Category() &&
                                std::string(resolved.channels[0]->Category()) == "netadapter";
        if (create == ENOTSUP && !writer && !netadapter) return error::OK;
        *handled = true;
        if (create || !writer)
            return reject(create == EBUSY ? "PACKET_SINK_IN_USE" : "PACKET_SINK_UNAVAILABLE", create ? create : EIO);
        if (!netadapter || stmt.sources.size() != 1 || resolved.block_channels.size() != 1)
            return reject("PACKET_SINK_SOURCE_UNSUPPORTED");
        if (!stmt.columns.empty() || !stmt.where_clause.empty() || !stmt.stage_filters.empty() || stmt.HasOperator() ||
            !stmt.with_params.empty())
            return reject("PACKET_SINK_SQL_UNSUPPORTED");
        if (!control) return reject("ASYNC_EXECUTION_REQUIRED");
        struct Wake {
            std::shared_ptr<BlockTransformRunControl> control;
            ~Wake() { control->BindWake({}); }
        } wake{control};
        control->BindWake([control, p = writer.get()] {
            if (control->cancel_requested) p->Cancel();
        });
        if (control->cancel_requested) {
            writer->Cancel();
            return finish(0, "cancelled");
        }
        std::string detail;
        int rc = CreateExclusiveBlockReader(querier_, NextStreamTaskId(), resolved.block_channels[0].get(),
                                            kEmptyCanonicalFilterPlanV1, &reader, &detail);
        if (rc || !reader) {
            fallback.code = "PACKET_CAPTURE_READER_UNAVAILABLE";
            fallback.stage = "sink_open";
            return finish(rc ? rc : EIO, "failed");
        }
        capture = dynamic_cast<ICaptureBlockStreamReaderV2*>(reader.get());
        auto* schema = dynamic_cast<IBlockStreamSchemaV1*>(reader.get());
        if (!capture || !schema || capture->DescribeSources(&sources) ||
            ValidateCaptureSourceSetV2(sources) != CaptureDescriptionErrorV1::kNone) {
            fallback.code = "PACKET_CAPTURE_CONTRACT_REQUIRED";
            fallback.stage = "sink_open";
            reader->Cancel();
            return finish(EINVAL, "failed");
        }
        control->BindWake([control, p = writer.get(), source = reader.get()] {
            if (control->cancel_requested) {
                p->Cancel();
                source->Cancel();
            } else
                source->Flush();
        });
        rc = writer->Open(sources, schema->ArrowSchema());
        while (!rc) {
            if (control->cancel_requested) {
                writer->Cancel();
                break;
            }
            if (control->stop_requested) {
                stopped = true;
                break;
            }
            auto event = capture->PollCapture(sources.limits.max_wait_ms);
            if (event.block.kind == BlockPollEvent::kEof) {
                stopped = control->stop_requested;
                break;
            }
            if (event.block.kind == BlockPollEvent::kCancelled &&
                (control->stop_requested || control->cancel_requested)) {
                stopped = control->stop_requested;
                break;
            }
            if (event.block.kind != BlockPollEvent::kData && event.block.kind != BlockPollEvent::kTimeout) {
                source_failed = true;
                rc = EIO;
                break;
            }
            if (event.block.kind == BlockPollEvent::kTimeout && event.progress.empty() && !event.block.batch) continue;
            const auto start = std::chrono::steady_clock::now();
            ++delivery_waits;
            // event (including its borrowed fact vector) stays alive and no further poll runs until Write returns.
            try {
                rc = writer->Write(event);
            } catch (...) {
                rc = EIO;
            }
            delivery_wait_ns +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            if (event.block.batch) {
                const int released = reader->ReleaseBlock(event.block.batch);
                if (!rc && released) {
                    rc = released;
                    source_failed = true;
                }
            }
            control->Publish(Encode(publication()));
        }
        if (control->cancel_requested) {
            writer->Cancel();
            reader->Cancel();
            return finish(0, "cancelled");
        }
        if (!rc) rc = writer->Finish();
        reader->Cancel();
        if (rc) {
            auto report = writer->Report();
            if (report.code == "OK") {
                fallback.code = "PACKET_SINK_EXECUTION_FAILED";
                fallback.stage = "sink_deliver";
            }
            writer->Cancel();
        }
        return finish(rc, rc ? "failed" : stopped ? "stopped" : "completed");
    } catch (...) {
        if (writer) writer->Cancel();
        if (reader) reader->Cancel();
        return reject("PACKET_SINK_EXECUTION_FAILED", EIO);
    }
}
}  // namespace flowsql::scheduler
