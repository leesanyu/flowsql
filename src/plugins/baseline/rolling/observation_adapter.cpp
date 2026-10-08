// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "plugins/baseline/rolling/observation_adapter.h"

#include <algorithm>
#include <cmath>

#include "plugins/baseline/model/observation_validation.h"
#include "plugins/baseline/model/profile_config.h"

namespace flowsql {
namespace baseline {
namespace {

ObservedModelPoint InvalidPoint(const std::string& series_key,
                                int64_t bucket_id,
                                const char* diagnostics) {
    ObservedModelPoint point;
    point.status = BaselineStatus::kInvalidArgument;
    point.series_key = series_key;
    point.bucket_id = bucket_id;
    point.diagnostics = diagnostics ? diagnostics : "invalid rolling observation";
    return point;
}

double UnitWeight(double value, double ref) {
    if (ref <= 0.0) return 1.0;
    return std::min(1.0, value / ref);
}

void EnableWeightedUpdate(double weight, ObservedModelPoint* point) {
    point->can_score = true;
    point->can_update = true;
    point->score_weight = weight;
    point->update_weight = weight;
}

double SafeLogit(double probability, double eps_logit) {
    const double clipped = std::max(eps_logit, std::min(1.0 - eps_logit, probability));
    return std::log(clipped / (1.0 - clipped));
}

}  // namespace

ObservedModelPoint AdaptValueRollingObservation(const BaselineTaskSpec& spec,
                                                const BaselineRollingConfig& config,
                                                const ValueRollingObservation& obs) {
    if (obs.series_key.empty()) {
        return InvalidPoint(obs.series_key, obs.bucket_id, "series_key must not be empty");
    }
    if (!IsValidValueObservation(obs.value)) {
        return InvalidPoint(obs.series_key, obs.bucket_id, "value must be finite and non-negative");
    }
    if (spec.feature_type != "value_basic" && spec.feature_type != "value_sampled") {
        return InvalidPoint(obs.series_key, obs.bucket_id, "unsupported value feature_type");
    }

    ObservedModelPoint point;
    point.series_key = obs.series_key;
    point.bucket_id = obs.bucket_id;
    point.observed = obs.value;
    point.y_model = spec.value_identity_transform ? obs.value : std::log1p(obs.value);
    point.sample_count = obs.sample_count;

    if (spec.feature_type == "value_basic") {
        EnableWeightedUpdate(1.0, &point);
        return point;
    }

    if (obs.sample_count == 0) {
        return InvalidPoint(obs.series_key, obs.bucket_id,
                            "value_sampled sample_count must be > 0");
    }
    if (obs.sample_count < config.n_min_score) {
        point.skipped_low_sample_count = true;
        point.uncertainty_source.push_back("low_sample_count");
        return point;
    }

    const double sample_count = static_cast<double>(obs.sample_count);
    const double weight = UnitWeight(sample_count, static_cast<double>(config.n_ref));
    EnableWeightedUpdate(weight, &point);
    point.extra_obs_noise_scale = config.sample_count_noise / sample_count;
    point.uncertainty_source.push_back("sample_count_noise");
    if (obs.sample_count < config.n_min_update) {
        point.uncertainty_source.push_back("low_sample_count_weight");
    }
    return point;
}

ObservedModelPoint AdaptRatioRollingObservation(const BaselineTaskSpec& spec,
                                                const BaselineRollingConfig& config,
                                                const RatioRollingObservation& obs) {
    if (obs.series_key.empty()) {
        return InvalidPoint(obs.series_key, obs.bucket_id, "series_key must not be empty");
    }
    if (spec.feature_type != "ratio") {
        return InvalidPoint(obs.series_key, obs.bucket_id, "unsupported ratio feature_type");
    }
    if (!IsValidRatioObservation(obs.numerator, obs.denominator)) {
        return InvalidPoint(obs.series_key, obs.bucket_id,
                            "ratio requires denominator > 0 and 0 <= numerator <= denominator");
    }

    ObservedModelPoint point;
    point.series_key = obs.series_key;
    point.bucket_id = obs.bucket_id;
    point.numerator = obs.numerator;
    point.denominator = obs.denominator;
    point.observed = obs.numerator / obs.denominator;
    point.y_model = SafeLogit(point.observed, config.ratio_eps_logit);

    if (obs.denominator < static_cast<double>(config.d_min_score)) {
        point.skipped_low_denominator = true;
        point.uncertainty_source.push_back("low_denominator");
        return point;
    }

    const double weight = UnitWeight(obs.denominator, static_cast<double>(config.d_ref));
    EnableWeightedUpdate(weight, &point);
    point.extra_obs_noise_scale = config.ratio_denominator_noise / obs.denominator;
    point.uncertainty_source.push_back("denominator_noise");
    if (obs.denominator < static_cast<double>(config.d_min_update)) {
        point.uncertainty_source.push_back("low_denominator_weight");
    }
    if (point.observed <= config.ratio_eps_logit || point.observed >= 1.0 - config.ratio_eps_logit) {
        point.uncertainty_source.push_back("ratio_clip");
    }
    return point;
}

}  // namespace baseline
}  // namespace flowsql
