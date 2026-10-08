// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <cassert>
#include <cmath>
#include <string>
#include <type_traits>

#include <plugins/baseline/rolling/observation_adapter.h>
#include <plugins/baseline/rolling/rolling_state.h>

using namespace flowsql;
using namespace flowsql::baseline;

namespace {

void AssertNear(double actual, double expected) {
    assert(std::fabs(actual - expected) < 1.0e-12);
}

template <typename T, typename = void>
struct HasLegacyConfidenceField : std::false_type {};

template <typename T>
struct HasLegacyConfidenceField<T, std::void_t<decltype(std::declval<T>().confidence)>>
    : std::true_type {};

BaselineRollingConfig BuildConfig() {
    BaselineRollingConfig config;
    config.daily_harmonic_order = 2;
    config.weekly_harmonic_order = 1;
    config.min_ready_hint_updates = 100;
    return config;
}

BaselineTaskSpec BuildValueSpec() {
    BaselineTaskSpec spec;
    spec.task_id = "rolling-state-value";
    spec.task_kind = "value";
    spec.feature_type = "value_basic";
    spec.feature_id = "bps";
    spec.profile = "default";
    spec.clock_spec.bucket_seconds = 60;
    spec.clock_spec.timezone = "Asia/Shanghai";
    spec.calendar_ref.calendar_id = "cn-holiday";
    spec.calendar_ref.calendar_version = "2026.1";
    return spec;
}

BootstrapSeed BuildValueSeed() {
    BootstrapSeed seed;
    seed.artifact_kind = BootstrapArtifactKind::kValue;
    seed.seed_status = BootstrapSeedStatus::kPartial;
    seed.series_key = "link-a";
    seed.task_identity.task_id = "rolling-state-value";
    seed.task_identity.task_kind = "value";
    seed.task_identity.feature_type = "value_basic";
    seed.task_identity.feature_id = "bps";
    seed.task_identity.profile = "default";
    seed.clock_spec.bucket_seconds = 60;
    seed.clock_spec.timezone = "Asia/Shanghai";
    seed.calendar_ref.calendar_id = "cn-holiday";
    seed.calendar_ref.calendar_version = "2026.1";
    seed.coverage_report.accepted_count = 500;
    seed.coverage_report.train_start_bucket = 1;
    seed.coverage_report.train_end_bucket = 20;
    seed.coverage_report.coverage_ratio = 0.5;
    seed.seeded_components = {"level", "trend", "daily", "weekly"};
    seed.enabled_components = {"level", "trend", "daily", "weekly"};
    seed.theta_init.available = true;
    seed.theta_init.model_space = "log1p";
    seed.theta_init.reference_bucket_id = 10;
    seed.theta_init.level = 100.0;
    seed.theta_init.trend = 0.5;
    seed.theta_init.daily_harmonic.push_back(BootstrapHarmonicInit{1, 1.0, 2.0});
    seed.theta_init.daily_harmonic.push_back(BootstrapHarmonicInit{3, 30.0, 40.0});
    seed.theta_init.weekly_harmonic.push_back(BootstrapHarmonicInit{1, 3.0, 4.0});
    seed.sigma_init.available = true;
    seed.sigma_init.model_space = "log1p";
    seed.sigma_init.value = 0.2;
    seed.uncertainty_init.available = true;
    seed.uncertainty_init.coverage_ratio = 0.5;
    seed.uncertainty_init.component_uncertainty.level_scale = 2.0;
    seed.uncertainty_init.component_uncertainty.trend_scale = 3.0;
    seed.uncertainty_init.component_uncertainty.daily_scale = 4.0;
    seed.uncertainty_init.component_uncertainty.weekly_scale = 5.0;
    seed.maturity_init.available = true;
    seed.maturity_init.seed_status = BootstrapSeedStatus::kPartial;
    seed.maturity_init.confidence = 0.9;
    seed.maturity_init.accepted_count = 500;
    seed.maturity_init.coverage_ratio = 0.5;
    return seed;
}

void TestBuildEmptyRollingState() {
    static_assert(!HasLegacyConfidenceField<RollingState>::value);
    const BaselineRollingConfig config = BuildConfig();
    RollingState state;
    assert(BuildEmptyRollingState("link-a", config, &state) == BaselineStatus::kOk);
    assert(state.series_key == "link-a");
    assert(!state.has_seen_observation);
    assert(state.state_status == RollingStateStatus::kColdLearning);
    AssertNear(state.sigma_init, config.sigma_floor);
    AssertNear(state.sigma, config.sigma_floor);
    AssertNear(state.learning_confidence, config.confidence_cold);
    assert(state.theta.daily.sin_coeff.size() == 2);
    assert(state.theta.weekly.sin_coeff.size() == 1);
    AssertNear(state.p_level, 9.0 * config.sigma_floor * config.sigma_floor);
    AssertNear(state.p_trend, 16.0 * config.sigma_floor * config.sigma_floor);
}

void TestEmptyStateFirstObservation() {
    const BaselineRollingConfig config = BuildConfig();
    RollingState state;
    assert(BuildEmptyRollingState("link-a", config, &state) == BaselineStatus::kOk);

    ObservedModelPoint point;
    point.status = BaselineStatus::kOk;
    point.series_key = "link-a";
    point.bucket_id = 101;
    point.y_model = std::log1p(99.0);
    point.can_update = true;
    assert(InitializeEmptyRollingStateFromObservation(point, config, &state) ==
           BaselineStatus::kOk);
    assert(state.has_seen_observation);
    assert(state.last_seen_bucket == 101);
    assert(state.accepted_update_count == 1);
    assert(state.state_status == RollingStateStatus::kColdLearning);
    AssertNear(state.theta.level, std::log1p(99.0));
    AssertNear(state.theta.trend, 0.0);

    RollingState low_state;
    assert(BuildEmptyRollingState("link-b", config, &low_state) == BaselineStatus::kOk);
    point.series_key = "link-b";
    point.can_update = false;
    assert(InitializeEmptyRollingStateFromObservation(point, config, &low_state) ==
           BaselineStatus::kInsufficientData);
    assert(!low_state.has_seen_observation);
}

void TestBootstrapSeedInitialization() {
    const BaselineRollingConfig config = BuildConfig();
    const BaselineTaskSpec spec = BuildValueSpec();
    const BootstrapSeed seed = BuildValueSeed();

    RollingState state;
    std::string diagnostics;
    assert(InitializeRollingStateFromBootstrapSeed(spec, "link-a", seed, config, &state,
                                                   &diagnostics) == BaselineStatus::kOk);
    assert(state.series_key == "link-a");
    assert(state.has_seen_observation);
    assert(state.last_seen_bucket == 20);
    assert(state.accepted_update_count == 100);
    assert(state.state_status == RollingStateStatus::kReadyHint);
    AssertNear(state.theta.level, 105.0);
    AssertNear(state.theta.trend, 0.5);
    AssertNear(state.theta.daily.sin_coeff[0], 1.0);
    AssertNear(state.theta.daily.cos_coeff[0], 2.0);
    AssertNear(state.theta.daily.sin_coeff[1], 0.0);
    AssertNear(state.theta.weekly.sin_coeff[0], 3.0);
    AssertNear(state.theta.weekly.cos_coeff[0], 4.0);
    AssertNear(state.sigma_init, 0.2);
    AssertNear(state.sigma, 0.2);
    assert(state.bootstrap_seed_status == BootstrapSeedStatus::kPartial);
    assert(state.daily_prior_quality == RollingSeasonalPriorQuality::kPartial);
    assert(state.weekly_prior_quality == RollingSeasonalPriorQuality::kPartial);
    AssertNear(state.learning_confidence, 0.8);
    AssertNear(state.p_level, 0.72);
    AssertNear(state.p_trend, 1.92);
    AssertNear(state.theta.daily.sin_p[0], 0.64);
    AssertNear(state.theta.weekly.sin_p[0], 1.8);
    assert(diagnostics.find("ignored_daily_harmonic_order") != std::string::npos);
}

void TestBootstrapSeedRejectsInvalidInputs() {
    const BaselineRollingConfig config = BuildConfig();
    const BaselineTaskSpec spec = BuildValueSpec();
    BootstrapSeed seed = BuildValueSeed();
    RollingState state;
    std::string diagnostics;

    seed.series_key = "other";
    assert(InitializeRollingStateFromBootstrapSeed(spec, "link-a", seed, config, &state,
                                                   &diagnostics) ==
           BaselineStatus::kIncompatibleArtifact);

    seed = BuildValueSeed();
    seed.seed_status = BootstrapSeedStatus::kNone;
    assert(InitializeRollingStateFromBootstrapSeed(spec, "link-a", seed, config, &state,
                                                   &diagnostics) ==
           BaselineStatus::kInsufficientData);

    seed = BuildValueSeed();
    seed.theta_init.model_space = "identity";
    assert(InitializeRollingStateFromBootstrapSeed(spec, "link-a", seed, config, &state,
                                                   &diagnostics) ==
           BaselineStatus::kIncompatibleArtifact);

    seed = BuildValueSeed();
    seed.uncertainty_init.available = false;
    assert(InitializeRollingStateFromBootstrapSeed(spec, "link-a", seed, config, &state,
                                                   &diagnostics) ==
           BaselineStatus::kInsufficientData);
}

}  // namespace

int main() {
    auto spec = BuildValueSpec();
    spec.feature_type = "value_sampled";
    spec.profile = "cont_core";
    spec.value_identity_transform = true;
    auto seed = BuildValueSeed();
    seed.task_identity.feature_type = spec.feature_type;
    seed.task_identity.profile = spec.profile;
    seed.theta_init.model_space = "identity";
    seed.sigma_init.model_space = "identity";
    RollingState state;
    assert(InitializeRollingStateFromBootstrapSeed(spec, seed.series_key, seed, BuildConfig(), &state) ==
           BaselineStatus::kOk);
    seed.theta_init.model_space = "log1p";
    seed.sigma_init.model_space = "log1p";
    assert(InitializeRollingStateFromBootstrapSeed(spec, seed.series_key, seed, BuildConfig(), &state) ==
           BaselineStatus::kIncompatibleArtifact);
    TestBuildEmptyRollingState();
    TestEmptyStateFirstObservation();
    TestBootstrapSeedInitialization();
    TestBootstrapSeedRejectsInvalidInputs();
    return 0;
}
