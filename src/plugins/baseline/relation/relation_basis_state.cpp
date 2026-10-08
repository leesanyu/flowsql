// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "relation_basis_state.h"
#include "plugins/baseline/model/observation_validation.h"

#include <common/error_code.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>
#include <vector>

namespace flowsql {
namespace baseline {

namespace {

std::size_t NormalizeMaxGroups(std::size_t value) {
    return value == 0 ? static_cast<std::size_t>(256) : value;
}

double NormalizeMargin(double value) {
    if (!std::isfinite(value) || value < 1.0) return 1.0;
    return value;
}

struct ConservativeCandidate {
    RelationGroupHistoryStat stat;
    double hist_share_lower = 0.0;
    double active_ratio = 0.0;
    int64_t last_seen_bucket = 0;
};

bool CandidateOrder(const ConservativeCandidate& lhs, const ConservativeCandidate& rhs) {
    if (lhs.hist_share_lower != rhs.hist_share_lower) {
        return lhs.hist_share_lower > rhs.hist_share_lower;
    }
    if (lhs.active_ratio != rhs.active_ratio) return lhs.active_ratio > rhs.active_ratio;
    if (lhs.last_seen_bucket != rhs.last_seen_bucket) {
        return lhs.last_seen_bucket > rhs.last_seen_bucket;
    }
    return lhs.stat.group_idx < rhs.stat.group_idx;
}

uint32_t ReplacementCap(std::size_t old_size,
                        std::size_t new_size,
                        double cap_ratio,
                        uint32_t cap_max) {
    const std::size_t base_size = std::max<std::size_t>(1, std::max(old_size, new_size));
    const uint32_t ratio_cap =
        static_cast<uint32_t>(std::ceil(std::max(0.0, cap_ratio) * base_size));
    const uint32_t bounded_ratio_cap = std::max<uint32_t>(1, ratio_cap);
    if (cap_max == 0) return bounded_ratio_cap;
    return std::min<uint32_t>(cap_max, bounded_ratio_cap);
}

uint32_t ReplacementCount(const std::vector<uint32_t>& old_groups,
                          const std::vector<uint32_t>& new_groups) {
    uint32_t added = 0;
    uint32_t removed = 0;
    for (uint32_t group : new_groups) {
        if (std::find(old_groups.begin(), old_groups.end(), group) == old_groups.end()) {
            ++added;
        }
    }
    for (uint32_t group : old_groups) {
        if (std::find(new_groups.begin(), new_groups.end(), group) == new_groups.end()) {
            ++removed;
        }
    }
    return std::max(added, removed);
}

bool SameUintVector(const std::vector<uint32_t>& lhs, const std::vector<uint32_t>& rhs) {
    return lhs == rhs;
}

template <typename BlockLike>
BaselineStatus ObserveImpl(RelationStreamBasisAccumulator* accumulator,
                           const BlockLike& block,
                           std::size_t metric_index) {
    if (!accumulator) return BaselineStatus::kInvalidArgument;
    if (metric_index >= block.metrics.size()) return BaselineStatus::kInvalidArgument;
    const RelationBootstrapMetric& metric = block.metrics[metric_index];
    if (!IsValidRelationMetric(metric, block.group_idx.size())) {
        return BaselineStatus::kInvalidArgument;
    }
    return BaselineStatus::kOk;
}

template <typename BlockLike>
BaselineStatus RuntimeObserveImpl(RelationBasisRuntimeState* runtime,
                                  const BlockLike& block,
                                  std::size_t metric_index) {
    if (!runtime) return BaselineStatus::kInvalidArgument;
    return runtime->Observe(block, metric_index);
}

}  // namespace

RelationStreamBasisAccumulator::RelationStreamBasisAccumulator(
    RelationStreamBasisConfig config)
    : config_(std::move(config)) {
    config_.max_groups = NormalizeMaxGroups(config_.max_groups);
    config_.threshold_margin = NormalizeMargin(config_.threshold_margin);
}

void RelationStreamBasisAccumulator::ObserveMass(uint32_t group_idx,
                                                 double mass,
                                                 int64_t bucket_id) {
    if (!(mass > 0.0)) return;
    auto it = groups_.find(group_idx);
    if (it != groups_.end()) {
        it->second.estimated_mass += mass;
        it->second.active_bucket_count += 1;
        it->second.last_seen_bucket = bucket_id;
        return;
    }

    if (groups_.size() < config_.max_groups) {
        RelationStreamGroupEstimate estimate;
        estimate.group_idx = group_idx;
        estimate.estimated_mass = mass;
        estimate.active_bucket_count = 1;
        estimate.last_seen_bucket = bucket_id;
        groups_.emplace(group_idx, estimate);
        return;
    }

    auto min_it = std::min_element(
        groups_.begin(),
        groups_.end(),
        [](const auto& lhs, const auto& rhs) {
            if (lhs.second.estimated_mass != rhs.second.estimated_mass) {
                return lhs.second.estimated_mass < rhs.second.estimated_mass;
            }
            return lhs.first > rhs.first;
        });
    if (min_it == groups_.end()) return;

    const double replacement_error = min_it->second.estimated_mass;
    groups_.erase(min_it);

    RelationStreamGroupEstimate estimate;
    estimate.group_idx = group_idx;
    estimate.estimated_mass = replacement_error + mass;
    estimate.mass_error_upper_bound = replacement_error;
    estimate.active_bucket_count = 1;
    estimate.last_seen_bucket = bucket_id;
    groups_.emplace(group_idx, estimate);
}

BaselineStatus RelationStreamBasisAccumulator::Observe(const RelationRollingObservation& obs,
                                                       std::size_t metric_index) {
    if (valid_bucket_count_ > 0 && obs.bucket_id <= last_bucket_id_) return BaselineStatus::kInvalidArgument;
    const BaselineStatus validation = ObserveImpl(this, obs, metric_index);
    if (validation != BaselineStatus::kOk) return validation;
    return ObserveValidated(obs, metric_index);
}

BaselineStatus RelationStreamBasisAccumulator::ObserveValidated(const RelationRollingObservation& obs,
                                                                std::size_t metric_index) {
    if (valid_bucket_count_ > 0 && obs.bucket_id <= last_bucket_id_) return BaselineStatus::kInvalidArgument;

    const RelationBootstrapMetric& metric = obs.metrics[metric_index];
    if (valid_bucket_count_ == 0) first_bucket_id_ = obs.bucket_id;
    last_bucket_id_ = obs.bucket_id;
    ++valid_bucket_count_;
    total_mass_ += metric.total;
    for (std::size_t pos = 0; pos < obs.group_idx.size(); ++pos) {
        ObserveMass(obs.group_idx[pos], metric.values_by_group[pos], obs.bucket_id);
    }
    return BaselineStatus::kOk;
}

BaselineStatus RelationStreamBasisAccumulator::Observe(const RelationBootstrapBlock& block,
                                                       std::size_t metric_index) {
    if (valid_bucket_count_ > 0 && block.bucket_id <= last_bucket_id_) return BaselineStatus::kInvalidArgument;
    const BaselineStatus validation = ObserveImpl(this, block, metric_index);
    if (validation != BaselineStatus::kOk) return validation;

    const RelationBootstrapMetric& metric = block.metrics[metric_index];
    if (valid_bucket_count_ == 0) first_bucket_id_ = block.bucket_id;
    last_bucket_id_ = block.bucket_id;
    ++valid_bucket_count_;
    total_mass_ += metric.total;
    for (std::size_t pos = 0; pos < block.group_idx.size(); ++pos) {
        ObserveMass(block.group_idx[pos], metric.values_by_group[pos], block.bucket_id);
    }
    return BaselineStatus::kOk;
}

double RelationStreamBasisAccumulator::coverage_ratio() const {
    if (valid_bucket_count_ == 0 || last_bucket_id_ < first_bucket_id_) return 0.0;
    const uint64_t span =
        static_cast<uint64_t>(last_bucket_id_ - first_bucket_id_ + 1);
    return span == 0 ? 0.0 : static_cast<double>(valid_bucket_count_) /
                               static_cast<double>(span);
}

BaselineStatus RelationStreamBasisAccumulator::BuildConservativeInput(
    const RelationBasisBuildInput& base_input,
    RelationBasisBuildInput* out_input) const {
    if (!out_input || valid_bucket_count_ == 0 || total_mass_ <= 0.0) {
        return BaselineStatus::kInsufficientData;
    }

    std::vector<ConservativeCandidate> candidates;
    candidates.reserve(groups_.size());
    const double hist_threshold =
        base_input.support_policy.min_hist_share * config_.threshold_margin;
    const double active_threshold =
        base_input.support_policy.min_active_ratio * config_.threshold_margin;

    for (const auto& entry : groups_) {
        const RelationStreamGroupEstimate& estimate = entry.second;
        const double mass_lower =
            std::max(0.0, estimate.estimated_mass - estimate.mass_error_upper_bound);
        const double hist_share_lower = mass_lower / total_mass_;
        const double active_ratio =
            static_cast<double>(estimate.active_bucket_count) /
            static_cast<double>(valid_bucket_count_);
        if (hist_share_lower < hist_threshold || active_ratio < active_threshold) {
            continue;
        }

        ConservativeCandidate candidate;
        candidate.stat.group_idx = estimate.group_idx;
        candidate.stat.hist_mass = mass_lower;
        candidate.stat.active_bucket_count = estimate.active_bucket_count;
        candidate.hist_share_lower = hist_share_lower;
        candidate.active_ratio = active_ratio;
        candidate.last_seen_bucket = estimate.last_seen_bucket;
        candidates.push_back(std::move(candidate));
    }

    std::sort(candidates.begin(), candidates.end(), CandidateOrder);

    RelationBasisBuildInput result = base_input;
    result.valid_bucket_count = valid_bucket_count_;
    result.total_hist_mass_denominator = total_mass_;
    result.group_stats.clear();
    result.group_stats.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        result.group_stats.push_back(candidate.stat);
    }
    *out_input = std::move(result);
    return BaselineStatus::kOk;
}

RelationBasisRuntimeState::RelationBasisRuntimeState(RelationBasisRuntimeConfig config)
    : config_(std::move(config)), accumulator_(config_.stream) {}

BaselineStatus RelationBasisRuntimeState::Observe(const RelationRollingObservation& obs,
                                                  std::size_t metric_index) {
    const BaselineStatus status = accumulator_.Observe(obs, metric_index);
    if (status == BaselineStatus::kOk && !has_active_basis_) {
        basis_status_ = RelationBasisStatus::kCollecting;
    }
    return status;
}

BaselineStatus RelationBasisRuntimeState::Observe(const RelationBootstrapBlock& block,
                                                  std::size_t metric_index) {
    const BaselineStatus status = accumulator_.Observe(block, metric_index);
    if (status == BaselineStatus::kOk && !has_active_basis_) {
        basis_status_ = RelationBasisStatus::kCollecting;
    }
    return status;
}

BaselineStatus RelationBasisRuntimeState::ObserveValidated(const RelationRollingObservation& obs,
                                                           std::size_t metric_index) {
    const BaselineStatus status = accumulator_.ObserveValidated(obs, metric_index);
    if (status == BaselineStatus::kOk && !has_active_basis_) {
        basis_status_ = RelationBasisStatus::kCollecting;
    }
    return status;
}

BaselineStatus RelationBasisRuntimeState::LoadSeedBasis(RelationServiceBasis basis,
                                                        RelationBasisStatus status) {
    if (basis.metric_name.empty() || basis.group_space_id.empty() || basis.basis_version == 0) {
        return BaselineStatus::kInvalidArgument;
    }
    active_basis_ = std::move(basis);
    has_active_basis_ = true;
    basis_status_ = status;
    if (basis_status_ == RelationBasisStatus::kNoBasis ||
        basis_status_ == RelationBasisStatus::kCollecting) {
        basis_status_ = RelationBasisStatus::kBasisWarming;
    }
    return BaselineStatus::kOk;
}

RelationBasisRefreshDecision RelationBasisRuntimeState::CompleteHandoverIfReady(
    int64_t bucket_id) {
    RelationBasisRefreshDecision decision;
    decision.basis_status = basis_status_;
    decision.basis_version = has_active_basis_ ? active_basis_.basis_version : 0;
    if (basis_status_ == RelationBasisStatus::kHandoverWarming &&
        bucket_id >= handover_start_bucket_ &&
        static_cast<uint64_t>(bucket_id - handover_start_bucket_) >=
            config_.handover_warmup_buckets) {
        basis_status_ = RelationBasisStatus::kBasisReady;
        decision.basis_status = basis_status_;
        decision.basis_version = active_basis_.basis_version;
    }
    return decision;
}

bool RelationBasisRuntimeState::IsRefreshDue(int64_t bucket_id) const {
    if (last_refresh_bucket_ == 0) return true;
    if (bucket_id < last_refresh_bucket_) return false;
    return static_cast<uint64_t>(bucket_id - last_refresh_bucket_) >=
           config_.refresh_interval_buckets;
}

uint64_t RelationBasisRuntimeState::NextBasisVersion() const {
    return has_active_basis_ ? active_basis_.basis_version + 1 : 1;
}

bool RelationBasisRuntimeState::SameBasis(const RelationServiceBasis& candidate) const {
    return has_active_basis_ &&
           SameUintVector(active_basis_.support_explicit, candidate.support_explicit) &&
           SameUintVector(active_basis_.stable_head, candidate.stable_head);
}

bool RelationBasisRuntimeState::IsWithinReplacementCap(
    const RelationServiceBasis& candidate) const {
    if (!has_active_basis_) return true;
    const uint32_t support_cap = ReplacementCap(active_basis_.support_explicit.size(),
                                                candidate.support_explicit.size(),
                                                config_.replacement_cap_ratio,
                                                config_.replacement_cap_max);
    const uint32_t stable_cap = ReplacementCap(active_basis_.stable_head.size(),
                                               candidate.stable_head.size(),
                                               config_.replacement_cap_ratio,
                                               config_.replacement_cap_max);
    return ReplacementCount(active_basis_.support_explicit, candidate.support_explicit) <=
               support_cap &&
           ReplacementCount(active_basis_.stable_head, candidate.stable_head) <= stable_cap;
}

void RelationBasisRuntimeState::ApplyStableRefreshGate(RelationBasisBuildInput* input) {
    if (!input || config_.min_stable_refresh_count <= 1) return;

    std::unordered_set<uint32_t> current_groups;
    current_groups.reserve(input->group_stats.size());
    for (const auto& stat : input->group_stats) {
        current_groups.insert(stat.group_idx);
        stable_refresh_counts_[stat.group_idx] += 1;
    }
    for (auto it = stable_refresh_counts_.begin(); it != stable_refresh_counts_.end();) {
        if (current_groups.find(it->first) == current_groups.end()) {
            it = stable_refresh_counts_.erase(it);
        } else {
            ++it;
        }
    }

    std::vector<RelationGroupHistoryStat> stable_stats;
    stable_stats.reserve(input->group_stats.size());
    for (const auto& stat : input->group_stats) {
        const auto count_it = stable_refresh_counts_.find(stat.group_idx);
        if (count_it != stable_refresh_counts_.end() &&
            count_it->second >= config_.min_stable_refresh_count) {
            stable_stats.push_back(stat);
        }
    }
    input->group_stats = std::move(stable_stats);
}

RelationBasisRefreshDecision RelationBasisRuntimeState::MaybeRefresh(
    const RelationBasisBuildInput& base_input,
    int64_t bucket_id) {
    RelationBasisRefreshDecision decision = CompleteHandoverIfReady(bucket_id);
    if (decision.status != BaselineStatus::kOk) return decision;

    decision.basis_status = basis_status_;
    decision.basis_version = has_active_basis_ ? active_basis_.basis_version : 0;

    if (accumulator_.valid_bucket_count() < config_.collect_min_buckets) {
        if (!has_active_basis_) basis_status_ = RelationBasisStatus::kCollecting;
        decision.basis_status = basis_status_;
        return decision;
    }
    if (!IsRefreshDue(bucket_id)) return decision;
    if (accumulator_.coverage_ratio() < config_.candidate_min_coverage_ratio) {
        return decision;
    }

    RelationBasisBuildInput build_input = base_input;
    build_input.basis_version = NextBasisVersion();
    const BaselineStatus input_status =
        accumulator_.BuildConservativeInput(build_input, &build_input);
    if (input_status != BaselineStatus::kOk) {
        decision.status = input_status;
        return decision;
    }
    ApplyStableRefreshGate(&build_input);

    decision.evaluated = true;
    last_refresh_bucket_ = bucket_id;
    if (config_.min_stable_refresh_count > 1 && build_input.group_stats.empty()) {
        decision.basis_status = basis_status_;
        decision.basis_version = has_active_basis_ ? active_basis_.basis_version : 0;
        return decision;
    }

    RelationServiceBasis candidate;
    if (RelationBasisBuilder::BuildServiceBasis(build_input, &candidate) != error::OK) {
        decision.status = BaselineStatus::kTrainFailed;
        return decision;
    }

    if (!has_active_basis_) {
        active_basis_ = std::move(candidate);
        has_active_basis_ = true;
        basis_status_ = accumulator_.valid_bucket_count() >= config_.ready_min_buckets
                            ? RelationBasisStatus::kBasisReady
                            : RelationBasisStatus::kBasisWarming;
        decision.basis_updated = true;
        decision.basis_status = basis_status_;
        decision.basis_version = active_basis_.basis_version;
        return decision;
    }

    if (SameBasis(candidate)) {
        if (basis_status_ == RelationBasisStatus::kBasisWarming &&
            accumulator_.valid_bucket_count() >= config_.ready_min_buckets) {
            basis_status_ = RelationBasisStatus::kBasisReady;
        }
        decision.basis_status = basis_status_;
        decision.basis_version = active_basis_.basis_version;
        return decision;
    }

    if (!IsWithinReplacementCap(candidate)) {
        decision.rejected_by_replacement_cap = true;
        decision.basis_status = basis_status_;
        decision.basis_version = active_basis_.basis_version;
        return decision;
    }

    active_basis_ = std::move(candidate);
    basis_status_ = RelationBasisStatus::kHandoverWarming;
    handover_start_bucket_ = bucket_id;
    decision.basis_updated = true;
    decision.handover_started = true;
    decision.basis_status = basis_status_;
    decision.basis_version = active_basis_.basis_version;
    return decision;
}

}  // namespace baseline
}  // namespace flowsql
