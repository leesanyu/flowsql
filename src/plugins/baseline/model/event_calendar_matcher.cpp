// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "plugins/baseline/model/event_calendar_matcher.h"

#include <common/error_code.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <unordered_map>

#include <unicode/basictz.h>
#include <unicode/timezone.h>
#include <unicode/tzrule.h>
#include <unicode/tztrans.h>

#include "plugins/baseline/model/calendar_feature_helper.h"

namespace flowsql {
namespace baseline {

namespace {

using WideTime = __int128;

WideTime FloorDiv(WideTime value, int64_t divisor) {
    const WideTime quotient = value / divisor;
    return quotient - (value % divisor < 0 ? 1 : 0);
}

WideTime CeilDiv(WideTime value, int64_t divisor) { return -FloorDiv(-value, divisor); }

struct ZoneRules {
    std::unique_ptr<icu::TimeZone> zone;
    const icu::BasicTimeZone* basic = nullptr;
    int64_t min_offset = 0;
    int64_t max_offset = 0;
};

int64_t RuleOffset(const icu::TimeZoneRule& rule) {
    return (static_cast<int64_t>(rule.getRawOffset()) + rule.getDSTSavings()) / 1000;
}

bool PrepareZone(const std::string& name, ZoneRules* out) {
    out->zone.reset(icu::TimeZone::createTimeZone(icu::UnicodeString::fromUTF8(name.empty() ? "UTC" : name)));
    out->basic = dynamic_cast<const icu::BasicTimeZone*>(out->zone.get());
    if (!out->basic) return false;
    UErrorCode status = U_ZERO_ERROR;
    int32_t count = out->basic->countTransitionRules(status);
    if (U_FAILURE(status)) return false;
    std::vector<const icu::TimeZoneRule*> rules(static_cast<std::size_t>(count));
    const icu::InitialTimeZoneRule* initial = nullptr;
    out->basic->getTimeZoneRules(initial, rules.data(), count, status);
    if (U_FAILURE(status) || !initial) return false;
    const auto include = [&](const icu::TimeZoneRule& rule) {
        const int64_t offset = RuleOffset(rule);
        out->min_offset = std::min(out->min_offset, offset);
        out->max_offset = std::max(out->max_offset, offset);
    };
    include(*initial);
    for (int32_t i = 0; i < count; ++i) include(*rules[static_cast<std::size_t>(i)]);
    return true;
}

int64_t OffsetAt(const ZoneRules& rules, int64_t utc_second) {
    UErrorCode status = U_ZERO_ERROR;
    int32_t raw = 0;
    int32_t dst = 0;
    rules.zone->getOffset(static_cast<UDate>(utc_second) * 1000.0, false, raw, dst, status);
    return U_FAILURE(status) ? 0 : (static_cast<int64_t>(raw) + dst) / 1000;
}

void AddBucketRange(WideTime begin, WideTime end, int64_t delta, std::size_t code,
                    std::vector<CompiledEventBucketRange>* ranges) {
    // Restrict to buckets whose two UTC endpoints are representable, as required by the old matcher.
    begin = std::max(begin, CeilDiv(std::numeric_limits<int64_t>::min(), delta));
    end = std::min(end, FloorDiv(static_cast<WideTime>(std::numeric_limits<int64_t>::max()) - delta, delta) + 1);
    if (begin < end) {
        ranges->push_back({static_cast<int64_t>(begin), static_cast<int64_t>(end), 0, code});
    }
}

void CompileLocalBucketRanges(const CompiledEventCalendarEntry& entry, const std::string& timezone,
                              const ZoneRules& rules, int64_t delta, std::vector<CompiledEventBucketRange>* ranges) {
    const WideTime local_begin = LocalWallClockSecond(entry.start_ts, timezone);
    const WideTime local_end = LocalWallClockSecond(entry.end_ts, timezone);
    const WideTime begin =
        std::max(FloorDiv(local_begin - rules.max_offset, delta), CeilDiv(std::numeric_limits<int64_t>::min(), delta));
    const WideTime end =
        std::min(CeilDiv(local_end - rules.min_offset, delta),
                 FloorDiv(static_cast<WideTime>(std::numeric_limits<int64_t>::max()) - delta, delta) + 1);
    if (begin >= end) return;
    std::vector<int64_t> boundaries{static_cast<int64_t>(begin), static_cast<int64_t>(end)};
    UDate cursor = static_cast<UDate>(begin * delta) * 1000.0;
    const UDate limit = static_cast<UDate>(end * delta) * 1000.0;
    icu::TimeZoneTransition transition;
    while (rules.basic->getNextTransition(cursor, false, transition) && transition.getTime() <= limit) {
        const WideTime transition_second = static_cast<int64_t>(transition.getTime() / 1000.0);
        const WideTime first_new_start = CeilDiv(transition_second, delta);
        // The bucket ending across a transition may have two different endpoint offsets.
        for (WideTime boundary : {first_new_start - 1, first_new_start}) {
            if (boundary > begin && boundary < end) boundaries.push_back(static_cast<int64_t>(boundary));
        }
        cursor = transition.getTime();
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    for (std::size_t i = 1; i < boundaries.size(); ++i) {
        const int64_t first = boundaries[i - 1];
        const int64_t start_offset = OffsetAt(rules, static_cast<int64_t>(static_cast<WideTime>(first) * delta));
        const int64_t end_offset = OffsetAt(rules, static_cast<int64_t>((static_cast<WideTime>(first) + 1) * delta));
        // Original predicate: k*delta+start_offset < local_end && local_begin < (k+1)*delta+end_offset.
        AddBucketRange(std::max<WideTime>(first, FloorDiv(local_begin - end_offset, delta)),
                       std::min<WideTime>(boundaries[i], CeilDiv(local_end - start_offset, delta)), delta,
                       entry.event_code_index, ranges);
    }
}

int64_t BuildBucketRangeIndex(std::vector<CompiledEventBucketRange>* ranges, std::size_t begin, std::size_t end) {
    const std::size_t mid = begin + (end - begin) / 2;
    auto& node = (*ranges)[mid];
    node.subtree_end = node.end;
    if (begin < mid) node.subtree_end = std::max(node.subtree_end, BuildBucketRangeIndex(ranges, begin, mid));
    if (mid + 1 < end) node.subtree_end = std::max(node.subtree_end, BuildBucketRangeIndex(ranges, mid + 1, end));
    return node.subtree_end;
}

template <typename Hit>
void VisitBucketRanges(const std::vector<CompiledEventBucketRange>& ranges, std::size_t begin, std::size_t end,
                       int64_t bucket_id, const Hit& hit) {
    if (begin == end) return;
    const std::size_t mid = begin + (end - begin) / 2;
    const auto& node = ranges[mid];
    if (node.subtree_end <= bucket_id) return;
    VisitBucketRanges(ranges, begin, mid, bucket_id, hit);
    if (node.begin > bucket_id) return;
    if (bucket_id < node.end) hit(node.event_code_index);
    VisitBucketRanges(ranges, mid + 1, end, bucket_id, hit);
}

bool IsAllowedAlignment(const std::string& alignment_mode) {
    return alignment_mode == "absolute_utc" || alignment_mode == "local_wall_clock";
}

bool IntervalsOverlap(int64_t lhs_start, int64_t lhs_end, int64_t rhs_start, int64_t rhs_end) {
    return lhs_start < rhs_end && rhs_start < lhs_end;
}

bool EntryOverlapsBucket(const CompiledEventCalendarEntry& entry,
                         const BaselineTaskSpec& task_spec,
                         int64_t bucket_id) {
    const int64_t bucket_start_utc = bucket_id * task_spec.delta;
    const int64_t bucket_end_utc = bucket_start_utc + task_spec.delta;
    if (entry.alignment_mode == "absolute_utc") {
        return IntervalsOverlap(bucket_start_utc, bucket_end_utc, entry.start_ts, entry.end_ts);
    }

    const std::string& event_tz = entry.tz.empty() ? task_spec.tz : entry.tz;
    const int64_t bucket_start_local = LocalWallClockSecond(bucket_start_utc, event_tz);
    const int64_t bucket_end_local = LocalWallClockSecond(bucket_end_utc, event_tz);
    const int64_t event_start_local = LocalWallClockSecond(entry.start_ts, event_tz);
    const int64_t event_end_local = LocalWallClockSecond(entry.end_ts, event_tz);
    return IntervalsOverlap(bucket_start_local, bucket_end_local, event_start_local, event_end_local);
}

}  // namespace

int CompileEventCalendar(const EventCalendarSpec& spec, const BaselineTaskSpec& task_spec, CompiledEventCalendar* out,
                         std::string* err) {
    if (!out) return error::BAD_REQUEST;
    *out = CompiledEventCalendar{};
    if (spec.calendar_id.empty() || spec.calendar_version.empty()) {
        if (err) *err = "calendar_id and calendar_version must not be empty";
        return error::BAD_REQUEST;
    }

    out->calendar_id = spec.calendar_id;
    out->calendar_version = spec.calendar_version;
    std::unordered_map<std::string, std::size_t> code_index_by_name;

    for (const auto& entry : spec.entries) {
        if (!entry.enabled) continue;
        if (entry.event_code.empty()) {
            if (err) *err = "event_code must not be empty";
            return error::BAD_REQUEST;
        }
        if (!IsAllowedAlignment(entry.alignment_mode)) {
            if (err) *err = "alignment_mode is invalid";
            return error::BAD_REQUEST;
        }
        if (entry.end_ts <= entry.start_ts) {
            if (err) *err = "event interval must be positive";
            return error::BAD_REQUEST;
        }
        auto code_it = code_index_by_name.find(entry.event_code);
        if (code_it == code_index_by_name.end()) {
            const std::size_t next_index = out->enabled_event_codes.size();
            out->enabled_event_codes.push_back(entry.event_code);
            code_it = code_index_by_name.emplace(entry.event_code, next_index).first;
        }

        CompiledEventCalendarEntry compiled;
        compiled.event_code = entry.event_code;
        compiled.alignment_mode = entry.alignment_mode;
        compiled.start_ts = entry.start_ts;
        compiled.end_ts = entry.end_ts;
        compiled.tz = entry.tz;
        compiled.event_code_index = code_it->second;
        out->entries.push_back(std::move(compiled));
    }

    return task_spec.delta > 0 ? BindEventCalendar(*out, task_spec, out, err) : error::OK;
}

int BindEventCalendar(const CompiledEventCalendar& calendar, const BaselineTaskSpec& task_spec,
                      CompiledEventCalendar* out, std::string* err) {
    if (!out || task_spec.delta <= 0) {
        if (err) *err = "event calendar binding requires a positive task bucket length";
        return error::BAD_REQUEST;
    }
    *out = calendar;
    out->bound_bucket_seconds = 0;
    out->bucket_ranges.clear();
    std::unordered_map<std::string, ZoneRules> zones;
    for (const auto& entry : out->entries) {
        if (entry.alignment_mode == "absolute_utc") {
            AddBucketRange(FloorDiv(entry.start_ts, task_spec.delta), CeilDiv(entry.end_ts, task_spec.delta),
                           task_spec.delta, entry.event_code_index, &out->bucket_ranges);
            continue;
        }
        const std::string& timezone = entry.tz.empty() ? task_spec.tz : entry.tz;
        auto zone = zones.find(timezone);
        if (zone == zones.end()) {
            ZoneRules rules;
            if (!PrepareZone(timezone, &rules)) {
                if (err) *err = "event calendar timezone rules could not be compiled";
                return error::BAD_REQUEST;
            }
            zone = zones.emplace(timezone, std::move(rules)).first;
        }
        CompileLocalBucketRanges(entry, timezone, zone->second, task_spec.delta, &out->bucket_ranges);
    }
    auto& ranges = out->bucket_ranges;
    std::sort(ranges.begin(), ranges.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.event_code_index != rhs.event_code_index) return lhs.event_code_index < rhs.event_code_index;
        return lhs.begin < rhs.begin;
    });
    std::size_t size = 0;
    for (const auto range : ranges) {
        if (size > 0 && ranges[size - 1].event_code_index == range.event_code_index &&
            range.begin <= ranges[size - 1].end) {
            ranges[size - 1].end = std::max(ranges[size - 1].end, range.end);
        } else {
            ranges[size++] = range;
        }
    }
    ranges.resize(size);
    std::sort(ranges.begin(), ranges.end(), [](const auto& lhs, const auto& rhs) { return lhs.begin < rhs.begin; });
    if (!ranges.empty()) BuildBucketRangeIndex(&ranges, 0, ranges.size());
    out->bound_bucket_seconds = task_spec.delta;
    return error::OK;
}

std::vector<std::string> ResolveBucketEvents(const CompiledEventCalendar& calendar,
                                             const BaselineTaskSpec& task_spec,
                                             int64_t bucket_id) {
    if (calendar.bound_bucket_seconds > 0) {
        std::vector<std::size_t> codes;
        VisitBucketRanges(calendar.bucket_ranges, 0, calendar.bucket_ranges.size(), bucket_id,
                          [&](std::size_t code) { codes.push_back(code); });
        std::sort(codes.begin(), codes.end());
        codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
        std::vector<std::string> events;
        events.reserve(codes.size());
        for (std::size_t code : codes) events.push_back(calendar.enabled_event_codes[code]);
        return events;
    }
    std::vector<uint8_t> hit(calendar.enabled_event_codes.size(), 0);
    for (const auto& entry : calendar.entries) {
        if (!EntryOverlapsBucket(entry, task_spec, bucket_id)) continue;
        if (entry.event_code_index < hit.size()) {
            hit[entry.event_code_index] = 1;
        }
    }

    std::vector<std::string> events;
    for (std::size_t i = 0; i < hit.size(); ++i) {
        if (hit[i]) events.push_back(calendar.enabled_event_codes[i]);
    }
    return events;
}

int BuildEventIndicatorRow(const CompiledEventCalendar& calendar,
                           const BaselineTaskSpec& task_spec,
                           int64_t bucket_id,
                           double* out_row,
                           std::size_t row_size) {
    if (!out_row || row_size < calendar.enabled_event_codes.size()) return error::BAD_REQUEST;
    std::fill(out_row, out_row + row_size, 0.0);
    if (calendar.bound_bucket_seconds > 0) {
        VisitBucketRanges(calendar.bucket_ranges, 0, calendar.bucket_ranges.size(), bucket_id,
                          [&](std::size_t code) { out_row[code] = 1.0; });
        return error::OK;
    }
    for (const auto& entry : calendar.entries) {
        if (!EntryOverlapsBucket(entry, task_spec, bucket_id)) continue;
        if (entry.event_code_index < row_size) {
            out_row[entry.event_code_index] = 1.0;
        }
    }
    return error::OK;
}

}  // namespace baseline
}  // namespace flowsql
