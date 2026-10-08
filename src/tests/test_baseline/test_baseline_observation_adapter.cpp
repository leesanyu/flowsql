// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <algorithm>
#include <cassert>
#include <cmath>

#include <plugins/baseline/model/profile_config.h>
#include <plugins/baseline/model/task_spec.h>
#include <plugins/baseline/rolling/observation_adapter.h>
#include <plugins/baseline/rolling/rolling_config.h>

using namespace flowsql;
using namespace flowsql::baseline;

namespace {

void AssertNear(double actual, double expected) {
    assert(std::fabs(actual - expected) < 1.0e-12);
}

BaselineTaskSpec BuildValueSpec(const std::string& feature_type) {
    BaselineTaskSpec spec;
    spec.task_id = "adapter-value-task";
    spec.task_kind = "value";
    spec.feature_id = "bps";
    spec.feature_type = feature_type;
    spec.profile = feature_type == "value_sampled" ? "cont_core" : "default";
    return spec;
}

BaselineTaskSpec BuildRatioSpec() {
    BaselineTaskSpec spec;
    spec.task_id = "adapter-ratio-task";
    spec.task_kind = "ratio";
    spec.feature_id = "success_rate";
    spec.feature_type = "ratio";
    spec.profile = "rate_core";
    return spec;
}

void TestValueBasicObservation() {
    BaselineRollingConfig config;
    ValueRollingObservation obs;
    obs.series_key = "link-a";
    obs.bucket_id = 100;
    obs.value = 99.0;
    obs.sample_count = 0;

    const ObservedModelPoint point =
        AdaptValueRollingObservation(BuildValueSpec("value_basic"), config, obs);

    assert(point.status == BaselineStatus::kOk);
    assert(point.series_key == "link-a");
    assert(point.bucket_id == 100);
    AssertNear(point.observed, 99.0);
    AssertNear(point.y_model, std::log1p(99.0));
    assert(point.can_score);
    assert(point.can_update);
    AssertNear(point.score_weight, 1.0);
    AssertNear(point.update_weight, 1.0);
    AssertNear(point.extra_obs_noise_scale, 0.0);
    assert(!point.skipped_low_sample_count);
}

void TestValueRejectsInvalidInput() {
    BaselineRollingConfig config;
    ValueRollingObservation negative;
    negative.series_key = "link-a";
    negative.bucket_id = 1;
    negative.value = -1.0;
    assert(AdaptValueRollingObservation(BuildValueSpec("value_basic"), config, negative).status ==
           BaselineStatus::kInvalidArgument);

    ValueRollingObservation sampled = negative;
    sampled.value = 1.0;
    sampled.sample_count = 0;
    assert(AdaptValueRollingObservation(BuildValueSpec("value_sampled"), config, sampled).status ==
           BaselineStatus::kInvalidArgument);
}

void TestSampledValueSupportBuckets() {
    BaselineRollingConfig config;
    ValueRollingObservation obs;
    obs.series_key = "rtt-a";
    obs.bucket_id = 2;
    obs.value = 19.0;

    obs.sample_count = 2;
    ObservedModelPoint low =
        AdaptValueRollingObservation(BuildValueSpec("value_sampled"), config, obs);
    assert(low.status == BaselineStatus::kOk);
    assert(!low.can_score);
    assert(!low.can_update);
    assert(low.skipped_low_sample_count);
    AssertNear(low.score_weight, 0.0);
    AssertNear(low.update_weight, 0.0);

    obs.sample_count = 5;
    ObservedModelPoint medium =
        AdaptValueRollingObservation(BuildValueSpec("value_sampled"), config, obs);
    assert(medium.status == BaselineStatus::kOk);
    assert(medium.can_score);
    assert(medium.can_update);
    assert(!medium.skipped_low_sample_count);
    AssertNear(medium.score_weight, 0.5);
    AssertNear(medium.update_weight, 0.5);
    AssertNear(medium.extra_obs_noise_scale, 0.2);

    obs.sample_count = 20;
    ObservedModelPoint normal =
        AdaptValueRollingObservation(BuildValueSpec("value_sampled"), config, obs);
    assert(normal.status == BaselineStatus::kOk);
    AssertNear(normal.score_weight, 1.0);
    AssertNear(normal.update_weight, 1.0);
    AssertNear(normal.extra_obs_noise_scale, 0.05);
}

void TestRatioRejectsInvalidInput() {
    BaselineRollingConfig config;
    RatioRollingObservation obs;
    obs.series_key = "svc-a";
    obs.bucket_id = 10;

    obs.numerator = 1.0;
    obs.denominator = 0.0;
    assert(AdaptRatioRollingObservation(BuildRatioSpec(), config, obs).status ==
           BaselineStatus::kInvalidArgument);

    obs.numerator = -1.0;
    obs.denominator = 10.0;
    assert(AdaptRatioRollingObservation(BuildRatioSpec(), config, obs).status ==
           BaselineStatus::kInvalidArgument);

    obs.numerator = 11.0;
    obs.denominator = 10.0;
    assert(AdaptRatioRollingObservation(BuildRatioSpec(), config, obs).status ==
           BaselineStatus::kInvalidArgument);
}

void TestRatioSupportBucketsAndLogit() {
    BaselineRollingConfig config;
    RatioRollingObservation obs;
    obs.series_key = "svc-a";
    obs.bucket_id = 11;
    obs.numerator = 1.0;
    obs.denominator = 5.0;

    ObservedModelPoint low = AdaptRatioRollingObservation(BuildRatioSpec(), config, obs);
    assert(low.status == BaselineStatus::kOk);
    assert(!low.can_score);
    assert(!low.can_update);
    assert(low.skipped_low_denominator);
    AssertNear(low.score_weight, 0.0);
    AssertNear(low.update_weight, 0.0);

    obs.numerator = 25.0;
    obs.denominator = 50.0;
    ObservedModelPoint medium = AdaptRatioRollingObservation(BuildRatioSpec(), config, obs);
    assert(medium.status == BaselineStatus::kOk);
    assert(medium.can_score);
    assert(medium.can_update);
    AssertNear(medium.observed, 0.5);
    AssertNear(medium.y_model, 0.0);
    AssertNear(medium.score_weight, 0.5);
    AssertNear(medium.update_weight, 0.5);
    AssertNear(medium.extra_obs_noise_scale, 0.02);

    obs.numerator = 0.0;
    obs.denominator = 100.0;
    ObservedModelPoint zero = AdaptRatioRollingObservation(BuildRatioSpec(), config, obs);
    assert(zero.status == BaselineStatus::kOk);
    assert(std::isfinite(zero.y_model));
    AssertNear(zero.y_model, std::log(kRatioEpsLogit / (1.0 - kRatioEpsLogit)));

    obs.numerator = 100.0;
    ObservedModelPoint one = AdaptRatioRollingObservation(BuildRatioSpec(), config, obs);
    assert(one.status == BaselineStatus::kOk);
    assert(std::isfinite(one.y_model));
    AssertNear(one.y_model, std::log((1.0 - kRatioEpsLogit) / kRatioEpsLogit));
}

}  // namespace

int main() {
    auto identity_spec = BuildValueSpec("value_sampled");
    identity_spec.value_identity_transform = true;
    BaselineRollingConfig config;
    const auto identity = AdaptValueRollingObservation(identity_spec, config, {"identity", 1, 100.0, 50});
    assert(identity.status == BaselineStatus::kOk);
    AssertNear(identity.y_model, 100.0);
    config.ratio_eps_logit = 0.1;
    const auto clipped = AdaptRatioRollingObservation(BuildRatioSpec(), config, {"ratio", 1, 5.0, 100.0});
    assert(clipped.status == BaselineStatus::kOk);
    AssertNear(clipped.y_model, std::log(0.1 / 0.9));
    assert(std::find(clipped.uncertainty_source.begin(), clipped.uncertainty_source.end(), "ratio_clip") !=
           clipped.uncertainty_source.end());
    TestValueBasicObservation();
    TestValueRejectsInvalidInput();
    TestSampledValueSupportBuckets();
    TestRatioRejectsInvalidInput();
    TestRatioSupportBucketsAndLogit();
    return 0;
}
