// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_BASELINE_RELATION_RELATION_BASIS_STATE_H_
#define _FLOWSQL_PLUGINS_BASELINE_RELATION_RELATION_BASIS_STATE_H_

#include <framework/interfaces/ibaseline_types.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "plugins/baseline/relation/relation_basis.h"

namespace flowsql {
namespace baseline {

struct RelationStreamBasisConfig {
    std::size_t max_groups = 256;
    double threshold_margin = 1.20;
};

enum class RelationBasisStatus : int32_t {
    kNoBasis = 0,
    kCollecting = 1,
    kBasisWarming = 2,
    kBasisReady = 3,
    kHandoverWarming = 4,
};

struct RelationBasisRuntimeConfig {
    RelationStreamBasisConfig stream;
    uint64_t collect_min_buckets = 1440;
    uint64_t ready_min_buckets = 4320;
    uint64_t refresh_interval_buckets = 1440;
    double candidate_min_coverage_ratio = 0.60;
    double replacement_cap_ratio = 0.20;
    uint32_t replacement_cap_max = 2;
    uint64_t handover_warmup_buckets = 1440;
    uint64_t min_stable_refresh_count = 2;
};

struct RelationBasisRefreshDecision {
    BaselineStatus status = BaselineStatus::kOk;
    bool evaluated = false;
    bool basis_updated = false;
    bool handover_started = false;
    bool rejected_by_replacement_cap = false;
    RelationBasisStatus basis_status = RelationBasisStatus::kNoBasis;
    uint64_t basis_version = 0;
};

struct RelationStreamGroupEstimate {
    uint32_t group_idx = 0;
    double estimated_mass = 0.0;
    double mass_error_upper_bound = 0.0;
    uint64_t active_bucket_count = 0;
    int64_t last_seen_bucket = 0;
};

class RelationStreamBasisAccumulator {
 public:
    explicit RelationStreamBasisAccumulator(RelationStreamBasisConfig config);

    BaselineStatus Observe(const RelationRollingObservation& obs, std::size_t metric_index);
    // Caller must validate the complete metric before updating any runtime state.
    BaselineStatus ObserveValidated(const RelationRollingObservation& obs, std::size_t metric_index);
    BaselineStatus Observe(const RelationBootstrapBlock& block, std::size_t metric_index);

    BaselineStatus BuildConservativeInput(const RelationBasisBuildInput& base_input,
                                          RelationBasisBuildInput* out_input) const;

    uint64_t valid_bucket_count() const { return valid_bucket_count_; }
    double total_mass() const { return total_mass_; }
    double coverage_ratio() const;
    std::size_t group_count() const { return groups_.size(); }

 private:
    void ObserveMass(uint32_t group_idx, double mass, int64_t bucket_id);

    RelationStreamBasisConfig config_;
    uint64_t valid_bucket_count_ = 0;
    double total_mass_ = 0.0;
    int64_t first_bucket_id_ = 0;
    int64_t last_bucket_id_ = 0;
    std::unordered_map<uint32_t, RelationStreamGroupEstimate> groups_;
};

class RelationBasisRuntimeState {
 public:
    explicit RelationBasisRuntimeState(RelationBasisRuntimeConfig config);

    BaselineStatus Observe(const RelationRollingObservation& obs, std::size_t metric_index);
    BaselineStatus ObserveValidated(const RelationRollingObservation& obs, std::size_t metric_index);
    BaselineStatus Observe(const RelationBootstrapBlock& block, std::size_t metric_index);
    BaselineStatus LoadSeedBasis(RelationServiceBasis basis, RelationBasisStatus status);
    RelationBasisRefreshDecision MaybeRefresh(const RelationBasisBuildInput& base_input,
                                              int64_t bucket_id);

    RelationBasisStatus basis_status() const { return basis_status_; }
    const RelationServiceBasis* active_basis() const {
        return has_active_basis_ ? &active_basis_ : nullptr;
    }
    const RelationStreamBasisAccumulator& accumulator() const { return accumulator_; }

 private:
    RelationBasisRefreshDecision CompleteHandoverIfReady(int64_t bucket_id);
    bool IsRefreshDue(int64_t bucket_id) const;
    bool IsWithinReplacementCap(const RelationServiceBasis& candidate) const;
    bool SameBasis(const RelationServiceBasis& candidate) const;
    void ApplyStableRefreshGate(RelationBasisBuildInput* input);
    uint64_t NextBasisVersion() const;

    RelationBasisRuntimeConfig config_;
    RelationStreamBasisAccumulator accumulator_;
    RelationBasisStatus basis_status_ = RelationBasisStatus::kNoBasis;
    bool has_active_basis_ = false;
    RelationServiceBasis active_basis_;
    int64_t last_refresh_bucket_ = 0;
    int64_t handover_start_bucket_ = 0;
    std::unordered_map<uint32_t, uint64_t> stable_refresh_counts_;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_RELATION_RELATION_BASIS_STATE_H_
