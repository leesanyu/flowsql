// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "relation_summary.h"
#include "plugins/baseline/model/observation_validation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace flowsql {
namespace baseline {

namespace {

double Clamp01(double value) {
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

void InsertTopMass(double mass, std::size_t limit, std::vector<double>* top_masses) {
    if (!top_masses || limit == 0) return;
    if (top_masses->size() < limit) {
        top_masses->push_back(mass);
        return;
    }

    auto min_it = std::min_element(top_masses->begin(), top_masses->end());
    if (min_it != top_masses->end() && mass > *min_it) {
        *min_it = mass;
    }
}

RelationProjectedSummary MakeValueSummary(const std::string& metric_name,
                                          const std::string& summary_name,
                                          double value,
                                          bool basis_scoped,
                                          uint64_t basis_version,
                                          bool active_count_from_upstream = false) {
    RelationProjectedSummary summary;
    summary.metric_name = metric_name;
    summary.summary_name = summary_name;
    summary.task_kind = BaselineTaskKind::kValue;
    summary.feature_type = "value_basic";
    summary.basis_scoped = basis_scoped;
    summary.basis_version = basis_version;
    summary.active_count_from_upstream = active_count_from_upstream;
    summary.value = value;
    return summary;
}

RelationProjectedSummary MakeRatioSummary(const std::string& metric_name,
                                          const std::string& summary_name,
                                          double share,
                                          double denominator,
                                          bool basis_scoped,
                                          uint64_t basis_version) {
    RelationProjectedSummary summary;
    summary.metric_name = metric_name;
    summary.summary_name = summary_name;
    summary.task_kind = BaselineTaskKind::kRatio;
    summary.feature_type = "ratio";
    summary.basis_scoped = basis_scoped;
    summary.basis_version = basis_version;
    summary.denominator = denominator;
    summary.numerator = Clamp01(share) * denominator;
    return summary;
}

template <typename BlockLike>
bool ProjectRelationMetricSummariesImpl(
    const BlockLike& block,
    std::size_t metric_index,
    const std::string& requested_metric_name,
    const RelationSummaryProjectionOptions& options,
    std::vector<RelationProjectedSummary>* out_summaries) {
    if (!out_summaries) return false;
    out_summaries->clear();
    if (metric_index >= block.metrics.size()) return false;

    const RelationBootstrapMetric& metric = block.metrics[metric_index];
    if (!IsValidRelationMetricHeader(metric, block.group_idx.size(), requested_metric_name)) {
        return false;
    }

    const std::string metric_name =
        requested_metric_name.empty() ? metric.metric : requested_metric_name;
    const std::unordered_set<uint32_t> common_other_group_set(
        options.other_group_idxs.begin(), options.other_group_idxs.end());

    double entropy_shannon = 0.0;
    double top1_mass = 0.0;
    std::vector<double> top_masses;
    top_masses.reserve(static_cast<std::size_t>(std::max(0, options.summary_policy.k_head)));
    uint32_t active_count = 0;

    for (std::size_t group_pos = 0; group_pos < block.group_idx.size(); ++group_pos) {
        const double mass = metric.values_by_group[group_pos];
        if (!IsValidRelationMass(mass)) return false;
        if (!(mass > 0.0)) continue;
        ++active_count;

        const uint32_t group_idx = block.group_idx[group_pos];
        const double share = mass / metric.total;
        entropy_shannon -= share * std::log(share);

        if (common_other_group_set.find(group_idx) == common_other_group_set.end()) {
            if (mass > top1_mass) top1_mass = mass;
            InsertTopMass(mass,
                          static_cast<std::size_t>(std::max(0, options.summary_policy.k_head)),
                          &top_masses);
        }
    }

    out_summaries->push_back(MakeValueSummary(
        metric_name, "entropy_shannon", entropy_shannon, false, 0));

    const uint32_t distinct_count = metric.active_count > 0 ? metric.active_count : active_count;
    if (distinct_count > 0) {
        out_summaries->push_back(MakeValueSummary(metric_name,
                                                  "distinct_group_count",
                                                  static_cast<double>(distinct_count),
                                                  false,
                                                  0,
                                                  metric.active_count > 0));
    }

    out_summaries->push_back(MakeRatioSummary(
        metric_name, "top1_share", top1_mass / metric.total, metric.total, false, 0));

    double head_mass = 0.0;
    for (double mass : top_masses) head_mass += mass;
    out_summaries->push_back(MakeRatioSummary(
        metric_name, "headk_share", head_mass / metric.total, metric.total, false, 0));

    const RelationServiceBasis* basis = options.basis;
    if (!options.include_basis_scoped || !basis) return true;

    const std::unordered_set<uint32_t> basis_other_group_set(
        basis->other_group_idxs.begin(), basis->other_group_idxs.end());
    std::unordered_set<uint32_t> support_set(
        basis->support_explicit.begin(), basis->support_explicit.end());
    for (uint32_t group_idx : basis->other_group_idxs) {
        support_set.erase(group_idx);
    }

    std::unordered_map<uint32_t, std::size_t> stable_index_by_group;
    stable_index_by_group.reserve(basis->stable_head.size());
    std::vector<double> stable_g_shares(basis->stable_head.size(), 0.0);
    for (std::size_t i = 0; i < basis->stable_head.size(); ++i) {
        stable_index_by_group.emplace(basis->stable_head[i], i);
    }

    double support_mass = 0.0;
    for (std::size_t group_pos = 0; group_pos < block.group_idx.size(); ++group_pos) {
        const double mass = metric.values_by_group[group_pos];
        if (!(mass > 0.0)) continue;
        const uint32_t group_idx = block.group_idx[group_pos];
        const bool is_other = basis_other_group_set.find(group_idx) != basis_other_group_set.end();
        if (!is_other && support_set.find(group_idx) != support_set.end()) {
            support_mass += mass;
        }
        if (!is_other) {
            auto stable_it = stable_index_by_group.find(group_idx);
            if (stable_it != stable_index_by_group.end()) {
                stable_g_shares[stable_it->second] = mass / metric.total;
            }
        }
    }

    const uint64_t basis_version = basis->basis_version;
    out_summaries->push_back(MakeRatioSummary(metric_name,
                                              "out_of_support_share",
                                              std::max(0.0, metric.total - support_mass) /
                                                  metric.total,
                                              metric.total,
                                              true,
                                              basis_version));

    double stable_headk_coverage = 0.0;
    for (double share : stable_g_shares) stable_headk_coverage += share;
    out_summaries->push_back(MakeRatioSummary(metric_name,
                                              "stable_headk_coverage",
                                              stable_headk_coverage,
                                              metric.total,
                                              true,
                                              basis_version));

    for (std::size_t stable_index = 0; stable_index < stable_g_shares.size(); ++stable_index) {
        out_summaries->push_back(MakeRatioSummary(metric_name,
                                                  "stable_g_share_" +
                                                      std::to_string(stable_index),
                                                  stable_g_shares[stable_index],
                                                  metric.total,
                                                  true,
                                                  basis_version));
    }

    if (basis->stable_head.size() >= 2 &&
        basis->head_proto_q.size() == basis->stable_head.size() &&
        stable_headk_coverage > 0.0) {
        double total_variation = 0.0;
        for (std::size_t i = 0; i < basis->stable_head.size(); ++i) {
            const double current_mix = stable_g_shares[i] / stable_headk_coverage;
            total_variation += std::fabs(current_mix - basis->head_proto_q[i]);
        }
        out_summaries->push_back(MakeValueSummary(metric_name,
                                                  "stable_headk_mix_drift",
                                                  0.5 * total_variation,
                                                  true,
                                                  basis_version));
    }

    return true;
}

}  // namespace

bool ProjectRelationMetricSummaries(
    const RelationBootstrapBlock& block,
    std::size_t metric_index,
    const std::string& metric_name,
    const RelationSummaryProjectionOptions& options,
    std::vector<RelationProjectedSummary>* out_summaries) {
    return ProjectRelationMetricSummariesImpl(
        block, metric_index, metric_name, options, out_summaries);
}

bool ProjectRelationMetricSummaries(
    const RelationRollingObservation& obs,
    std::size_t metric_index,
    const std::string& metric_name,
    const RelationSummaryProjectionOptions& options,
    std::vector<RelationProjectedSummary>* out_summaries) {
    return ProjectRelationMetricSummariesImpl(
        obs, metric_index, metric_name, options, out_summaries);
}

}  // namespace baseline
}  // namespace flowsql
