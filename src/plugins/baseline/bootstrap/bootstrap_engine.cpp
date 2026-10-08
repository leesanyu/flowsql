// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "bootstrap_engine.h"

#include <common/error_code.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "plugins/baseline/bootstrap/formal_model_trainer.h"
#include "plugins/baseline/config/runtime_config.h"
#include "plugins/baseline/model/formal_predictor.h"
#include "plugins/baseline/model/observation_validation.h"
#include "plugins/baseline/model/profile_config.h"
#include "plugins/baseline/relation/relation_basis.h"
#include "plugins/baseline/relation/relation_summary.h"
#include "plugins/baseline/relation/routed_summary.h"
#include "plugins/baseline/rolling/rolling_config.h"
#include "plugins/baseline/rolling/rolling_feature_batch.h"
#include "plugins/baseline/serialization/json_serialization.h"

namespace flowsql {
namespace baseline {

namespace {

BootstrapTaskIdentity MakeIdentity(const BaselineTaskSpec& spec) {
    BootstrapTaskIdentity identity;
    identity.task_id = spec.task_id;
    identity.task_kind = spec.task_kind;
    identity.feature_type = spec.feature_type.empty() ? spec.task_kind : spec.feature_type;
    identity.feature_id = spec.feature_id;
    identity.profile = spec.profile;
    return identity;
}

BootstrapClockSpec MakeClockSpec(const BaselineTaskSpec& spec) {
    BootstrapClockSpec clock;
    clock.bucket_seconds = spec.clock_spec.bucket_seconds > 0
                               ? spec.clock_spec.bucket_seconds
                               : spec.delta;
    clock.timezone = !spec.clock_spec.timezone.empty() ? spec.clock_spec.timezone : spec.tz;
    if (clock.timezone.empty()) clock.timezone = "UTC";
    return clock;
}

BootstrapClockSpec MakeClockSpec(const RelationTaskCreateSpec& spec) {
    BootstrapClockSpec clock;
    clock.bucket_seconds = spec.clock_spec.delta;
    clock.timezone = spec.clock_spec.tz.empty() ? "UTC" : spec.clock_spec.tz;
    return clock;
}

BootstrapCalendarRef MakeCalendarRef(const BaselineTaskSpec& spec) {
    BootstrapCalendarRef calendar;
    calendar.calendar_id = spec.calendar_ref.calendar_id;
    calendar.calendar_version = spec.calendar_ref.calendar_version;
    return calendar;
}

BootstrapCalendarRef MakeCalendarRef(const RelationTaskCreateSpec& spec) {
    BootstrapCalendarRef calendar;
    calendar.calendar_id = spec.task_spec.calendar_ref.calendar_id;
    calendar.calendar_version = spec.task_spec.calendar_ref.calendar_version;
    return calendar;
}

BaselineStatus MapTrainFailure(FormalTrainFailureCode failure) {
    switch (failure) {
        case FormalTrainFailureCode::kNone:
            return BaselineStatus::kOk;
        case FormalTrainFailureCode::kInsufficientTrainData:
            return BaselineStatus::kInsufficientData;
        case FormalTrainFailureCode::kSolverUnavailable:
        case FormalTrainFailureCode::kTrainFailed:
            return BaselineStatus::kTrainFailed;
    }
    return BaselineStatus::kTrainFailed;
}

ReplayWindowSummary BuildWindowSummary(uint64_t count,
                                       int64_t first_bucket,
                                       int64_t last_bucket) {
    ReplayWindowSummary window;
    window.has_data = count > 0;
    window.observation_count = count;
    window.first_bucket_id = first_bucket;
    window.last_bucket_id = last_bucket;
    window.request_bucket_start = first_bucket;
    window.request_bucket_end = last_bucket;
    return window;
}

void FillCoverage(uint64_t accepted_count,
                  uint64_t rejected_count,
                  int64_t train_start,
                  int64_t train_end,
                  BootstrapCoverageReport* out) {
    if (!out) return;
    out->accepted_count = accepted_count;
    out->rejected_count = rejected_count;
    out->train_start_bucket = train_start;
    out->train_end_bucket = train_end;
    const int64_t span = train_end >= train_start ? train_end - train_start + 1 : 0;
    out->coverage_ratio =
        span > 0 ? std::min(1.0, static_cast<double>(accepted_count) / static_cast<double>(span))
                 : 0.0;
}

BootstrapCoverageReport BuildCoverage(uint64_t accepted_count,
                                      uint64_t rejected_count,
                                      int64_t train_start,
                                      int64_t train_end) {
    BootstrapCoverageReport coverage;
    FillCoverage(accepted_count, rejected_count, train_start, train_end, &coverage);
    return coverage;
}

bool ViolatesMinObservationCount(uint64_t accepted_count,
                                 const BootstrapTrainOptions& options) {
    return options.min_observation_count > 0 &&
           accepted_count < static_cast<uint64_t>(options.min_observation_count);
}

void ApplyDiagnosticsOption(const BootstrapTrainOptions& options,
                            BootstrapTrainResult* result,
                            BootstrapArtifact* artifact) {
    if (options.include_diagnostics) return;
    if (result) result->diagnostics.clear();
    if (artifact) artifact->diagnostics.clear();
}

void ApplyPredictionDiagnosticsOption(const BootstrapPredictionOptions& options,
                                      BootstrapPrediction* prediction) {
    if (options.include_diagnostics || !prediction) return;
    prediction->diagnostics.clear();
}

BootstrapPrediction UnavailablePrediction(int64_t bucket_id,
                                          const char* diagnostics) {
    BootstrapPrediction prediction;
    prediction.status = BaselineStatus::kNotTrained;
    prediction.bucket_id = bucket_id;
    prediction.diagnostics = diagnostics ? diagnostics : "";
    return prediction;
}

double ZValue(double confidence_level) {
    if (confidence_level >= 0.99) return 2.576;
    if (confidence_level >= 0.95) return 1.96;
    if (confidence_level >= 0.90) return 1.645;
    return 1.0;
}

double Clamp01(double value) { return std::max(0.0, std::min(1.0, value)); }

void ApplyRatioPredictionBand(double p, double effective_n, double sigma_ref, double z,
                              BootstrapPrediction* prediction) {
    const double sigma_p = std::max(std::sqrt(std::max(p * (1.0 - p), 0.0) / effective_n), 1.0e-4);
    const double probability_half_width = z * sigma_p;
    // Formal Ratio prediction already clips p inside (0, 1); sigma_ref is in logit space.
    const double eta = std::log(p / (1.0 - p));
    const double residual_half_width = z * std::max(sigma_ref, 1.0e-3);
    const auto sigmoid = [](double value) {
        const double exp_value = std::exp(value >= 0.0 ? -value : value);
        return value >= 0.0 ? 1.0 / (1.0 + exp_value) : exp_value / (1.0 + exp_value);
    };
    // Preserve the probability interval as a minimum range for quiet histories.
    prediction->baseline_lower = std::min(Clamp01(p - probability_half_width), sigmoid(eta - residual_half_width));
    prediction->baseline_upper = std::max(Clamp01(p + probability_half_width), sigmoid(eta + residual_half_width));
    prediction->band_width = prediction->baseline_upper - prediction->baseline_lower;
    prediction->uncertainty_source = {"ratio_probability_variance", "ratio_logit_sigma_ref"};
}

template <typename TModel>
int64_t ResolvePredictDelta(const TModel& model, const BaselineTaskSpec* task_spec) {
    if (model.delta > 0) return model.delta;
    if (task_spec && task_spec->delta > 0) return task_spec->delta;
    return 0;
}

template <typename TModel>
std::string ResolvePredictTimezone(const TModel& model, const BaselineTaskSpec* task_spec) {
    if (!model.tz.empty()) return model.tz;
    if (task_spec && !task_spec->tz.empty()) return task_spec->tz;
    return "UTC";
}

BaselineRollingConfig MakeFormalFeatureConfig(const CoreBlock& core_block,
                                              int64_t bucket_seconds,
                                              const std::string& timezone) {
    BaselineRollingConfig config;
    config.bucket_seconds = bucket_seconds;
    config.timezone = timezone;
    config.daily_harmonic_order = static_cast<int32_t>(
        std::max(core_block.day_sin.size(), core_block.day_cos.size()));
    config.weekly_harmonic_order = static_cast<int32_t>(
        std::max(core_block.week_sin.size(), core_block.week_cos.size()));
    return config;
}

FormalPredictFeatureView MakeFormalFeatureView(const RollingFeatureView& view) {
    FormalPredictFeatureView result;
    result.calendar = view.calendar;
    result.day_sin = view.day_sin;
    result.day_cos = view.day_cos;
    result.day_size = view.day_size;
    result.week_sin = view.week_sin;
    result.week_cos = view.week_cos;
    result.week_size = view.week_size;
    return result;
}

void AppendDiagnostic(std::string* diagnostics, const char* token) {
    if (!diagnostics || !token || token[0] == '\0') return;
    if (!diagnostics->empty()) diagnostics->append("; ");
    diagnostics->append(token);
}

template <typename Writer>
void WriteDoubleVector(Writer* writer, const char* name, const std::vector<double>& values) {
    writer->Key(name);
    writer->StartArray();
    for (double value : values) writer->Double(value);
    writer->EndArray();
}

template <typename Writer, typename ArrayT>
void WriteDoubleArray(Writer* writer, const char* name, const ArrayT& values) {
    writer->Key(name);
    writer->StartArray();
    for (double value : values) writer->Double(value);
    writer->EndArray();
}

template <typename Writer>
void WriteStringVector(Writer* writer,
                       const char* name,
                       const std::vector<std::string>& values) {
    writer->Key(name);
    writer->StartArray();
    for (const auto& value : values) writer->String(value.c_str());
    writer->EndArray();
}

template <typename Writer>
void WriteCoreBlock(Writer* writer, const CoreBlock& core) {
    writer->Key("core_block");
    writer->StartObject();
    writer->Key("beta0");
    writer->Double(core.beta0);
    writer->Key("trend_k");
    writer->Double(core.trend_k);
    WriteDoubleVector(writer, "day_sin", core.day_sin);
    WriteDoubleVector(writer, "day_cos", core.day_cos);
    WriteDoubleVector(writer, "week_sin", core.week_sin);
    WriteDoubleVector(writer, "week_cos", core.week_cos);
    writer->EndObject();
}

template <typename Writer>
void WriteMonthPosBlock(Writer* writer, const MonthPosBlock& block) {
    writer->Key("monthpos_block");
    writer->StartObject();
    writer->Key("enabled");
    writer->Bool(block.enabled);
    WriteDoubleArray(writer, "dom_coeff", block.dom_coeff);
    WriteDoubleVector(writer, "dme_coeff", block.dme_coeff);
    WriteDoubleArray(writer, "lwd_coeff", block.lwd_coeff);
    WriteDoubleArray(writer, "dom_center", block.dom_center);
    WriteDoubleVector(writer, "dme_center", block.dme_center);
    WriteDoubleArray(writer, "lwd_center", block.lwd_center);
    writer->EndObject();
}

template <typename Writer>
void WriteEventBlock(Writer* writer, const EventBlock& block) {
    writer->Key("event_block");
    writer->StartObject();
    writer->Key("enabled");
    writer->Bool(block.enabled);
    WriteStringField(writer, "calendar_id", block.calendar_id);
    WriteStringField(writer, "calendar_version", block.calendar_version);
    WriteStringVector(writer, "active_event_codes", block.active_event_codes);
    WriteDoubleVector(writer, "coeff", block.coeff);
    writer->EndObject();
}

template <typename Writer>
void WriteCoverage(Writer* writer, const BootstrapCoverageReport& coverage) {
    writer->Key("train_coverage");
    writer->StartObject();
    writer->Key("accepted_count");
    writer->Uint64(coverage.accepted_count);
    writer->Key("rejected_count");
    writer->Uint64(coverage.rejected_count);
    writer->Key("train_start_bucket");
    writer->Int64(coverage.train_start_bucket);
    writer->Key("train_end_bucket");
    writer->Int64(coverage.train_end_bucket);
    writer->Key("coverage_ratio");
    writer->Double(coverage.coverage_ratio);
    writer->EndObject();
}

template <typename Writer>
void WriteSeriesIdentity(Writer* writer, const std::string& series_key) {
    writer->Key("series_identity");
    writer->StartObject();
    WriteStringField(writer, "series_key", series_key);
    writer->EndObject();
}

template <typename Writer>
void WriteValueModel(Writer* writer, const ValueFormalModel& model) {
    WriteStringField(writer, "transform_name", model.transform_name);
    writer->Key("delta");
    writer->Int64(model.delta);
    WriteStringField(writer, "timezone", model.tz);
    writer->Key("train_start");
    writer->Int64(model.train_start);
    writer->Key("train_end");
    writer->Int64(model.train_end);
    writer->Key("sigma_ref");
    writer->Double(model.sigma_ref);
    writer->Key("confidence_base_at_train");
    writer->Double(model.confidence_base_at_train);
    WriteStringField(writer, "calendar_id", model.metadata.calendar_id);
    WriteStringField(writer, "calendar_version", model.metadata.calendar_version);
    WriteCoreBlock(writer, model.core_block);
    WriteMonthPosBlock(writer, model.monthpos_block);
    WriteEventBlock(writer, model.event_block);
}

template <typename Writer>
void WriteRatioModel(Writer* writer, const RatioFormalModel& model) {
    WriteStringField(writer, "transform_name", model.transform_name);
    writer->Key("delta");
    writer->Int64(model.delta);
    WriteStringField(writer, "timezone", model.tz);
    writer->Key("train_start");
    writer->Int64(model.train_start);
    writer->Key("train_end");
    writer->Int64(model.train_end);
    writer->Key("m0");
    writer->Double(model.m0);
    writer->Key("alpha0");
    writer->Double(model.alpha0);
    writer->Key("beta0");
    writer->Double(model.beta0);
    writer->Key("sigma_ref");
    writer->Double(model.sigma_ref);
    writer->Key("confidence_base_at_train");
    writer->Double(model.confidence_base_at_train);
    WriteStringField(writer, "calendar_id", model.metadata.calendar_id);
    WriteStringField(writer, "calendar_version", model.metadata.calendar_version);
    WriteCoreBlock(writer, model.core_block);
    WriteMonthPosBlock(writer, model.monthpos_block);
    WriteEventBlock(writer, model.event_block);
}

const char* SeedStatusName(BootstrapSeedStatus status) {
    switch (status) {
        case BootstrapSeedStatus::kFull:
            return "full";
        case BootstrapSeedStatus::kPartial:
            return "partial";
        case BootstrapSeedStatus::kWeak:
            return "weak";
        case BootstrapSeedStatus::kNone:
            break;
    }
    return "none";
}

std::vector<BootstrapHarmonicInit> MakeHarmonicInit(const std::vector<double>& sin_coeff,
                                                    const std::vector<double>& cos_coeff) {
    std::vector<BootstrapHarmonicInit> values;
    const std::size_t size = std::max(sin_coeff.size(), cos_coeff.size());
    values.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        BootstrapHarmonicInit item;
        item.order = static_cast<int32_t>(i + 1);
        item.sin = i < sin_coeff.size() ? sin_coeff[i] : 0.0;
        item.cos = i < cos_coeff.size() ? cos_coeff[i] : 0.0;
        values.push_back(item);
    }
    return values;
}

BootstrapThetaInit MakeThetaInit(const CoreBlock& core,
                                 const std::string& model_space,
                                 int64_t reference_bucket_id) {
    BootstrapThetaInit theta;
    theta.available = true;
    theta.model_space = model_space;
    theta.reference_bucket_id = reference_bucket_id;
    theta.level = core.beta0;
    theta.trend = core.trend_k;
    theta.daily_harmonic = MakeHarmonicInit(core.day_sin, core.day_cos);
    theta.weekly_harmonic = MakeHarmonicInit(core.week_sin, core.week_cos);
    return theta;
}

BootstrapSigmaInit MakeValueSigmaInit(const ValueFormalModel& model) {
    BootstrapSigmaInit sigma;
    sigma.available = true;
    sigma.value = model.sigma_ref;
    sigma.model_space = model.transform_name;
    sigma.source = "value_sigma_ref";
    return sigma;
}

BootstrapSigmaInit MakeRatioSigmaInit(const RatioFormalModel& model,
                                      const BootstrapCoverageReport& coverage) {
    (void)coverage;
    BootstrapSigmaInit sigma;
    sigma.available = true;
    sigma.value = model.sigma_ref;
    sigma.model_space = model.transform_name;
    sigma.source = "ratio_logit_sigma_ref";
    return sigma;
}

BootstrapRatioPriorInit MakeRatioPriorInit(const RatioFormalModel& model) {
    BootstrapRatioPriorInit prior;
    prior.available = true;
    prior.m0 = model.m0;
    prior.alpha0 = model.alpha0;
    prior.beta0 = model.beta0;
    prior.model_space = "probability";
    prior.source = "ratio_beta_prior";
    return prior;
}

BootstrapMonthPosHint MakeMonthPosHint(const MonthPosBlock& block) {
    BootstrapMonthPosHint hint;
    hint.available = block.enabled;
    if (!hint.available) return hint;
    hint.dom_coeff.assign(block.dom_coeff.begin(), block.dom_coeff.end());
    hint.dme_coeff = block.dme_coeff;
    hint.lwd_coeff.assign(block.lwd_coeff.begin(), block.lwd_coeff.end());
    hint.dom_center.assign(block.dom_center.begin(), block.dom_center.end());
    hint.dme_center = block.dme_center;
    hint.lwd_center.assign(block.lwd_center.begin(), block.lwd_center.end());
    return hint;
}

BootstrapEventHint MakeEventHint(const EventBlock& block) {
    BootstrapEventHint hint;
    hint.available = block.enabled;
    if (!hint.available) return hint;
    hint.calendar_id = block.calendar_id;
    hint.calendar_version = block.calendar_version;
    hint.active_event_codes = block.active_event_codes;
    hint.coeff = block.coeff;
    return hint;
}

BootstrapUncertaintyInit MakeUncertaintyInit(double confidence_base,
                                             double coverage_ratio,
                                             const std::string& band_source,
                                             std::vector<std::string> sources) {
    BootstrapUncertaintyInit uncertainty;
    uncertainty.available = true;
    uncertainty.confidence_base = confidence_base;
    uncertainty.confidence_level = 0.95;
    uncertainty.coverage_ratio = coverage_ratio;
    uncertainty.band_z = ZValue(uncertainty.confidence_level);
    uncertainty.band_source = band_source;
    uncertainty.uncertainty_source = std::move(sources);
    return uncertainty;
}

int64_t CoveredSeconds(const BootstrapCoverageReport& coverage,
                       const BootstrapClockSpec& clock) {
    if (clock.bucket_seconds <= 0 ||
        coverage.train_end_bucket < coverage.train_start_bucket) {
        return 0;
    }
    const int64_t bucket_count = coverage.train_end_bucket - coverage.train_start_bucket + 1;
    if (bucket_count <= 0) return 0;
    return bucket_count * clock.bucket_seconds;
}

std::vector<std::string> MakeSeededComponents(
    const BootstrapThetaInit& theta,
    const BootstrapMonthPosHint* monthpos_hint = nullptr,
    const BootstrapEventHint* event_hint = nullptr) {
    std::vector<std::string> components;
    if (!theta.available) return components;
    components.push_back("level");
    components.push_back("trend");
    if (!theta.daily_harmonic.empty()) components.push_back("daily");
    if (!theta.weekly_harmonic.empty()) components.push_back("weekly");
    if (monthpos_hint && monthpos_hint->available) components.push_back("monthpos");
    if (event_hint && event_hint->available) components.push_back("event");
    return components;
}

std::vector<std::string> MakeEnabledComponents(
    const BootstrapThetaInit& theta,
    const BootstrapCoverageReport& coverage,
    const BootstrapClockSpec& clock,
    const BootstrapMonthPosHint* monthpos_hint = nullptr,
    const BootstrapEventHint* event_hint = nullptr) {
    std::vector<std::string> components;
    if (!theta.available) return components;
    components.push_back("level");
    components.push_back("trend");
    const int64_t covered_seconds = CoveredSeconds(coverage, clock);
    if (!theta.daily_harmonic.empty() && covered_seconds >= 86400) {
        components.push_back("daily");
    }
    if (!theta.weekly_harmonic.empty() && covered_seconds >= 14 * 86400) {
        components.push_back("weekly");
    }
    if (monthpos_hint && monthpos_hint->available) components.push_back("monthpos");
    if (event_hint && event_hint->available) components.push_back("event");
    return components;
}

bool HasComponent(const std::vector<std::string>& components, const char* name) {
    return std::find(components.begin(), components.end(), name) != components.end();
}

BootstrapSeedQualityConfig CurrentSeedQualityConfig() {
    BootstrapSeedQualityConfig config;
    (void)TryGetBootstrapSeedQualityConfigOverride(&config);
    return config;
}

BootstrapSeedStatus EvaluateBootstrapSeedStatus(
    BaselineStatus train_status,
    BootstrapArtifactKind artifact_kind,
    const BootstrapCoverageReport& coverage,
    const BootstrapClockSpec& clock,
    const BootstrapThetaInit* theta,
    const BootstrapSigmaInit* sigma,
    const std::vector<std::string>& enabled_components,
    std::vector<std::string>* diagnostics = nullptr) {
    const BootstrapSeedQualityConfig config = CurrentSeedQualityConfig();
    auto add_diag = [diagnostics](const char* value) {
        if (diagnostics && value) diagnostics->push_back(value);
    };

    if (train_status != BaselineStatus::kOk) {
        add_diag("train_status_not_ok");
        return BootstrapSeedStatus::kNone;
    }

    if (artifact_kind == BootstrapArtifactKind::kRelation && (!theta || !theta->available)) {
        if (coverage.coverage_ratio >= config.full_min_coverage_ratio) {
            return BootstrapSeedStatus::kFull;
        }
        if (coverage.coverage_ratio >= config.partial_min_coverage_ratio) {
            return BootstrapSeedStatus::kPartial;
        }
        add_diag("low_coverage_ratio");
        return BootstrapSeedStatus::kWeak;
    }

    if (!theta || !theta->available) {
        add_diag("missing_theta_init");
        return BootstrapSeedStatus::kNone;
    }
    if (!sigma || !sigma->available || !std::isfinite(sigma->value) || sigma->value <= 0.0) {
        add_diag("missing_sigma_init");
        return BootstrapSeedStatus::kNone;
    }

    if (coverage.coverage_ratio < config.partial_min_coverage_ratio) {
        add_diag("low_coverage_ratio");
        return BootstrapSeedStatus::kWeak;
    }

    const int64_t covered_seconds = CoveredSeconds(coverage, clock);
    const bool requires_daily = !theta->daily_harmonic.empty();
    const bool requires_weekly = !theta->weekly_harmonic.empty();
    const bool daily_ready =
        !requires_daily ||
        (HasComponent(enabled_components, "daily") &&
         covered_seconds >=
             static_cast<int64_t>(config.daily_min_span_days) * 86400 &&
         coverage.coverage_ratio >= config.daily_phase_coverage_ratio);
    const bool weekly_ready =
        !requires_weekly ||
        (HasComponent(enabled_components, "weekly") &&
         covered_seconds >=
             static_cast<int64_t>(config.weekly_min_span_days) * 86400 &&
         coverage.coverage_ratio >= config.weekly_phase_coverage_ratio);

    if (coverage.coverage_ratio >= config.full_min_coverage_ratio &&
        daily_ready &&
        weekly_ready) {
        return BootstrapSeedStatus::kFull;
    }

    if (requires_daily && !daily_ready) add_diag("insufficient_daily_coverage");
    if (requires_weekly && !weekly_ready) add_diag("insufficient_weekly_coverage");
    return (requires_daily && !HasComponent(enabled_components, "daily"))
               ? BootstrapSeedStatus::kWeak
               : BootstrapSeedStatus::kPartial;
}

BootstrapMaturityInit MakeMaturityInit(BootstrapSeedStatus seed_status,
                                       double confidence,
                                       const BootstrapCoverageReport& coverage) {
    BootstrapMaturityInit maturity;
    maturity.available = true;
    maturity.seed_status = seed_status;
    maturity.confidence = confidence;
    maturity.accepted_count = coverage.accepted_count;
    maturity.rejected_count = coverage.rejected_count;
    maturity.coverage_ratio = coverage.coverage_ratio;
    return maturity;
}

void FillValueSeedInitializers(const ValueFormalModel& model,
                               const BootstrapCoverageReport& coverage,
                               BootstrapSeedStatus seed_status,
                               BootstrapThetaInit* theta,
                               BootstrapMonthPosHint* monthpos_hint,
                               BootstrapEventHint* event_hint,
                               BootstrapSigmaInit* sigma,
                               BootstrapRatioPriorInit* ratio_prior,
                               BootstrapUncertaintyInit* uncertainty,
                               BootstrapMaturityInit* maturity) {
    if (theta) {
        *theta = MakeThetaInit(model.core_block, model.transform_name, model.train_start);
    }
    if (monthpos_hint) *monthpos_hint = MakeMonthPosHint(model.monthpos_block);
    if (event_hint) *event_hint = MakeEventHint(model.event_block);
    if (sigma) *sigma = MakeValueSigmaInit(model);
    if (ratio_prior) *ratio_prior = BootstrapRatioPriorInit{};
    if (uncertainty) {
        *uncertainty = MakeUncertaintyInit(model.confidence_base_at_train,
                                           coverage.coverage_ratio,
                                           "value_sigma_ref",
                                           {"value_sigma_ref"});
    }
    if (maturity) {
        *maturity = MakeMaturityInit(seed_status, model.confidence_base_at_train, coverage);
    }
}

void FillRatioSeedInitializers(const RatioFormalModel& model,
                               const BootstrapCoverageReport& coverage,
                               BootstrapSeedStatus seed_status,
                               BootstrapThetaInit* theta,
                               BootstrapMonthPosHint* monthpos_hint,
                               BootstrapEventHint* event_hint,
                               BootstrapSigmaInit* sigma,
                               BootstrapRatioPriorInit* ratio_prior,
                               BootstrapUncertaintyInit* uncertainty,
                               BootstrapMaturityInit* maturity) {
    if (theta) {
        *theta = MakeThetaInit(model.core_block, model.transform_name, model.train_start);
    }
    if (monthpos_hint) *monthpos_hint = MakeMonthPosHint(model.monthpos_block);
    if (event_hint) *event_hint = MakeEventHint(model.event_block);
    if (sigma) *sigma = MakeRatioSigmaInit(model, coverage);
    if (ratio_prior) *ratio_prior = MakeRatioPriorInit(model);
    if (uncertainty) {
        *uncertainty = MakeUncertaintyInit(model.confidence_base_at_train,
                                           coverage.coverage_ratio,
                                           "ratio_logit_sigma_ref",
                                           {"ratio_logit_sigma_ref"});
    }
    if (maturity) {
        *maturity = MakeMaturityInit(seed_status, model.confidence_base_at_train, coverage);
    }
}

template <typename Writer>
void WriteHarmonicInit(Writer* writer,
                       const char* name,
                       const std::vector<BootstrapHarmonicInit>& values) {
    writer->Key(name);
    writer->StartArray();
    for (const auto& item : values) {
        writer->StartObject();
        writer->Key("order");
        writer->Int(item.order);
        writer->Key("sin");
        writer->Double(item.sin);
        writer->Key("cos");
        writer->Double(item.cos);
        writer->EndObject();
    }
    writer->EndArray();
}

template <typename Writer>
void WriteThetaInit(Writer* writer, const BootstrapThetaInit& theta) {
    if (!theta.available) return;
    writer->Key("theta_init");
    writer->StartObject();
    WriteStringField(writer, "model_space", theta.model_space);
    writer->Key("reference_bucket_id");
    writer->Int64(theta.reference_bucket_id);
    writer->Key("level");
    writer->Double(theta.level);
    writer->Key("trend");
    writer->Double(theta.trend);
    WriteHarmonicInit(writer, "daily_harmonic", theta.daily_harmonic);
    WriteHarmonicInit(writer, "weekly_harmonic", theta.weekly_harmonic);
    writer->EndObject();
}

template <typename Writer>
void WriteSigmaInit(Writer* writer, const BootstrapSigmaInit& sigma) {
    if (!sigma.available) return;
    writer->Key("sigma_init");
    writer->StartObject();
    writer->Key("value");
    writer->Double(sigma.value);
    WriteStringField(writer, "model_space", sigma.model_space);
    WriteStringField(writer, "source", sigma.source);
    writer->EndObject();
}

template <typename Writer>
void WriteRatioPriorInit(Writer* writer, const BootstrapRatioPriorInit& prior) {
    if (!prior.available) return;
    writer->Key("ratio_prior_init");
    writer->StartObject();
    writer->Key("m0");
    writer->Double(prior.m0);
    writer->Key("alpha0");
    writer->Double(prior.alpha0);
    writer->Key("beta0");
    writer->Double(prior.beta0);
    WriteStringField(writer, "model_space", prior.model_space);
    WriteStringField(writer, "source", prior.source);
    writer->EndObject();
}

template <typename Writer>
void WriteUncertaintyInit(Writer* writer, const BootstrapUncertaintyInit& uncertainty) {
    if (!uncertainty.available) return;
    writer->Key("uncertainty_init");
    writer->StartObject();
    writer->Key("confidence_base");
    writer->Double(uncertainty.confidence_base);
    writer->Key("confidence_level");
    writer->Double(uncertainty.confidence_level);
    writer->Key("coverage_ratio");
    writer->Double(uncertainty.coverage_ratio);
    writer->Key("band_z");
    writer->Double(uncertainty.band_z);
    WriteStringField(writer, "band_source", uncertainty.band_source);
    WriteStringVector(writer, "uncertainty_source", uncertainty.uncertainty_source);
    writer->Key("component_uncertainty");
    writer->StartObject();
    writer->Key("level_scale");
    writer->Double(uncertainty.component_uncertainty.level_scale);
    writer->Key("trend_scale");
    writer->Double(uncertainty.component_uncertainty.trend_scale);
    writer->Key("daily_scale");
    writer->Double(uncertainty.component_uncertainty.daily_scale);
    writer->Key("weekly_scale");
    writer->Double(uncertainty.component_uncertainty.weekly_scale);
    writer->EndObject();
    writer->EndObject();
}

template <typename Writer>
void WriteMaturityInit(Writer* writer, const BootstrapMaturityInit& maturity) {
    if (!maturity.available) return;
    writer->Key("maturity_init");
    writer->StartObject();
    WriteStringField(writer, "seed_status", SeedStatusName(maturity.seed_status));
    writer->Key("confidence");
    writer->Double(maturity.confidence);
    writer->Key("accepted_count");
    writer->Uint64(maturity.accepted_count);
    writer->Key("rejected_count");
    writer->Uint64(maturity.rejected_count);
    writer->Key("coverage_ratio");
    writer->Double(maturity.coverage_ratio);
    writer->EndObject();
}

template <typename Writer>
void WriteMonthPosHint(Writer* writer, const BootstrapMonthPosHint& hint) {
    if (!hint.available) return;
    writer->Key("monthpos_hint");
    writer->StartObject();
    writer->Key("available");
    writer->Bool(hint.available);
    WriteDoubleVector(writer, "dom_coeff", hint.dom_coeff);
    WriteDoubleVector(writer, "dme_coeff", hint.dme_coeff);
    WriteDoubleVector(writer, "lwd_coeff", hint.lwd_coeff);
    WriteDoubleVector(writer, "dom_center", hint.dom_center);
    WriteDoubleVector(writer, "dme_center", hint.dme_center);
    WriteDoubleVector(writer, "lwd_center", hint.lwd_center);
    writer->EndObject();
}

template <typename Writer>
void WriteEventHint(Writer* writer, const BootstrapEventHint& hint) {
    if (!hint.available) return;
    writer->Key("event_hint");
    writer->StartObject();
    writer->Key("available");
    writer->Bool(hint.available);
    WriteStringField(writer, "calendar_id", hint.calendar_id);
    WriteStringField(writer, "calendar_version", hint.calendar_version);
    WriteStringVector(writer, "active_event_codes", hint.active_event_codes);
    WriteDoubleVector(writer, "coeff", hint.coeff);
    writer->EndObject();
}

bool ReadStringVector(const rapidjson::Value& obj,
                      const char* name,
                      std::vector<std::string>* out) {
    if (!out || !obj.HasMember(name) || !obj[name].IsArray()) return false;
    out->clear();
    for (const auto& item : obj[name].GetArray()) {
        if (!item.IsString()) return false;
        out->push_back(item.GetString());
    }
    return true;
}

bool ReadUintVector(const rapidjson::Value& obj,
                    const char* name,
                    std::vector<uint32_t>* out) {
    if (!out || !obj.HasMember(name) || !obj[name].IsArray()) return false;
    out->clear();
    for (const auto& item : obj[name].GetArray()) {
        if (!item.IsUint()) return false;
        out->push_back(item.GetUint());
    }
    return true;
}

bool ReadDoubleVector(const rapidjson::Value& obj,
                      const char* name,
                      std::vector<double>* out) {
    if (!out || !obj.HasMember(name) || !obj[name].IsArray()) return false;
    out->clear();
    for (const auto& item : obj[name].GetArray()) {
        if (!item.IsNumber()) return false;
        out->push_back(item.GetDouble());
    }
    return true;
}

template <std::size_t Size>
bool ReadDoubleArrayField(const rapidjson::Value& obj,
                          const char* name,
                          std::array<double, Size>* out) {
    if (!out || !obj.HasMember(name) || !obj[name].IsArray()) return false;
    const auto& array = obj[name];
    if (array.Size() != Size) return false;
    for (rapidjson::SizeType i = 0; i < array.Size(); ++i) {
        if (!array[i].IsNumber()) return false;
        (*out)[i] = array[i].GetDouble();
    }
    return true;
}

bool ReadCoreBlock(const rapidjson::Value& obj, CoreBlock* out);
bool ReadMonthPosBlock(const rapidjson::Value& obj, MonthPosBlock* out);
bool ReadEventBlock(const rapidjson::Value& obj, EventBlock* out);
void ReadCoverage(const rapidjson::Value& obj, BootstrapCoverageReport* out);

bool ReadSeriesIdentity(const rapidjson::Value& obj, std::string* out) {
    if (!out || !obj.HasMember("series_identity") ||
        !obj["series_identity"].IsObject()) {
        return false;
    }
    std::string series_key;
    if (!ReadString(obj["series_identity"], "series_key", &series_key) ||
        series_key.empty()) {
        return false;
    }
    *out = std::move(series_key);
    return true;
}

bool ReadRelationBasis(const rapidjson::Value& obj, RelationServiceBasis* out) {
    if (!out || !obj.IsObject()) return false;
    RelationServiceBasis basis;
    if (!ReadString(obj, "metric", &basis.metric_name) ||
        !ReadString(obj, "feature_base", &basis.feature_base) ||
        !ReadString(obj, "group_space_id", &basis.group_space_id) ||
        !ReadString(obj, "group_space_version", &basis.group_space_version) ||
        !ReadUintVector(obj, "support_explicit", &basis.support_explicit) ||
        !ReadUintVector(obj, "stable_head", &basis.stable_head) ||
        !ReadDoubleVector(obj, "head_proto_q", &basis.head_proto_q)) {
        return false;
    }
    if (!obj.HasMember("basis_version") || !obj["basis_version"].IsUint64() ||
        !obj.HasMember("k_head") || !obj["k_head"].IsInt()) {
        return false;
    }
    basis.basis_version = obj["basis_version"].GetUint64();
    basis.k_head = obj["k_head"].GetInt();
    (void)ReadUintVector(obj, "other_group_idxs", &basis.other_group_idxs);
    *out = std::move(basis);
    return true;
}

bool ReadValueFormalModel(const rapidjson::Value& model,
                          uint64_t model_version,
                          std::shared_ptr<ValueFormalModel>* out) {
    if (!out || !model.IsObject()) return false;
    auto value_model = std::make_shared<ValueFormalModel>();
    value_model->metadata.kind = FormalModelKind::kValueBaseline;
    value_model->metadata.model_version = model_version;
    value_model->readiness = ModelReadiness::kCoreNoMonthReady;
    if (!ReadString(model, "transform_name", &value_model->transform_name) ||
        !ReadString(model, "timezone", &value_model->tz) ||
        !model.HasMember("delta") || !model["delta"].IsInt64() ||
        !model.HasMember("train_start") || !model["train_start"].IsInt64() ||
        !model.HasMember("train_end") || !model["train_end"].IsInt64() ||
        !model.HasMember("sigma_ref") || !model["sigma_ref"].IsNumber() ||
        !model.HasMember("confidence_base_at_train") ||
        !model["confidence_base_at_train"].IsNumber() ||
        !ReadCoreBlock(model, &value_model->core_block)) {
        return false;
    }
    value_model->delta = model["delta"].GetInt64();
    value_model->train_start = model["train_start"].GetInt64();
    value_model->train_end = model["train_end"].GetInt64();
    value_model->sigma_ref = model["sigma_ref"].GetDouble();
    value_model->confidence_base_at_train = model["confidence_base_at_train"].GetDouble();
    value_model->metadata.train_bucket_start = value_model->train_start;
    value_model->metadata.train_bucket_end = value_model->train_end;
    (void)ReadString(model, "calendar_id", &value_model->metadata.calendar_id);
    (void)ReadString(model, "calendar_version", &value_model->metadata.calendar_version);
    if (!ReadMonthPosBlock(model, &value_model->monthpos_block) ||
        !ReadEventBlock(model, &value_model->event_block)) {
        return false;
    }
    if (value_model->metadata.calendar_id.empty() && value_model->event_block.enabled) {
        value_model->metadata.calendar_id = value_model->event_block.calendar_id;
        value_model->metadata.calendar_version = value_model->event_block.calendar_version;
    }
    value_model->readiness = value_model->monthpos_block.enabled
                                 ? ModelReadiness::kMonthposReady
                                 : ModelReadiness::kCoreNoMonthReady;
    *out = std::move(value_model);
    return true;
}

bool ReadRatioFormalModel(const rapidjson::Value& model,
                          uint64_t model_version,
                          std::shared_ptr<RatioFormalModel>* out) {
    if (!out || !model.IsObject()) return false;
    auto ratio_model = std::make_shared<RatioFormalModel>();
    ratio_model->metadata.kind = FormalModelKind::kRatioBaseline;
    ratio_model->metadata.model_version = model_version;
    ratio_model->readiness = ModelReadiness::kCoreNoMonthReady;
    if (!ReadString(model, "transform_name", &ratio_model->transform_name) ||
        !ReadString(model, "timezone", &ratio_model->tz) ||
        !model.HasMember("delta") || !model["delta"].IsInt64() ||
        !model.HasMember("train_start") || !model["train_start"].IsInt64() ||
        !model.HasMember("train_end") || !model["train_end"].IsInt64() ||
        !model.HasMember("m0") || !model["m0"].IsNumber() ||
        !model.HasMember("alpha0") || !model["alpha0"].IsNumber() ||
        !model.HasMember("beta0") || !model["beta0"].IsNumber() ||
        !model.HasMember("sigma_ref") || !model["sigma_ref"].IsNumber() ||
        !model.HasMember("confidence_base_at_train") ||
        !model["confidence_base_at_train"].IsNumber() ||
        !ReadCoreBlock(model, &ratio_model->core_block)) {
        return false;
    }
    ratio_model->delta = model["delta"].GetInt64();
    ratio_model->train_start = model["train_start"].GetInt64();
    ratio_model->train_end = model["train_end"].GetInt64();
    ratio_model->m0 = model["m0"].GetDouble();
    ratio_model->alpha0 = model["alpha0"].GetDouble();
    ratio_model->beta0 = model["beta0"].GetDouble();
    ratio_model->sigma_ref = model["sigma_ref"].GetDouble();
    ratio_model->confidence_base_at_train = model["confidence_base_at_train"].GetDouble();
    ratio_model->metadata.train_bucket_start = ratio_model->train_start;
    ratio_model->metadata.train_bucket_end = ratio_model->train_end;
    (void)ReadString(model, "calendar_id", &ratio_model->metadata.calendar_id);
    (void)ReadString(model, "calendar_version", &ratio_model->metadata.calendar_version);
    if (!ReadMonthPosBlock(model, &ratio_model->monthpos_block) ||
        !ReadEventBlock(model, &ratio_model->event_block)) {
        return false;
    }
    if (ratio_model->metadata.calendar_id.empty() && ratio_model->event_block.enabled) {
        ratio_model->metadata.calendar_id = ratio_model->event_block.calendar_id;
        ratio_model->metadata.calendar_version = ratio_model->event_block.calendar_version;
    }
    ratio_model->readiness = ratio_model->monthpos_block.enabled
                                 ? ModelReadiness::kMonthposReady
                                 : ModelReadiness::kCoreNoMonthReady;
    *out = std::move(ratio_model);
    return true;
}

bool ReadRoutedSummaryArtifact(const rapidjson::Value& obj,
                               RelationRoutedBootstrapArtifact* out) {
    if (!out || !obj.IsObject()) return false;
    RelationRoutedBootstrapArtifact artifact;
    std::string task_kind;
    if (!ReadString(obj, "metric", &artifact.metric_name) ||
        !ReadString(obj, "summary", &artifact.summary_name) ||
        !ReadString(obj, "task_kind", &task_kind) ||
        !ReadTaskIdentity(obj, &artifact.task_identity) ||
        !ReadClockSpec(obj, &artifact.clock_spec) ||
        !ReadCalendarRef(obj, &artifact.calendar_ref)) {
        return false;
    }
    if (!ParseTaskKindName(task_kind, &artifact.task_kind) ||
        artifact.task_kind == BaselineTaskKind::kRelation) return false;
    artifact.basis_scoped = IsBasisScopedRelationSummary(artifact.summary_name);
    if (obj.HasMember("basis_scoped")) {
        if (!obj["basis_scoped"].IsBool() ||
            obj["basis_scoped"].GetBool() != artifact.basis_scoped) {
            return false;
        }
    }
    if (obj.HasMember("basis_version")) {
        if (!obj["basis_version"].IsUint64()) return false;
        artifact.basis_version = obj["basis_version"].GetUint64();
    }
    if (!artifact.basis_scoped) {
        artifact.basis_version = 0;
    }
    ReadCoverage(obj, &artifact.coverage_report);
    (void)ReadStringVector(obj, "seeded_components", &artifact.seeded_components);
    (void)ReadStringVector(obj, "enabled_components", &artifact.enabled_components);
    if (obj.HasMember("model") && obj["model"].IsObject()) {
        if (artifact.task_kind == BaselineTaskKind::kValue) {
            if (!ReadValueFormalModel(obj["model"], 1, &artifact.value_model)) return false;
        } else if (artifact.task_kind == BaselineTaskKind::kRatio) {
            if (!ReadRatioFormalModel(obj["model"], 1, &artifact.ratio_model)) return false;
        }
    }
    *out = std::move(artifact);
    return true;
}

bool ReadRelationFusionSummaryMetadata(const rapidjson::Value& obj,
                                       RelationFusionSummaryMetadata* out) {
    if (!out || !obj.IsObject()) return false;
    RelationFusionSummaryMetadata metadata;
    std::string task_kind;
    if (!ReadString(obj, "metric_name", &metadata.metric_name) ||
        !ReadString(obj, "summary_name", &metadata.summary_name) ||
        !ReadString(obj, "task_kind", &task_kind)) {
        return false;
    }
    if (!ParseTaskKindName(task_kind, &metadata.task_kind) ||
        metadata.task_kind == BaselineTaskKind::kRelation) {
        return false;
    }
    if (!obj.HasMember("basis_scoped") || !obj["basis_scoped"].IsBool() ||
        !obj.HasMember("basis_version") || !obj["basis_version"].IsUint64()) {
        return false;
    }
    metadata.basis_scoped = obj["basis_scoped"].GetBool();
    metadata.basis_version = metadata.basis_scoped ? obj["basis_version"].GetUint64() : 0;
    *out = std::move(metadata);
    return true;
}

bool ReadRelationFusionPatternMetadata(const rapidjson::Value& obj,
                                       RelationFusionPatternMetadata* out) {
    if (!out || !obj.IsObject()) return false;
    RelationFusionPatternMetadata metadata;
    if (!ReadString(obj, "pattern", &metadata.pattern) ||
        !ReadString(obj, "scope", &metadata.scope) ||
        !obj.HasMember("pattern_weight") || !obj["pattern_weight"].IsNumber() ||
        !ReadStringVector(obj, "required_summaries", &metadata.required_summaries) ||
        !ReadStringVector(obj, "optional_summaries", &metadata.optional_summaries) ||
        !ReadStringVector(obj, "oppose_summaries", &metadata.oppose_summaries) ||
        !ReadStringVector(obj, "metrics", &metadata.metrics)) {
        return false;
    }
    metadata.pattern_weight = obj["pattern_weight"].GetDouble();
    *out = std::move(metadata);
    return true;
}

bool ReadRelationFusionMetadata(const rapidjson::Value& obj,
                                RelationFusionBootstrapMetadata* out) {
    if (!out || !obj.IsObject()) return false;
    RelationFusionBootstrapMetadata metadata;
    if (!obj.HasMember("metadata_version") || !obj["metadata_version"].IsUint() ||
        !ReadString(obj, "feature_base", &metadata.feature_base) ||
        !obj.HasMember("summary_metadata") || !obj["summary_metadata"].IsArray() ||
        !obj.HasMember("pattern_metadata") || !obj["pattern_metadata"].IsArray()) {
        return false;
    }
    metadata.metadata_version = obj["metadata_version"].GetUint();
    if (metadata.metadata_version == 0) return false;
    for (const auto& item : obj["summary_metadata"].GetArray()) {
        RelationFusionSummaryMetadata summary;
        if (!ReadRelationFusionSummaryMetadata(item, &summary)) return false;
        metadata.summary_metadata.push_back(std::move(summary));
    }
    for (const auto& item : obj["pattern_metadata"].GetArray()) {
        RelationFusionPatternMetadata pattern;
        if (!ReadRelationFusionPatternMetadata(item, &pattern)) return false;
        metadata.pattern_metadata.push_back(std::move(pattern));
    }
    *out = std::move(metadata);
    return true;
}

bool ReadCoreBlock(const rapidjson::Value& obj, CoreBlock* out) {
    if (!out || !obj.HasMember("core_block") || !obj["core_block"].IsObject()) return false;
    const auto& core = obj["core_block"];
    if (!core.HasMember("beta0") || !core["beta0"].IsNumber() ||
        !core.HasMember("trend_k") || !core["trend_k"].IsNumber()) {
        return false;
    }
    out->beta0 = core["beta0"].GetDouble();
    out->trend_k = core["trend_k"].GetDouble();
    return ReadDoubleVector(core, "day_sin", &out->day_sin) &&
           ReadDoubleVector(core, "day_cos", &out->day_cos) &&
           ReadDoubleVector(core, "week_sin", &out->week_sin) &&
           ReadDoubleVector(core, "week_cos", &out->week_cos);
}

bool ReadMonthPosBlock(const rapidjson::Value& obj, MonthPosBlock* out) {
    if (!out) return false;
    *out = MonthPosBlock{};
    if (!obj.HasMember("monthpos_block")) return true;
    if (!obj["monthpos_block"].IsObject()) return false;
    const auto& block = obj["monthpos_block"];
    if (!block.HasMember("enabled") || !block["enabled"].IsBool()) return false;
    out->enabled = block["enabled"].GetBool();
    return ReadDoubleArrayField(block, "dom_coeff", &out->dom_coeff) &&
           ReadDoubleVector(block, "dme_coeff", &out->dme_coeff) &&
           ReadDoubleArrayField(block, "lwd_coeff", &out->lwd_coeff) &&
           ReadDoubleArrayField(block, "dom_center", &out->dom_center) &&
           ReadDoubleVector(block, "dme_center", &out->dme_center) &&
           ReadDoubleArrayField(block, "lwd_center", &out->lwd_center);
}

bool ReadEventBlock(const rapidjson::Value& obj, EventBlock* out) {
    if (!out) return false;
    *out = EventBlock{};
    if (!obj.HasMember("event_block")) return true;
    if (!obj["event_block"].IsObject()) return false;
    const auto& block = obj["event_block"];
    if (!block.HasMember("enabled") || !block["enabled"].IsBool()) return false;
    out->enabled = block["enabled"].GetBool();
    return ReadString(block, "calendar_id", &out->calendar_id) &&
           ReadString(block, "calendar_version", &out->calendar_version) &&
           ReadStringVector(block, "active_event_codes", &out->active_event_codes) &&
           ReadDoubleVector(block, "coeff", &out->coeff);
}

void ReadCoverage(const rapidjson::Value& obj, BootstrapCoverageReport* out) {
    if (!out || !obj.HasMember("train_coverage") || !obj["train_coverage"].IsObject()) {
        return;
    }
    const auto& coverage = obj["train_coverage"];
    if (coverage.HasMember("accepted_count") && coverage["accepted_count"].IsUint64()) {
        out->accepted_count = coverage["accepted_count"].GetUint64();
    }
    if (coverage.HasMember("rejected_count") && coverage["rejected_count"].IsUint64()) {
        out->rejected_count = coverage["rejected_count"].GetUint64();
    }
    if (coverage.HasMember("train_start_bucket") && coverage["train_start_bucket"].IsInt64()) {
        out->train_start_bucket = coverage["train_start_bucket"].GetInt64();
    }
    if (coverage.HasMember("train_end_bucket") && coverage["train_end_bucket"].IsInt64()) {
        out->train_end_bucket = coverage["train_end_bucket"].GetInt64();
    }
    if (coverage.HasMember("coverage_ratio") && coverage["coverage_ratio"].IsNumber()) {
        out->coverage_ratio = coverage["coverage_ratio"].GetDouble();
    }
}

struct ValueBucketAggregate {
    double weighted_value_sum = 0.0;
    double weight_sum = 0.0;
    uint64_t sample_count = 0;
};

ValueReplaySeries BuildNormalizedValueReplay(const ValueBootstrapInput& input,
                                             const ValueFeatureProfile& profile,
                                             uint64_t* rejected_count) {
    if (rejected_count) *rejected_count = 0;
    std::unordered_map<int64_t, ValueBucketAggregate> by_bucket;
    by_bucket.reserve(input.observations.size());
    for (const auto& point : input.observations) {
        if (!IsValidValueObservation(point.value) || (profile.is_sampled && point.sample_count == 0)) {
            if (rejected_count) ++(*rejected_count);
            continue;
        }
        auto& aggregate = by_bucket[point.bucket_id];
        const double weight =
            point.sample_count > 0 ? static_cast<double>(point.sample_count) : 1.0;
        aggregate.weighted_value_sum += point.value * weight;
        aggregate.weight_sum += weight;
        aggregate.sample_count += point.sample_count;
    }

    ValueReplaySeries replay;
    replay.key = input.series_key;
    replay.points.reserve(by_bucket.size());
    for (const auto& entry : by_bucket) {
        const auto& aggregate = entry.second;
        if (aggregate.weight_sum <= 0.0) {
            if (rejected_count) ++(*rejected_count);
            continue;
        }
        ValueReplayPoint point;
        point.bucket_id = entry.first;
        point.value = aggregate.weighted_value_sum / aggregate.weight_sum;
        point.sample_count = aggregate.sample_count;
        if (!IsValidValueObservation(point.value) || (profile.is_sampled && point.sample_count < profile.n_train_min)) {
            if (rejected_count) ++(*rejected_count);
            continue;
        }
        replay.points.push_back(point);
    }
    std::sort(replay.points.begin(),
              replay.points.end(),
              [](const ValueReplayPoint& lhs, const ValueReplayPoint& rhs) {
                  return lhs.bucket_id < rhs.bucket_id;
              });
    if (!replay.points.empty()) {
        replay.window = BuildWindowSummary(replay.points.size(),
                                           replay.points.front().bucket_id,
                                           replay.points.back().bucket_id);
    }
    return replay;
}

struct RatioBucketAggregate {
    double numerator = 0.0;
    double denominator = 0.0;
};

RatioReplaySeries BuildNormalizedRatioReplay(const RatioBootstrapInput& input,
                                             const RatioFeatureProfile& profile,
                                             uint64_t* rejected_count) {
    if (rejected_count) *rejected_count = 0;
    std::unordered_map<int64_t, RatioBucketAggregate> by_bucket;
    by_bucket.reserve(input.observations.size());
    for (const auto& point : input.observations) {
        if (!IsValidRatioObservation(point.numerator, point.denominator)) {
            if (rejected_count) ++(*rejected_count);
            continue;
        }
        auto& aggregate = by_bucket[point.bucket_id];
        aggregate.numerator += point.numerator;
        aggregate.denominator += point.denominator;
    }

    RatioReplaySeries replay;
    replay.key = input.series_key;
    replay.points.reserve(by_bucket.size());
    for (const auto& entry : by_bucket) {
        const auto& aggregate = entry.second;
        if (!IsValidRatioObservation(aggregate.numerator, aggregate.denominator) ||
            aggregate.denominator < static_cast<double>(profile.d_min_train)) {
            if (rejected_count) ++(*rejected_count);
            continue;
        }
        replay.points.push_back(
            RatioReplayPoint{entry.first, aggregate.numerator, aggregate.denominator});
    }
    std::sort(replay.points.begin(),
              replay.points.end(),
              [](const RatioReplayPoint& lhs, const RatioReplayPoint& rhs) {
                  return lhs.bucket_id < rhs.bucket_id;
              });
    if (!replay.points.empty()) {
        replay.window = BuildWindowSummary(replay.points.size(),
                                           replay.points.front().bucket_id,
                                           replay.points.back().bucket_id);
    }
    return replay;
}

struct RelationMetricBucketAggregate {
    double total = 0.0;
    std::unordered_map<uint32_t, double> values_by_group;
};

struct RelationBucketAggregate {
    std::vector<RelationMetricBucketAggregate> metrics;
};

std::vector<RelationBootstrapBlock> BuildNormalizedRelationBlocks(
    const RelationBootstrapInput& input,
    const std::vector<std::string>& metric_names,
    uint64_t* rejected_count) {
    if (rejected_count) *rejected_count = 0;
    const std::size_t metric_count = metric_names.size();
    std::unordered_map<int64_t, RelationBucketAggregate> by_bucket;
    by_bucket.reserve(input.blocks.size());

    for (const auto& block : input.blocks) {
        for (std::size_t metric_index = 0; metric_index < metric_count; ++metric_index) {
            if (metric_index >= block.metrics.size()) {
                if (rejected_count) ++(*rejected_count);
                continue;
            }
            const auto& metric = block.metrics[metric_index];
            if (!IsValidRelationMetric(metric, block.group_idx.size(), metric_names[metric_index])) {
                if (rejected_count) ++(*rejected_count);
                continue;
            }

            auto& bucket = by_bucket[block.bucket_id];
            if (bucket.metrics.empty()) bucket.metrics.resize(metric_count);
            auto& aggregate = bucket.metrics[metric_index];
            aggregate.total += metric.total;
            for (std::size_t group_pos = 0; group_pos < block.group_idx.size(); ++group_pos) {
                const double mass = metric.values_by_group[group_pos];
                if (mass <= 0.0) continue;
                aggregate.values_by_group[block.group_idx[group_pos]] += mass;
            }
        }
    }

    std::vector<int64_t> bucket_ids;
    bucket_ids.reserve(by_bucket.size());
    for (const auto& entry : by_bucket) bucket_ids.push_back(entry.first);
    std::sort(bucket_ids.begin(), bucket_ids.end());

    std::vector<RelationBootstrapBlock> blocks;
    blocks.reserve(bucket_ids.size());
    for (int64_t bucket_id : bucket_ids) {
        const auto bucket_it = by_bucket.find(bucket_id);
        if (bucket_it == by_bucket.end()) continue;
        const auto& aggregate = bucket_it->second;

        std::unordered_set<uint32_t> group_set;
        for (const auto& metric : aggregate.metrics) {
            for (const auto& group_entry : metric.values_by_group) {
                if (group_entry.second > 0.0) group_set.insert(group_entry.first);
            }
        }
        if (group_set.empty()) continue;

        RelationBootstrapBlock block;
        block.bucket_id = bucket_id;
        block.group_idx.assign(group_set.begin(), group_set.end());
        std::sort(block.group_idx.begin(), block.group_idx.end());
        block.metrics.reserve(metric_count);
        bool has_valid_metric = false;
        for (std::size_t metric_index = 0; metric_index < metric_count; ++metric_index) {
            RelationBootstrapMetric metric;
            metric.metric = metric_names[metric_index];
            if (metric_index < aggregate.metrics.size()) {
                const auto& metric_aggregate = aggregate.metrics[metric_index];
                metric.total = metric_aggregate.total;
                metric.values_by_group.reserve(block.group_idx.size());
                for (uint32_t group_idx : block.group_idx) {
                    const auto value_it = metric_aggregate.values_by_group.find(group_idx);
                    const double value = value_it == metric_aggregate.values_by_group.end()
                                             ? 0.0
                                             : value_it->second;
                    if (value > 0.0) ++metric.active_count;
                    metric.values_by_group.push_back(value);
                }
                if (metric_aggregate.total != 0.0 &&
                    !IsValidRelationMetric(metric, block.group_idx.size(), metric_names[metric_index])) {
                    if (rejected_count) ++(*rejected_count);
                    metric.total = 0.0;
                    metric.active_count = 0;
                    std::fill(metric.values_by_group.begin(), metric.values_by_group.end(), 0.0);
                }
            }
            has_valid_metric = has_valid_metric || metric.total > 0.0;
            block.metrics.push_back(std::move(metric));
        }
        if (has_valid_metric) blocks.push_back(std::move(block));
    }
    return blocks;
}

struct RoutedValueSeries {
    std::string metric_name;
    std::string summary_name;
    ValueBootstrapInput input;
};

struct RoutedRatioSeries {
    std::string metric_name;
    std::string summary_name;
    RatioBootstrapInput input;
};

ValueBootstrapInput& RoutedValueInput(std::vector<RoutedValueSeries>* series,
                                      const std::string& metric_name,
                                      const std::string& summary_name,
                                      const std::string& series_key) {
    for (auto& item : *series) {
        if (item.metric_name == metric_name && item.summary_name == summary_name) {
            return item.input;
        }
    }
    RoutedValueSeries item;
    item.metric_name = metric_name;
    item.summary_name = summary_name;
    item.input.series_key = series_key;
    series->push_back(std::move(item));
    return series->back().input;
}

RatioBootstrapInput& RoutedRatioInput(std::vector<RoutedRatioSeries>* series,
                                      const std::string& metric_name,
                                      const std::string& summary_name,
                                      const std::string& series_key) {
    for (auto& item : *series) {
        if (item.metric_name == metric_name && item.summary_name == summary_name) {
            return item.input;
        }
    }
    RoutedRatioSeries item;
    item.metric_name = metric_name;
    item.summary_name = summary_name;
    item.input.series_key = series_key;
    series->push_back(std::move(item));
    return series->back().input;
}

void WriteUintVector(rapidjson::Writer<rapidjson::StringBuffer>* writer,
                     const char* name,
                     const std::vector<uint32_t>& values) {
    writer->Key(name);
    writer->StartArray();
    for (uint32_t value : values) writer->Uint(value);
    writer->EndArray();
}

BootstrapRelationBasisSeed MakeRelationBasisSeed(const RelationServiceBasis& basis) {
    BootstrapRelationBasisSeed seed;
    seed.basis_version = basis.basis_version;
    seed.feature_base = basis.feature_base;
    seed.metric_name = basis.metric_name;
    seed.group_space_id = basis.group_space_id;
    seed.group_space_version = basis.group_space_version;
    seed.k_head = basis.k_head;
    seed.other_group_idxs = basis.other_group_idxs;
    seed.support_explicit = basis.support_explicit;
    seed.stable_head = basis.stable_head;
    seed.head_proto_q = basis.head_proto_q;
    return seed;
}

uint64_t FindRelationBasisVersion(const std::vector<RelationServiceBasis>& bases,
                                  const std::string& metric_name) {
    for (const auto& basis : bases) {
        if (basis.metric_name == metric_name) return basis.basis_version;
    }
    return 0;
}

void NormalizeRoutedSummaryScope(const std::vector<RelationServiceBasis>& bases,
                                 RelationRoutedBootstrapArtifact* artifact) {
    if (!artifact) return;
    artifact->basis_scoped = IsBasisScopedRelationSummary(artifact->summary_name);
    if (!artifact->basis_scoped) {
        artifact->basis_version = 0;
        return;
    }
    if (artifact->basis_version == 0) {
        artifact->basis_version = FindRelationBasisVersion(bases, artifact->metric_name);
    }
}

BaselineRelationFusionConfig CurrentRelationFusionConfig() {
    BaselineRollingConfig rolling_config;
    (void)TryGetBaselineRollingConfigOverride(&rolling_config);
    return rolling_config.relation_rolling.relation_fusion;
}

bool HasRelationFusionMetadata(const RelationFusionBootstrapMetadata& metadata) {
    return !metadata.feature_base.empty() ||
           !metadata.summary_metadata.empty() ||
           !metadata.pattern_metadata.empty();
}

std::vector<std::string> UniqueSortedMetrics(std::vector<std::string> metrics) {
    std::sort(metrics.begin(), metrics.end());
    metrics.erase(std::unique(metrics.begin(), metrics.end()), metrics.end());
    return metrics;
}

RelationFusionPatternMetadata MakeRelationFusionPatternMetadata(
    const std::string& pattern,
    double pattern_weight,
    std::vector<std::string> required_summaries,
    std::vector<std::string> optional_summaries,
    std::vector<std::string> oppose_summaries,
    const std::vector<std::string>& metrics) {
    RelationFusionPatternMetadata metadata;
    metadata.pattern = pattern;
    metadata.scope = "relation_local";
    metadata.pattern_weight = pattern_weight;
    metadata.required_summaries = std::move(required_summaries);
    metadata.optional_summaries = std::move(optional_summaries);
    metadata.oppose_summaries = std::move(oppose_summaries);
    metadata.metrics = metrics;
    return metadata;
}

std::vector<RelationFusionPatternMetadata> MakeRelationFusionPatternMetadata(
    const std::vector<std::string>& metrics) {
    const BaselineRelationFusionConfig config = CurrentRelationFusionConfig();
    std::vector<RelationFusionPatternMetadata> patterns;
    patterns.push_back(MakeRelationFusionPatternMetadata(
        "support_escape",
        config.basic_pattern_weight,
        {"out_of_support_share:up"},
        {"entropy_shannon:up", "distinct_group_count:up", "stable_headk_coverage:down"},
        {"top1_share:up", "headk_share:up", "entropy_shannon:down"},
        metrics));
    patterns.push_back(MakeRelationFusionPatternMetadata(
        "head_concentration",
        config.basic_pattern_weight,
        {"top1_share:up"},
        {"headk_share:up", "entropy_shannon:down"},
        {"out_of_support_share:up", "entropy_shannon:up"},
        metrics));
    patterns.push_back(MakeRelationFusionPatternMetadata(
        "legacy_head_dilution",
        config.stable_head_pattern_weight,
        {"stable_headk_coverage:down"},
        {"out_of_support_share:up", "entropy_shannon:up"},
        {"stable_headk_coverage:up"},
        metrics));
    patterns.push_back(MakeRelationFusionPatternMetadata(
        "stable_head_mix_shift",
        config.stable_head_pattern_weight,
        {"stable_headk_mix_drift:up"},
        {},
        {"stable_headk_coverage:down", "out_of_support_share:up"},
        metrics));
    return patterns;
}

RelationFusionBootstrapMetadata MakeRelationFusionMetadata(
    const std::string& fallback_feature_base,
    const std::vector<RelationServiceBasis>& bases,
    const std::vector<RelationRoutedBootstrapArtifact>& routed_artifacts) {
    RelationFusionBootstrapMetadata metadata;
    metadata.metadata_version = 1;
    metadata.feature_base = fallback_feature_base;
    std::vector<std::string> metrics;
    metrics.reserve(bases.size() + routed_artifacts.size());
    for (const auto& basis : bases) {
        if (metadata.feature_base.empty()) metadata.feature_base = basis.feature_base;
        if (!basis.metric_name.empty()) metrics.push_back(basis.metric_name);
    }
    for (const auto& routed : routed_artifacts) {
        RelationFusionSummaryMetadata summary;
        summary.metric_name = routed.metric_name;
        summary.summary_name = routed.summary_name;
        summary.task_kind = routed.task_kind;
        summary.basis_scoped = routed.basis_scoped;
        summary.basis_version = routed.basis_scoped ? routed.basis_version : 0;
        metadata.summary_metadata.push_back(std::move(summary));
        if (!routed.metric_name.empty()) metrics.push_back(routed.metric_name);
    }
    metadata.pattern_metadata = MakeRelationFusionPatternMetadata(UniqueSortedMetrics(metrics));
    return metadata;
}

RelationFusionBootstrapMetadata MakeRelationFusionMetadata(
    const BootstrapArtifact& artifact) {
    return MakeRelationFusionMetadata(artifact.task_identity.feature_id,
                                      artifact.relation_basis_by_metric,
                                      artifact.relation_routed_summary_artifacts);
}

template <typename BasisT>
void WriteRelationBasis(rapidjson::Writer<rapidjson::StringBuffer>* writer,
                        const BasisT& basis) {
    writer->StartObject();
    WriteStringField(writer, "metric", basis.metric_name);
    writer->Key("basis_version");
    writer->Uint64(basis.basis_version);
    WriteStringField(writer, "feature_base", basis.feature_base);
    WriteStringField(writer, "group_space_id", basis.group_space_id);
    WriteStringField(writer, "group_space_version", basis.group_space_version);
    writer->Key("k_head");
    writer->Int(basis.k_head);
    WriteUintVector(writer, "support_explicit", basis.support_explicit);
    WriteUintVector(writer, "stable_head", basis.stable_head);
    WriteDoubleVector(writer, "head_proto_q", basis.head_proto_q);
    writer->EndObject();
}

void WriteRoutedSummaryArtifact(rapidjson::Writer<rapidjson::StringBuffer>* writer,
                                const RelationRoutedBootstrapArtifact& artifact) {
    writer->StartObject();
    WriteStringField(writer, "metric", artifact.metric_name);
    WriteStringField(writer, "summary", artifact.summary_name);
    WriteStringField(writer, "task_kind", TaskKindName(artifact.task_kind));
    writer->Key("basis_version");
    writer->Uint64(artifact.basis_version);
    writer->Key("basis_scoped");
    writer->Bool(artifact.basis_scoped);
    WriteTaskIdentity(writer, artifact.task_identity);
    WriteClockSpec(writer, artifact.clock_spec);
    WriteCalendarRef(writer, artifact.calendar_ref);
    WriteCoverage(writer, artifact.coverage_report);
    WriteStringVector(writer, "seeded_components", artifact.seeded_components);
    WriteStringVector(writer, "enabled_components", artifact.enabled_components);
    if (artifact.value_model || artifact.ratio_model) {
        writer->Key("model");
        writer->StartObject();
        if (artifact.value_model) {
            WriteValueModel(writer, *artifact.value_model);
        } else if (artifact.ratio_model) {
            WriteRatioModel(writer, *artifact.ratio_model);
        }
        writer->EndObject();
    }
    writer->EndObject();
}

void WriteRoutedSummarySeed(rapidjson::Writer<rapidjson::StringBuffer>* writer,
                            const RelationRoutedBootstrapSeed& seed) {
    writer->StartObject();
    WriteStringField(writer, "metric", seed.metric_name);
    WriteStringField(writer, "summary", seed.summary_name);
    WriteStringField(writer, "task_kind", TaskKindName(seed.task_kind));
    writer->Key("basis_version");
    writer->Uint64(seed.basis_version);
    writer->Key("basis_scoped");
    writer->Bool(seed.basis_scoped);
    writer->Key("seed_status");
    writer->String(SeedStatusName(seed.seed_status));
    WriteTaskIdentity(writer, seed.task_identity);
    WriteClockSpec(writer, seed.clock_spec);
    WriteCalendarRef(writer, seed.calendar_ref);
    WriteCoverage(writer, seed.coverage_report);
    WriteStringVector(writer, "seeded_components", seed.seeded_components);
    WriteStringVector(writer, "enabled_components", seed.enabled_components);
    WriteThetaInit(writer, seed.theta_init);
    WriteMonthPosHint(writer, seed.monthpos_hint);
    WriteEventHint(writer, seed.event_hint);
    WriteSigmaInit(writer, seed.sigma_init);
    WriteRatioPriorInit(writer, seed.ratio_prior_init);
    WriteUncertaintyInit(writer, seed.uncertainty_init);
    WriteMaturityInit(writer, seed.maturity_init);
    writer->EndObject();
}

void WriteFusionSummaryMetadata(rapidjson::Writer<rapidjson::StringBuffer>* writer,
                                const RelationFusionSummaryMetadata& metadata) {
    writer->StartObject();
    WriteStringField(writer, "metric_name", metadata.metric_name);
    WriteStringField(writer, "summary_name", metadata.summary_name);
    WriteStringField(writer, "task_kind", TaskKindName(metadata.task_kind));
    writer->Key("basis_scoped");
    writer->Bool(metadata.basis_scoped);
    writer->Key("basis_version");
    writer->Uint64(metadata.basis_version);
    writer->EndObject();
}

void WriteFusionPatternMetadata(rapidjson::Writer<rapidjson::StringBuffer>* writer,
                                const RelationFusionPatternMetadata& metadata) {
    writer->StartObject();
    WriteStringField(writer, "pattern", metadata.pattern);
    WriteStringField(writer, "scope", metadata.scope);
    writer->Key("pattern_weight");
    writer->Double(metadata.pattern_weight);
    WriteStringVector(writer, "required_summaries", metadata.required_summaries);
    WriteStringVector(writer, "optional_summaries", metadata.optional_summaries);
    WriteStringVector(writer, "oppose_summaries", metadata.oppose_summaries);
    WriteStringVector(writer, "metrics", metadata.metrics);
    writer->EndObject();
}

void WriteRelationFusionMetadata(rapidjson::Writer<rapidjson::StringBuffer>* writer,
                                 const RelationFusionBootstrapMetadata& metadata) {
    writer->Key("relation_fusion_metadata");
    writer->StartObject();
    writer->Key("metadata_version");
    writer->Uint(metadata.metadata_version);
    WriteStringField(writer, "feature_base", metadata.feature_base);
    writer->Key("summary_metadata");
    writer->StartArray();
    for (const auto& summary : metadata.summary_metadata) {
        WriteFusionSummaryMetadata(writer, summary);
    }
    writer->EndArray();
    writer->Key("pattern_metadata");
    writer->StartArray();
    for (const auto& pattern : metadata.pattern_metadata) {
        WriteFusionPatternMetadata(writer, pattern);
    }
    writer->EndArray();
    writer->EndObject();
}

}  // namespace

BootstrapTrainResult BootstrapEngine::TrainValue(
    const BaselineTaskSpec& spec,
    const ValueBootstrapInput& input,
    BootstrapArtifact* out_artifact,
    const CompiledEventCalendar* compiled_event_calendar) const {
    if (input.series_key.empty()) {
        BootstrapTrainResult result;
        result.status = BaselineStatus::kInvalidArgument;
        result.rejected_count = input.observations.size();
        result.diagnostics = "series_key is required";
        ApplyDiagnosticsOption(input.options, &result, nullptr);
        return result;
    }
    if (out_artifact) {
        *out_artifact = BootstrapArtifact{};
        out_artifact->artifact_kind = BootstrapArtifactKind::kValue;
        out_artifact->series_key = input.series_key;
        out_artifact->task_identity = MakeIdentity(spec);
        out_artifact->clock_spec = MakeClockSpec(spec);
        out_artifact->calendar_ref = MakeCalendarRef(spec);
    }

    ValueFeatureProfile profile;
    profile.profile = spec.profile;
    profile.transform_name = "log1p";
    if (spec.feature_type == "value_basic") {
        if (spec.profile != "default") {
            BootstrapTrainResult result;
            result.status = BaselineStatus::kInvalidArgument;
            result.rejected_count = input.observations.size();
            result.diagnostics = "value_basic requires profile=default";
            ApplyDiagnosticsOption(input.options, &result, out_artifact);
            return result;
        }
    } else if (spec.feature_type == "value_sampled") {
        ValueSampledProfileConfig sampled_config;
        if (spec.profile == "default" ||
            !GetValueSampledProfileConfig(spec.profile, &sampled_config)) {
            BootstrapTrainResult result;
            result.status = BaselineStatus::kInvalidArgument;
            result.rejected_count = input.observations.size();
            result.diagnostics = "value_sampled requires a sampled value profile";
            ApplyDiagnosticsOption(input.options, &result, out_artifact);
            return result;
        }
        profile.transform_name = sampled_config.transform_name_override;
        profile.is_sampled = true;
        profile.n_train_min = sampled_config.n_train_min;
        profile.kappa_sample = sampled_config.kappa_sample();
    } else {
        BootstrapTrainResult result;
        result.status = BaselineStatus::kInvalidArgument;
        result.rejected_count = input.observations.size();
        result.diagnostics = "unsupported value feature_type";
        ApplyDiagnosticsOption(input.options, &result, out_artifact);
        return result;
    }

    uint64_t rejected_count = 0;
    ValueReplaySeries replay = BuildNormalizedValueReplay(input, profile, &rejected_count);

    if (ViolatesMinObservationCount(replay.points.size(), input.options)) {
        BootstrapTrainResult result;
        result.status = BaselineStatus::kInsufficientData;
        result.accepted_count = replay.points.size();
        result.rejected_count = rejected_count;
        if (!replay.points.empty()) {
            result.train_start_bucket = replay.points.front().bucket_id;
            result.train_end_bucket = replay.points.back().bucket_id;
            result.coverage_ratio =
                BuildCoverage(result.accepted_count,
                              result.rejected_count,
                              result.train_start_bucket,
                              result.train_end_bucket)
                    .coverage_ratio;
        }
        result.diagnostics = "insufficient observations for bootstrap min_observation_count";
        if (out_artifact) {
            out_artifact->train_status = result.status;
            out_artifact->coverage_report = BuildCoverage(result.accepted_count,
                                                          result.rejected_count,
                                                          result.train_start_bucket,
                                                          result.train_end_bucket);
            out_artifact->diagnostics = result.diagnostics;
        }
        ApplyDiagnosticsOption(input.options, &result, out_artifact);
        return result;
    }

    ValueFormalTrainInput train_input;
    train_input.profile = &profile;
    train_input.replay = &replay;
    train_input.train_count = replay.points.size();
    train_input.model_version = 1;
    train_input.train_window = replay.window;
    train_input.task_spec = &spec;
    train_input.delta = spec.clock_spec.bucket_seconds;
    train_input.tz = spec.clock_spec.timezone;
    train_input.compiled_event_calendar =
        input.options.enable_event ? compiled_event_calendar : nullptr;
    train_input.enable_monthpos = input.options.enable_monthpos;
    train_input.enable_event = input.options.enable_event;

    ValueFormalTrainResult train_result;
    const FormalTrainFailureCode failure =
        FormalModelTrainer::TrainValue(train_input, &train_result);

    BootstrapTrainResult result;
    result.status = MapTrainFailure(failure);
    result.accepted_count = replay.points.size();
    result.rejected_count = rejected_count;
    if (!replay.points.empty()) {
        result.train_start_bucket = replay.points.front().bucket_id;
        result.train_end_bucket = replay.points.back().bucket_id;
        result.coverage_ratio =
            BuildCoverage(result.accepted_count,
                          result.rejected_count,
                          result.train_start_bucket,
                          result.train_end_bucket)
                .coverage_ratio;
    }
    result.seed_status = BootstrapSeedStatus::kNone;
    if (failure != FormalTrainFailureCode::kNone) {
        result.diagnostics = FormalTrainFailureCodeName(failure);
    } else {
        BootstrapCoverageReport coverage = BuildCoverage(result.accepted_count,
                                                         result.rejected_count,
                                                         result.train_start_bucket,
                                                         result.train_end_bucket);
        const BootstrapThetaInit theta =
            train_result.model
                ? MakeThetaInit(train_result.model->core_block,
                                train_result.model->transform_name,
                                train_result.model->train_start)
                : BootstrapThetaInit{};
        const BootstrapMonthPosHint monthpos_hint =
            train_result.model ? MakeMonthPosHint(train_result.model->monthpos_block)
                               : BootstrapMonthPosHint{};
        const BootstrapEventHint event_hint =
            train_result.model ? MakeEventHint(train_result.model->event_block)
                               : BootstrapEventHint{};
        result.enabled_components =
            MakeEnabledComponents(theta, coverage, MakeClockSpec(spec), &monthpos_hint, &event_hint);
        const BootstrapSigmaInit sigma =
            train_result.model ? MakeValueSigmaInit(*train_result.model) : BootstrapSigmaInit{};
        result.seed_status = EvaluateBootstrapSeedStatus(result.status,
                                                         BootstrapArtifactKind::kValue,
                                                         coverage,
                                                         MakeClockSpec(spec),
                                                         &theta,
                                                         &sigma,
                                                         result.enabled_components);
        result.confidence = train_result.model ? train_result.model->confidence_base_at_train : 0.0;
    }

    if (out_artifact) {
        out_artifact->train_status = result.status;
        out_artifact->model_version = 1;
        out_artifact->coverage_report = BuildCoverage(result.accepted_count,
                                                      result.rejected_count,
                                                      result.train_start_bucket,
                                                      result.train_end_bucket);
        if (train_result.model) {
            const BootstrapThetaInit theta =
                MakeThetaInit(train_result.model->core_block,
                              train_result.model->transform_name,
                              train_result.model->train_start);
            const BootstrapMonthPosHint monthpos_hint =
                MakeMonthPosHint(train_result.model->monthpos_block);
            const BootstrapEventHint event_hint = MakeEventHint(train_result.model->event_block);
            out_artifact->seeded_components =
                MakeSeededComponents(theta, &monthpos_hint, &event_hint);
            out_artifact->enabled_components =
                MakeEnabledComponents(theta,
                                      out_artifact->coverage_report,
                                      out_artifact->clock_spec,
                                      &monthpos_hint,
                                      &event_hint);
        }
        out_artifact->enabled_components = result.enabled_components;
        out_artifact->diagnostics = result.diagnostics;
        out_artifact->value_model = train_result.model;
    }
    ApplyDiagnosticsOption(input.options, &result, out_artifact);
    return result;
}

BootstrapTrainResult BootstrapEngine::TrainRatio(
    const BaselineTaskSpec& spec,
    const RatioBootstrapInput& input,
    BootstrapArtifact* out_artifact,
    const CompiledEventCalendar* compiled_event_calendar) const {
    if (input.series_key.empty()) {
        BootstrapTrainResult result;
        result.status = BaselineStatus::kInvalidArgument;
        result.rejected_count = input.observations.size();
        result.diagnostics = "series_key is required";
        ApplyDiagnosticsOption(input.options, &result, nullptr);
        return result;
    }
    if (out_artifact) {
        *out_artifact = BootstrapArtifact{};
        out_artifact->artifact_kind = BootstrapArtifactKind::kRatio;
        out_artifact->series_key = input.series_key;
        out_artifact->task_identity = MakeIdentity(spec);
        out_artifact->clock_spec = MakeClockSpec(spec);
        out_artifact->calendar_ref = MakeCalendarRef(spec);
    }

    RatioProfileConfig profile_config;
    const std::string profile_name = spec.profile;
    if (!GetRatioProfileConfig(profile_name, &profile_config)) {
        BootstrapTrainResult result;
        result.status = BaselineStatus::kInvalidArgument;
        result.rejected_count = input.observations.size();
        result.diagnostics = "unsupported ratio profile";
        ApplyDiagnosticsOption(input.options, &result, out_artifact);
        return result;
    }

    RatioFeatureProfile profile;
    profile.profile = profile_name;
    profile.d_min_train = profile_config.d_min_train;
    profile.d_score_min = profile_config.d_score_min();
    profile.d_shift_min = profile_config.d_shift_min();
    profile.kappa_den = profile_config.kappa_den();
    profile.s_prior = profile_config.s_prior;
    profile.phi_over = profile_config.phi_over;

    uint64_t rejected_count = 0;
    RatioReplaySeries replay = BuildNormalizedRatioReplay(input, profile, &rejected_count);

    if (ViolatesMinObservationCount(replay.points.size(), input.options)) {
        BootstrapTrainResult result;
        result.status = BaselineStatus::kInsufficientData;
        result.accepted_count = replay.points.size();
        result.rejected_count = rejected_count;
        if (!replay.points.empty()) {
            result.train_start_bucket = replay.points.front().bucket_id;
            result.train_end_bucket = replay.points.back().bucket_id;
            result.coverage_ratio =
                BuildCoverage(result.accepted_count,
                              result.rejected_count,
                              result.train_start_bucket,
                              result.train_end_bucket)
                    .coverage_ratio;
        }
        result.diagnostics = "insufficient observations for bootstrap min_observation_count";
        if (out_artifact) {
            out_artifact->train_status = result.status;
            out_artifact->coverage_report = BuildCoverage(result.accepted_count,
                                                          result.rejected_count,
                                                          result.train_start_bucket,
                                                          result.train_end_bucket);
            out_artifact->diagnostics = result.diagnostics;
        }
        ApplyDiagnosticsOption(input.options, &result, out_artifact);
        return result;
    }

    RatioFormalTrainInput train_input;
    train_input.profile = &profile;
    train_input.replay = &replay;
    train_input.train_count = replay.points.size();
    train_input.model_version = 1;
    train_input.train_window = replay.window;
    train_input.task_spec = &spec;
    train_input.delta = spec.clock_spec.bucket_seconds;
    train_input.tz = spec.clock_spec.timezone;
    train_input.compiled_event_calendar =
        input.options.enable_event ? compiled_event_calendar : nullptr;
    train_input.enable_monthpos = input.options.enable_monthpos;
    train_input.enable_event = input.options.enable_event;

    RatioFormalTrainResult train_result;
    const FormalTrainFailureCode failure =
        FormalModelTrainer::TrainRatio(train_input, &train_result);

    BootstrapTrainResult result;
    result.status = MapTrainFailure(failure);
    result.accepted_count = replay.points.size();
    result.rejected_count = rejected_count;
    if (!replay.points.empty()) {
        result.train_start_bucket = replay.points.front().bucket_id;
        result.train_end_bucket = replay.points.back().bucket_id;
        result.coverage_ratio =
            BuildCoverage(result.accepted_count,
                          result.rejected_count,
                          result.train_start_bucket,
                          result.train_end_bucket)
                .coverage_ratio;
    }
    result.seed_status = BootstrapSeedStatus::kNone;
    if (failure != FormalTrainFailureCode::kNone) {
        result.diagnostics = FormalTrainFailureCodeName(failure);
    } else {
        BootstrapCoverageReport coverage = BuildCoverage(result.accepted_count,
                                                         result.rejected_count,
                                                         result.train_start_bucket,
                                                         result.train_end_bucket);
        const BootstrapThetaInit theta =
            train_result.model
                ? MakeThetaInit(train_result.model->core_block,
                                train_result.model->transform_name,
                                train_result.model->train_start)
                : BootstrapThetaInit{};
        const BootstrapMonthPosHint monthpos_hint =
            train_result.model ? MakeMonthPosHint(train_result.model->monthpos_block)
                               : BootstrapMonthPosHint{};
        const BootstrapEventHint event_hint =
            train_result.model ? MakeEventHint(train_result.model->event_block)
                               : BootstrapEventHint{};
        result.enabled_components =
            MakeEnabledComponents(theta, coverage, MakeClockSpec(spec), &monthpos_hint, &event_hint);
        const BootstrapSigmaInit sigma =
            train_result.model ? MakeRatioSigmaInit(*train_result.model, coverage)
                               : BootstrapSigmaInit{};
        result.seed_status = EvaluateBootstrapSeedStatus(result.status,
                                                         BootstrapArtifactKind::kRatio,
                                                         coverage,
                                                         MakeClockSpec(spec),
                                                         &theta,
                                                         &sigma,
                                                         result.enabled_components);
        result.confidence = train_result.model ? train_result.model->confidence_base_at_train : 0.0;
    }

    if (out_artifact) {
        out_artifact->train_status = result.status;
        out_artifact->model_version = 1;
        out_artifact->coverage_report = BuildCoverage(result.accepted_count,
                                                      result.rejected_count,
                                                      result.train_start_bucket,
                                                      result.train_end_bucket);
        if (train_result.model) {
            const BootstrapThetaInit theta =
                MakeThetaInit(train_result.model->core_block,
                              train_result.model->transform_name,
                              train_result.model->train_start);
            const BootstrapMonthPosHint monthpos_hint =
                MakeMonthPosHint(train_result.model->monthpos_block);
            const BootstrapEventHint event_hint = MakeEventHint(train_result.model->event_block);
            out_artifact->seeded_components =
                MakeSeededComponents(theta, &monthpos_hint, &event_hint);
            out_artifact->enabled_components =
                MakeEnabledComponents(theta,
                                      out_artifact->coverage_report,
                                      out_artifact->clock_spec,
                                      &monthpos_hint,
                                      &event_hint);
        }
        out_artifact->enabled_components = result.enabled_components;
        out_artifact->diagnostics = result.diagnostics;
        out_artifact->ratio_model = train_result.model;
    }
    ApplyDiagnosticsOption(input.options, &result, out_artifact);
    return result;
}

BootstrapTrainResult BootstrapEngine::TrainRelation(
    const RelationTaskCreateSpec& spec,
    const RelationBootstrapInput& input,
    BootstrapArtifact* out_artifact,
    const CompiledEventCalendar* compiled_event_calendar) const {
    if (input.series_key.empty()) {
        BootstrapTrainResult result;
        result.status = BaselineStatus::kInvalidArgument;
        result.rejected_count = input.blocks.size();
        result.diagnostics = "series_key is required";
        ApplyDiagnosticsOption(input.options, &result, nullptr);
        return result;
    }
    if (out_artifact) {
        *out_artifact = BootstrapArtifact{};
        out_artifact->artifact_kind = BootstrapArtifactKind::kRelation;
        out_artifact->series_key = input.series_key;
        out_artifact->task_identity.task_id = spec.task_spec.task_id;
        out_artifact->task_identity.task_kind = spec.task_spec.task_kind;
        out_artifact->task_identity.feature_type = "relation";
        out_artifact->task_identity.feature_id = spec.task_spec.feature_id;
        out_artifact->task_identity.profile = spec.task_spec.profile;
        out_artifact->clock_spec = MakeClockSpec(spec);
        out_artifact->calendar_ref = MakeCalendarRef(spec);
    }

    const std::size_t metric_count = spec.task_spec.metrics.size();
    if (metric_count == 0) {
        BootstrapTrainResult result;
        result.status = BaselineStatus::kInvalidArgument;
        result.diagnostics = "relation task must define metrics";
        ApplyDiagnosticsOption(input.options, &result, out_artifact);
        return result;
    }

    uint64_t rejected_count = 0;
    const std::vector<RelationBootstrapBlock> normalized_blocks =
        BuildNormalizedRelationBlocks(input, spec.task_spec.metrics, &rejected_count);
    const int64_t train_start_bucket =
        normalized_blocks.empty() ? 0 : normalized_blocks.front().bucket_id;
    const int64_t train_end_bucket =
        normalized_blocks.empty() ? 0 : normalized_blocks.back().bucket_id;
    if (normalized_blocks.empty() ||
        ViolatesMinObservationCount(normalized_blocks.size(), input.options)) {
        BootstrapTrainResult result;
        result.status = BaselineStatus::kInsufficientData;
        result.accepted_count = normalized_blocks.size();
        result.rejected_count = rejected_count;
        result.train_start_bucket = train_start_bucket;
        result.train_end_bucket = train_end_bucket;
        result.coverage_ratio =
            BuildCoverage(result.accepted_count,
                          result.rejected_count,
                          result.train_start_bucket,
                          result.train_end_bucket)
                .coverage_ratio;
        result.diagnostics = normalized_blocks.empty()
                                 ? "relation bootstrap has no valid normalized buckets"
                                 : "insufficient observations for bootstrap min_observation_count";
        if (out_artifact) {
            out_artifact->train_status = result.status;
            out_artifact->coverage_report = BuildCoverage(result.accepted_count,
                                                          result.rejected_count,
                                                          result.train_start_bucket,
                                                          result.train_end_bucket);
            out_artifact->diagnostics = result.diagnostics;
        }
        ApplyDiagnosticsOption(input.options, &result, out_artifact);
        return result;
    }

    std::vector<std::unordered_map<uint32_t, RelationGroupHistoryStat>> stats_by_metric(
        metric_count);
    std::vector<uint64_t> valid_bucket_count(metric_count, 0);

    for (const auto& block : normalized_blocks) {
        for (std::size_t metric_index = 0; metric_index < metric_count; ++metric_index) {
            if (metric_index >= block.metrics.size()) continue;
            const auto& metric = block.metrics[metric_index];
            if (metric.total <= 0.0 ||
                metric.values_by_group.size() < block.group_idx.size()) continue;
            ++valid_bucket_count[metric_index];
            for (std::size_t group_pos = 0; group_pos < block.group_idx.size(); ++group_pos) {
                const double mass = metric.values_by_group[group_pos];
                if (!(mass > 0.0)) continue;
                auto& stat = stats_by_metric[metric_index][block.group_idx[group_pos]];
                stat.group_idx = block.group_idx[group_pos];
                stat.hist_mass += mass;
                stat.active_bucket_count += 1;
            }
        }
    }

    std::vector<RelationServiceBasis> bases;
    bases.reserve(metric_count);
    for (std::size_t metric_index = 0; metric_index < metric_count; ++metric_index) {
        RelationBasisBuildInput build_input;
        build_input.basis_version = 1;
        build_input.feature_base = spec.task_spec.feature_base.empty()
                                       ? spec.task_spec.feature_id
                                       : spec.task_spec.feature_base;
        build_input.metric_name = spec.task_spec.metrics[metric_index];
        build_input.group_space_id = spec.task_spec.group_space_id;
        build_input.group_space_version = spec.task_spec.group_space_version.value_or("");
        build_input.other_group_idxs = spec.task_spec.other_group_idxs;
        build_input.support_policy = spec.task_spec.support_policy;
        build_input.summary_policy = spec.task_spec.summary_policy;
        build_input.valid_bucket_count = valid_bucket_count[metric_index];
        build_input.group_stats.reserve(stats_by_metric[metric_index].size());
        for (const auto& entry : stats_by_metric[metric_index]) {
            build_input.group_stats.push_back(entry.second);
        }

        RelationServiceBasis basis;
        if (RelationBasisBuilder::BuildServiceBasis(build_input, &basis) != error::OK) {
            BootstrapTrainResult result;
            result.status = BaselineStatus::kTrainFailed;
            result.accepted_count = normalized_blocks.size();
            result.rejected_count = rejected_count;
            result.train_start_bucket = train_start_bucket;
            result.train_end_bucket = train_end_bucket;
            result.coverage_ratio =
                BuildCoverage(result.accepted_count,
                              result.rejected_count,
                              result.train_start_bucket,
                              result.train_end_bucket)
                    .coverage_ratio;
            result.diagnostics = "relation basis build failed";
            if (out_artifact) {
                out_artifact->train_status = result.status;
                out_artifact->coverage_report = BuildCoverage(result.accepted_count,
                                                              result.rejected_count,
                                                              result.train_start_bucket,
                                                              result.train_end_bucket);
                out_artifact->diagnostics = result.diagnostics;
            }
            ApplyDiagnosticsOption(input.options, &result, out_artifact);
            return result;
        }
        bases.push_back(std::move(basis));
    }

    std::vector<RoutedValueSeries> routed_value_series;
    std::vector<RoutedRatioSeries> routed_ratio_series;
    for (const auto& block : normalized_blocks) {
        for (std::size_t metric_index = 0; metric_index < metric_count; ++metric_index) {
            RelationSummaryProjectionOptions summary_options;
            summary_options.summary_policy = spec.task_spec.summary_policy;
            summary_options.other_group_idxs = spec.task_spec.other_group_idxs;
            summary_options.basis = &bases[metric_index];
            std::vector<RelationProjectedSummary> summaries;
            if (!ProjectRelationMetricSummaries(block,
                                                metric_index,
                                                bases[metric_index].metric_name,
                                                summary_options,
                                                &summaries)) {
                continue;
            }

            const std::string& metric_name = bases[metric_index].metric_name;
            for (const RelationProjectedSummary& summary : summaries) {
                if (summary.task_kind == BaselineTaskKind::kValue) {
                    RoutedValueInput(&routed_value_series,
                                     metric_name,
                                     summary.summary_name,
                                     input.series_key)
                        .observations.push_back(
                            ValueBootstrapPoint{block.bucket_id, summary.value, 1});
                    continue;
                }
                if (summary.task_kind != BaselineTaskKind::kRatio ||
                    summary.denominator <= 0.0) {
                    continue;
                }
                RoutedRatioInput(&routed_ratio_series,
                                 metric_name,
                                 summary.summary_name,
                                 input.series_key)
                    .observations.push_back(RatioBootstrapPoint{
                        block.bucket_id, summary.numerator, summary.denominator});
            }
        }
    }

    for (auto& series : routed_value_series) {
        series.input.options = input.options;
    }
    for (auto& series : routed_ratio_series) {
        series.input.options = input.options;
    }

    std::vector<RelationRoutedBootstrapArtifact> routed_artifacts;
    for (const auto& series : routed_value_series) {
        const BaselineTaskSpec routed_spec = MakeRoutedSummaryTaskSpec(
            spec, series.metric_name, series.summary_name, BaselineTaskKind::kValue);
        BootstrapArtifact routed_artifact;
        const BootstrapTrainResult routed_result =
            TrainValue(routed_spec, series.input, &routed_artifact, compiled_event_calendar);
        if (routed_result.status != BaselineStatus::kOk || !routed_artifact.value_model) {
            continue;
        }

        RelationRoutedBootstrapArtifact item;
        item.metric_name = series.metric_name;
        item.summary_name = series.summary_name;
        item.task_kind = BaselineTaskKind::kValue;
        item.task_identity = routed_artifact.task_identity;
        item.clock_spec = routed_artifact.clock_spec;
        item.calendar_ref = routed_artifact.calendar_ref;
        item.coverage_report = routed_artifact.coverage_report;
        item.seeded_components = routed_artifact.seeded_components;
        item.enabled_components = routed_artifact.enabled_components;
        item.value_model = routed_artifact.value_model;
        item.diagnostics = routed_artifact.diagnostics;
        NormalizeRoutedSummaryScope(bases, &item);
        routed_artifacts.push_back(std::move(item));
    }
    for (const auto& series : routed_ratio_series) {
        const BaselineTaskSpec routed_spec = MakeRoutedSummaryTaskSpec(
            spec, series.metric_name, series.summary_name, BaselineTaskKind::kRatio);
        BootstrapArtifact routed_artifact;
        const BootstrapTrainResult routed_result =
            TrainRatio(routed_spec, series.input, &routed_artifact, compiled_event_calendar);
        if (routed_result.status != BaselineStatus::kOk || !routed_artifact.ratio_model) {
            continue;
        }

        RelationRoutedBootstrapArtifact item;
        item.metric_name = series.metric_name;
        item.summary_name = series.summary_name;
        item.task_kind = BaselineTaskKind::kRatio;
        item.task_identity = routed_artifact.task_identity;
        item.clock_spec = routed_artifact.clock_spec;
        item.calendar_ref = routed_artifact.calendar_ref;
        item.coverage_report = routed_artifact.coverage_report;
        item.seeded_components = routed_artifact.seeded_components;
        item.enabled_components = routed_artifact.enabled_components;
        item.ratio_model = routed_artifact.ratio_model;
        item.diagnostics = routed_artifact.diagnostics;
        NormalizeRoutedSummaryScope(bases, &item);
        routed_artifacts.push_back(std::move(item));
    }

    BootstrapTrainResult result;
    result.status = BaselineStatus::kOk;
    result.accepted_count = normalized_blocks.size();
    result.rejected_count = rejected_count;
    result.train_start_bucket = train_start_bucket;
    result.train_end_bucket = train_end_bucket;
    result.coverage_ratio =
        BuildCoverage(result.accepted_count,
                      result.rejected_count,
                      result.train_start_bucket,
                      result.train_end_bucket)
            .coverage_ratio;
    result.enabled_components = {"relation_basis"};
    if (!routed_artifacts.empty()) {
        result.enabled_components.push_back("relation_routed_summaries");
    }
    result.seed_status =
        EvaluateBootstrapSeedStatus(result.status,
                                    BootstrapArtifactKind::kRelation,
                                    BuildCoverage(result.accepted_count,
                                                  result.rejected_count,
                                                  result.train_start_bucket,
                                                  result.train_end_bucket),
                                    MakeClockSpec(spec),
                                    nullptr,
                                    nullptr,
                                    result.enabled_components);

    if (out_artifact) {
        out_artifact->train_status = BaselineStatus::kOk;
        out_artifact->model_version = 1;
        out_artifact->coverage_report = BuildCoverage(result.accepted_count,
                                                      result.rejected_count,
                                                      result.train_start_bucket,
                                                      result.train_end_bucket);
        out_artifact->enabled_components = result.enabled_components;
        out_artifact->seeded_components = result.enabled_components;
        out_artifact->relation_basis_by_metric = std::move(bases);
        out_artifact->relation_routed_summary_artifacts = std::move(routed_artifacts);
        out_artifact->relation_fusion_metadata = MakeRelationFusionMetadata(*out_artifact);
    }
    ApplyDiagnosticsOption(input.options, &result, out_artifact);
    return result;
}

BootstrapPrediction BootstrapEngine::PredictValue(
    const BootstrapArtifact& artifact,
    int64_t bucket_id,
    const BootstrapPredictionOptions& options,
    const BaselineTaskSpec* task_spec,
    const CompiledEventCalendar* compiled_event_calendar) const {
    if (!artifact.value_model || artifact.train_status != BaselineStatus::kOk) {
        BootstrapPrediction prediction =
            UnavailablePrediction(bucket_id, "value bootstrap artifact is not available");
        prediction.series_key = artifact.series_key;
        ApplyPredictionDiagnosticsOption(options, &prediction);
        return prediction;
    }

    FormalPredictContext context;
    context.bucket_id = bucket_id;
    context.task_spec = task_spec;
    context.event_calendar = compiled_event_calendar;
    FormalPrediction formal_prediction;
    if (PredictFormalModel(artifact.value_model.get(), context, &formal_prediction) !=
            error::OK ||
        !formal_prediction.ready) {
        BootstrapPrediction prediction;
        prediction.status = BaselineStatus::kPredictFailed;
        prediction.series_key = artifact.series_key;
        prediction.bucket_id = bucket_id;
        prediction.diagnostics = "formal value prediction failed";
        ApplyPredictionDiagnosticsOption(options, &prediction);
        return prediction;
    }

    const double sigma = std::max(artifact.value_model->sigma_ref, 1.0e-3);
    const double z = ZValue(options.confidence_level);
    const double model_mu = formal_prediction.value;
    const double model_lower = model_mu - z * sigma;
    const double model_upper = model_mu + z * sigma;
    const bool identity_transform = artifact.value_model->transform_name == "identity";

    BootstrapPrediction prediction;
    prediction.status = BaselineStatus::kOk;
    prediction.series_key = artifact.series_key;
    prediction.bucket_id = bucket_id;
    prediction.baseline_mu = std::max(0.0, identity_transform ? model_mu : std::expm1(model_mu));
    prediction.baseline_lower = std::max(0.0, identity_transform ? model_lower : std::expm1(model_lower));
    prediction.baseline_upper =
        std::max(prediction.baseline_lower, identity_transform ? model_upper : std::expm1(model_upper));
    prediction.band_width = prediction.baseline_upper - prediction.baseline_lower;
    prediction.confidence = formal_prediction.confidence_base;
    prediction.uncertainty_source.push_back("value_sigma_ref");
    if (options.include_model_space_debug) {
        prediction.has_model_space = true;
        prediction.model_space_mu = model_mu;
        prediction.model_space_lower = model_lower;
        prediction.model_space_upper = model_upper;
    }
    return prediction;
}

BootstrapPredictionSequence BootstrapEngine::PredictValueSequence(
    const BootstrapArtifact& artifact,
    int64_t start_bucket_id,
    uint32_t point_count,
    const BootstrapPredictionOptions& options,
    const BaselineTaskSpec* task_spec,
    const CompiledEventCalendar* compiled_event_calendar) const {
    BootstrapPredictionSequence sequence;
    sequence.series_key = artifact.series_key;
    sequence.start_bucket_id = start_bucket_id;
    sequence.point_count = point_count;
    if (point_count == 0) {
        sequence.status = BaselineStatus::kInvalidArgument;
        return sequence;
    }
    if (start_bucket_id > std::numeric_limits<int64_t>::max() -
                              static_cast<int64_t>(point_count - 1)) {
        sequence.status = BaselineStatus::kInvalidArgument;
        return sequence;
    }

    sequence.predictions.reserve(point_count);
    if (!artifact.value_model || artifact.train_status != BaselineStatus::kOk) {
        sequence.status = BaselineStatus::kNotTrained;
        for (uint32_t i = 0; i < point_count; ++i) {
            BootstrapPrediction prediction = UnavailablePrediction(
                start_bucket_id + static_cast<int64_t>(i),
                "value bootstrap artifact is not available");
            prediction.series_key = artifact.series_key;
            ApplyPredictionDiagnosticsOption(options, &prediction);
            sequence.predictions.push_back(std::move(prediction));
        }
        return sequence;
    }

    FormalPredictContext context;
    context.task_spec = task_spec;
    context.event_calendar = compiled_event_calendar;
    const double sigma = std::max(artifact.value_model->sigma_ref, 1.0e-3);
    const double z = ZValue(options.confidence_level);
    const int64_t delta = ResolvePredictDelta(*artifact.value_model, task_spec);
    const std::string timezone = ResolvePredictTimezone(*artifact.value_model, task_spec);
    const BaselineRollingConfig feature_config =
        MakeFormalFeatureConfig(artifact.value_model->core_block, delta, timezone);
    const bool identity_transform = artifact.value_model->transform_name == "identity";
    const uint32_t chunk_size =
        delta > 0
            ? ComputeRollingFeatureChunkSize(feature_config.daily_harmonic_order,
                                             feature_config.weekly_harmonic_order)
            : point_count;

    for (uint32_t offset = 0; offset < point_count; offset += chunk_size) {
        const uint32_t chunk_count = std::min<uint32_t>(chunk_size, point_count - offset);
        RollingFeatureBatch batch;
        const BaselineStatus batch_status =
            delta > 0
                ? BuildRollingFeatureBatch(
                      start_bucket_id + static_cast<int64_t>(offset),
                      chunk_count,
                      feature_config,
                      &batch)
                : BaselineStatus::kInvalidArgument;
        for (uint32_t i = 0; i < chunk_count; ++i) {
            const int64_t bucket_id =
                start_bucket_id + static_cast<int64_t>(offset + i);
            context.bucket_id = bucket_id;
            FormalPrediction formal_prediction;
            const int predict_status =
                batch_status == BaselineStatus::kOk
                    ? PredictFormalModelWithFeature(artifact.value_model.get(),
                                                    context,
                                                    MakeFormalFeatureView(batch.View(i)),
                                                    &formal_prediction)
                    : PredictFormalModel(
                          artifact.value_model.get(), context, &formal_prediction);
            if (predict_status != error::OK || !formal_prediction.ready) {
                BootstrapPrediction prediction;
                prediction.status = BaselineStatus::kPredictFailed;
                prediction.series_key = artifact.series_key;
                prediction.bucket_id = bucket_id;
                prediction.diagnostics = "formal value prediction failed";
                ApplyPredictionDiagnosticsOption(options, &prediction);
                if (sequence.status == BaselineStatus::kOk) sequence.status = prediction.status;
                sequence.predictions.push_back(std::move(prediction));
                continue;
            }

            const double model_mu = formal_prediction.value;
            const double model_lower = model_mu - z * sigma;
            const double model_upper = model_mu + z * sigma;

            BootstrapPrediction prediction;
            prediction.status = BaselineStatus::kOk;
            prediction.series_key = artifact.series_key;
            prediction.bucket_id = bucket_id;
            prediction.baseline_mu = std::max(0.0, identity_transform ? model_mu : std::expm1(model_mu));
            prediction.baseline_lower = std::max(0.0, identity_transform ? model_lower : std::expm1(model_lower));
            prediction.baseline_upper =
                std::max(prediction.baseline_lower, identity_transform ? model_upper : std::expm1(model_upper));
            prediction.band_width = prediction.baseline_upper - prediction.baseline_lower;
            prediction.confidence = formal_prediction.confidence_base;
            prediction.uncertainty_source.push_back("value_sigma_ref");
            if (options.include_model_space_debug) {
                prediction.has_model_space = true;
                prediction.model_space_mu = model_mu;
                prediction.model_space_lower = model_lower;
                prediction.model_space_upper = model_upper;
            }
            sequence.predictions.push_back(std::move(prediction));
        }
    }
    return sequence;
}

BootstrapPrediction BootstrapEngine::PredictRatio(
    const BootstrapArtifact& artifact,
    int64_t bucket_id,
    const BootstrapPredictionOptions& options,
    const BaselineTaskSpec* task_spec,
    const CompiledEventCalendar* compiled_event_calendar) const {
    if (!artifact.ratio_model || artifact.train_status != BaselineStatus::kOk) {
        BootstrapPrediction prediction =
            UnavailablePrediction(bucket_id, "ratio bootstrap artifact is not available");
        prediction.series_key = artifact.series_key;
        ApplyPredictionDiagnosticsOption(options, &prediction);
        return prediction;
    }

    FormalPredictContext context;
    context.bucket_id = bucket_id;
    context.task_spec = task_spec;
    context.event_calendar = compiled_event_calendar;
    FormalPrediction formal_prediction;
    if (PredictFormalModel(artifact.ratio_model.get(), context, &formal_prediction) !=
            error::OK ||
        !formal_prediction.ready) {
        BootstrapPrediction prediction;
        prediction.status = BaselineStatus::kPredictFailed;
        prediction.series_key = artifact.series_key;
        prediction.bucket_id = bucket_id;
        prediction.diagnostics = "formal ratio prediction failed";
        ApplyPredictionDiagnosticsOption(options, &prediction);
        return prediction;
    }

    const double p = Clamp01(formal_prediction.value);
    const double effective_n = std::max<double>(
        1.0,
        static_cast<double>(artifact.coverage_report.accepted_count) +
            artifact.ratio_model->alpha0 + artifact.ratio_model->beta0);
    BootstrapPrediction prediction;
    prediction.status = BaselineStatus::kOk;
    prediction.series_key = artifact.series_key;
    prediction.bucket_id = bucket_id;
    prediction.baseline_mu = p;
    prediction.confidence = formal_prediction.confidence_base;
    ApplyRatioPredictionBand(p, effective_n, artifact.ratio_model->sigma_ref, ZValue(options.confidence_level),
                             &prediction);
    return prediction;
}

BootstrapPredictionSequence BootstrapEngine::PredictRatioSequence(
    const BootstrapArtifact& artifact,
    int64_t start_bucket_id,
    uint32_t point_count,
    const BootstrapPredictionOptions& options,
    const BaselineTaskSpec* task_spec,
    const CompiledEventCalendar* compiled_event_calendar) const {
    BootstrapPredictionSequence sequence;
    sequence.series_key = artifact.series_key;
    sequence.start_bucket_id = start_bucket_id;
    sequence.point_count = point_count;
    if (point_count == 0) {
        sequence.status = BaselineStatus::kInvalidArgument;
        return sequence;
    }
    if (start_bucket_id > std::numeric_limits<int64_t>::max() -
                              static_cast<int64_t>(point_count - 1)) {
        sequence.status = BaselineStatus::kInvalidArgument;
        return sequence;
    }

    sequence.predictions.reserve(point_count);
    if (!artifact.ratio_model || artifact.train_status != BaselineStatus::kOk) {
        sequence.status = BaselineStatus::kNotTrained;
        for (uint32_t i = 0; i < point_count; ++i) {
            BootstrapPrediction prediction = UnavailablePrediction(
                start_bucket_id + static_cast<int64_t>(i),
                "ratio bootstrap artifact is not available");
            prediction.series_key = artifact.series_key;
            ApplyPredictionDiagnosticsOption(options, &prediction);
            sequence.predictions.push_back(std::move(prediction));
        }
        return sequence;
    }

    FormalPredictContext context;
    context.task_spec = task_spec;
    context.event_calendar = compiled_event_calendar;
    const double effective_n = std::max<double>(
        1.0,
        static_cast<double>(artifact.coverage_report.accepted_count) +
            artifact.ratio_model->alpha0 + artifact.ratio_model->beta0);
    const double z = ZValue(options.confidence_level);
    const int64_t delta = ResolvePredictDelta(*artifact.ratio_model, task_spec);
    const std::string timezone = ResolvePredictTimezone(*artifact.ratio_model, task_spec);
    const BaselineRollingConfig feature_config =
        MakeFormalFeatureConfig(artifact.ratio_model->core_block, delta, timezone);
    const uint32_t chunk_size =
        delta > 0
            ? ComputeRollingFeatureChunkSize(feature_config.daily_harmonic_order,
                                             feature_config.weekly_harmonic_order)
            : point_count;

    for (uint32_t offset = 0; offset < point_count; offset += chunk_size) {
        const uint32_t chunk_count = std::min<uint32_t>(chunk_size, point_count - offset);
        RollingFeatureBatch batch;
        const BaselineStatus batch_status =
            delta > 0
                ? BuildRollingFeatureBatch(
                      start_bucket_id + static_cast<int64_t>(offset),
                      chunk_count,
                      feature_config,
                      &batch)
                : BaselineStatus::kInvalidArgument;
        for (uint32_t i = 0; i < chunk_count; ++i) {
            const int64_t bucket_id =
                start_bucket_id + static_cast<int64_t>(offset + i);
            context.bucket_id = bucket_id;
            FormalPrediction formal_prediction;
            const int predict_status =
                batch_status == BaselineStatus::kOk
                    ? PredictFormalModelWithFeature(artifact.ratio_model.get(),
                                                    context,
                                                    MakeFormalFeatureView(batch.View(i)),
                                                    &formal_prediction)
                    : PredictFormalModel(
                          artifact.ratio_model.get(), context, &formal_prediction);
            if (predict_status != error::OK || !formal_prediction.ready) {
                BootstrapPrediction prediction;
                prediction.status = BaselineStatus::kPredictFailed;
                prediction.series_key = artifact.series_key;
                prediction.bucket_id = bucket_id;
                prediction.diagnostics = "formal ratio prediction failed";
                ApplyPredictionDiagnosticsOption(options, &prediction);
                if (sequence.status == BaselineStatus::kOk) sequence.status = prediction.status;
                sequence.predictions.push_back(std::move(prediction));
                continue;
            }

            const double p = Clamp01(formal_prediction.value);
            BootstrapPrediction prediction;
            prediction.status = BaselineStatus::kOk;
            prediction.series_key = artifact.series_key;
            prediction.bucket_id = bucket_id;
            prediction.baseline_mu = p;
            prediction.confidence = formal_prediction.confidence_base;
            ApplyRatioPredictionBand(p, effective_n, artifact.ratio_model->sigma_ref, z, &prediction);
            sequence.predictions.push_back(std::move(prediction));
        }
    }
    return sequence;
}

BaselineStatus BootstrapEngine::ExportSeed(const BootstrapArtifact& artifact,
                                           BootstrapSeed* out_seed) const {
    if (!out_seed) return BaselineStatus::kInvalidArgument;
    *out_seed = BootstrapSeed{};
    out_seed->seed_status = BootstrapSeedStatus::kNone;
    out_seed->artifact_kind = artifact.artifact_kind;
    out_seed->source_artifact_version = artifact.model_version;
    out_seed->series_key = artifact.series_key;
    out_seed->task_identity = artifact.task_identity;
    out_seed->clock_spec = artifact.clock_spec;
    out_seed->calendar_ref = artifact.calendar_ref;
    out_seed->coverage_report = artifact.coverage_report;
    out_seed->seeded_components = artifact.seeded_components;
    out_seed->enabled_components = artifact.enabled_components;
    if (artifact.value_model) {
        FillValueSeedInitializers(*artifact.value_model,
                                  artifact.coverage_report,
                                  BootstrapSeedStatus::kPartial,
                                  &out_seed->theta_init,
                                  &out_seed->monthpos_hint,
                                  &out_seed->event_hint,
                                  &out_seed->sigma_init,
                                  &out_seed->ratio_prior_init,
                                  &out_seed->uncertainty_init,
                                  &out_seed->maturity_init);
        out_seed->seeded_components = MakeSeededComponents(
            out_seed->theta_init, &out_seed->monthpos_hint, &out_seed->event_hint);
        out_seed->enabled_components =
            MakeEnabledComponents(out_seed->theta_init,
                                  out_seed->coverage_report,
                                  out_seed->clock_spec,
                                  &out_seed->monthpos_hint,
                                  &out_seed->event_hint);
        out_seed->seed_status =
            EvaluateBootstrapSeedStatus(artifact.train_status,
                                        artifact.artifact_kind,
                                        out_seed->coverage_report,
                                        out_seed->clock_spec,
                                        &out_seed->theta_init,
                                        &out_seed->sigma_init,
                                        out_seed->enabled_components);
        out_seed->maturity_init.seed_status = out_seed->seed_status;
    } else if (artifact.ratio_model) {
        FillRatioSeedInitializers(*artifact.ratio_model,
                                  artifact.coverage_report,
                                  BootstrapSeedStatus::kPartial,
                                  &out_seed->theta_init,
                                  &out_seed->monthpos_hint,
                                  &out_seed->event_hint,
                                  &out_seed->sigma_init,
                                  &out_seed->ratio_prior_init,
                                  &out_seed->uncertainty_init,
                                  &out_seed->maturity_init);
        out_seed->seeded_components = MakeSeededComponents(
            out_seed->theta_init, &out_seed->monthpos_hint, &out_seed->event_hint);
        out_seed->enabled_components =
            MakeEnabledComponents(out_seed->theta_init,
                                  out_seed->coverage_report,
                                  out_seed->clock_spec,
                                  &out_seed->monthpos_hint,
                                  &out_seed->event_hint);
        out_seed->seed_status =
            EvaluateBootstrapSeedStatus(artifact.train_status,
                                        artifact.artifact_kind,
                                        out_seed->coverage_report,
                                        out_seed->clock_spec,
                                        &out_seed->theta_init,
                                        &out_seed->sigma_init,
                                        out_seed->enabled_components);
        out_seed->maturity_init.seed_status = out_seed->seed_status;
    } else {
        out_seed->seed_status =
            EvaluateBootstrapSeedStatus(artifact.train_status,
                                        artifact.artifact_kind,
                                        out_seed->coverage_report,
                                        out_seed->clock_spec,
                                        nullptr,
                                        nullptr,
                                        out_seed->enabled_components);
    }
    out_seed->relation_basis_by_metric.reserve(artifact.relation_basis_by_metric.size());
    for (const auto& basis : artifact.relation_basis_by_metric) {
        out_seed->relation_basis_by_metric.push_back(MakeRelationBasisSeed(basis));
    }
    if (artifact.artifact_kind == BootstrapArtifactKind::kRelation) {
        out_seed->relation_fusion_metadata = artifact.relation_fusion_metadata;
        if (!HasRelationFusionMetadata(out_seed->relation_fusion_metadata)) {
            out_seed->relation_fusion_metadata = MakeRelationFusionMetadata(artifact);
        }
    }
    out_seed->diagnostics = artifact.diagnostics;
    out_seed->relation_routed_summary_seeds.reserve(
        artifact.relation_routed_summary_artifacts.size());
    for (const auto& routed_artifact : artifact.relation_routed_summary_artifacts) {
        RelationRoutedBootstrapSeed routed_seed;
        routed_seed.metric_name = routed_artifact.metric_name;
        routed_seed.summary_name = routed_artifact.summary_name;
        routed_seed.task_kind = routed_artifact.task_kind;
        routed_seed.basis_scoped = IsBasisScopedRelationSummary(routed_seed.summary_name);
        routed_seed.basis_version =
            routed_seed.basis_scoped ? routed_artifact.basis_version : 0;
        routed_seed.task_identity = routed_artifact.task_identity;
        routed_seed.clock_spec = routed_artifact.clock_spec;
        routed_seed.calendar_ref = routed_artifact.calendar_ref;
        routed_seed.coverage_report = routed_artifact.coverage_report;
        routed_seed.seed_status = BootstrapSeedStatus::kNone;
        routed_seed.seeded_components = routed_artifact.seeded_components;
        routed_seed.enabled_components = routed_artifact.enabled_components;
        if (routed_artifact.value_model) {
            FillValueSeedInitializers(*routed_artifact.value_model,
                                      routed_artifact.coverage_report,
                                      BootstrapSeedStatus::kPartial,
                                      &routed_seed.theta_init,
                                      &routed_seed.monthpos_hint,
                                      &routed_seed.event_hint,
                                      &routed_seed.sigma_init,
                                      &routed_seed.ratio_prior_init,
                                      &routed_seed.uncertainty_init,
                                      &routed_seed.maturity_init);
            routed_seed.seeded_components = MakeSeededComponents(
                routed_seed.theta_init, &routed_seed.monthpos_hint, &routed_seed.event_hint);
            routed_seed.enabled_components =
                MakeEnabledComponents(routed_seed.theta_init,
                                      routed_seed.coverage_report,
                                      routed_seed.clock_spec,
                                      &routed_seed.monthpos_hint,
                                      &routed_seed.event_hint);
            routed_seed.seed_status =
                EvaluateBootstrapSeedStatus(BaselineStatus::kOk,
                                            BootstrapArtifactKind::kValue,
                                            routed_seed.coverage_report,
                                            routed_seed.clock_spec,
                                            &routed_seed.theta_init,
                                            &routed_seed.sigma_init,
                                            routed_seed.enabled_components);
            routed_seed.maturity_init.seed_status = routed_seed.seed_status;
        } else if (routed_artifact.ratio_model) {
            FillRatioSeedInitializers(*routed_artifact.ratio_model,
                                      routed_artifact.coverage_report,
                                      BootstrapSeedStatus::kPartial,
                                      &routed_seed.theta_init,
                                      &routed_seed.monthpos_hint,
                                      &routed_seed.event_hint,
                                      &routed_seed.sigma_init,
                                      &routed_seed.ratio_prior_init,
                                      &routed_seed.uncertainty_init,
                                      &routed_seed.maturity_init);
            routed_seed.seeded_components = MakeSeededComponents(
                routed_seed.theta_init, &routed_seed.monthpos_hint, &routed_seed.event_hint);
            routed_seed.enabled_components =
                MakeEnabledComponents(routed_seed.theta_init,
                                      routed_seed.coverage_report,
                                      routed_seed.clock_spec,
                                      &routed_seed.monthpos_hint,
                                      &routed_seed.event_hint);
            routed_seed.seed_status =
                EvaluateBootstrapSeedStatus(BaselineStatus::kOk,
                                            BootstrapArtifactKind::kRatio,
                                            routed_seed.coverage_report,
                                            routed_seed.clock_spec,
                                            &routed_seed.theta_init,
                                            &routed_seed.sigma_init,
                                            routed_seed.enabled_components);
            routed_seed.maturity_init.seed_status = routed_seed.seed_status;
        }
        out_seed->relation_routed_summary_seeds.push_back(std::move(routed_seed));
    }
    return artifact.train_status == BaselineStatus::kOk ? BaselineStatus::kOk
                                                        : BaselineStatus::kNotTrained;
}

BaselineSerializationResult BootstrapEngine::ExportArtifact(
    const BootstrapArtifact& artifact,
    BaselineSerializationFormat format) const {
    if (format != BaselineSerializationFormat::kJson) {
        return {BaselineStatus::kUnsupportedFormat, ""};
    }
    if (artifact.train_status != BaselineStatus::kOk) {
        return {BaselineStatus::kNotTrained, ""};
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("schema_version");
    writer.Int(1);
    WriteStringField(&writer, "document_kind", "bootstrap_artifact");
    WriteStringField(&writer, "artifact_kind", ArtifactKindName(artifact.artifact_kind));
    WriteStringField(&writer, "algorithm_version", "b1-bootstrap-v1");
    writer.Key("model_version");
    writer.Uint64(artifact.model_version);
    WriteTaskIdentity(&writer, artifact.task_identity);
    WriteSeriesIdentity(&writer, artifact.series_key);
    WriteClockSpec(&writer, artifact.clock_spec);
    WriteCalendarRef(&writer, artifact.calendar_ref);
    WriteCoverage(&writer, artifact.coverage_report);
    WriteStringVector(&writer, "seeded_components", artifact.seeded_components);
    WriteStringVector(&writer, "enabled_components", artifact.enabled_components);
    writer.Key("model");
    writer.StartObject();
    if (artifact.artifact_kind == BootstrapArtifactKind::kValue && artifact.value_model) {
        WriteValueModel(&writer, *artifact.value_model);
    } else if (artifact.artifact_kind == BootstrapArtifactKind::kRatio && artifact.ratio_model) {
        WriteRatioModel(&writer, *artifact.ratio_model);
    }
    writer.EndObject();
    if (artifact.artifact_kind == BootstrapArtifactKind::kRelation) {
        writer.Key("relation_basis_by_metric");
        writer.StartArray();
        for (const auto& basis : artifact.relation_basis_by_metric) {
            WriteRelationBasis(&writer, basis);
        }
        writer.EndArray();
        writer.Key("relation_routed_summary_artifacts");
        writer.StartArray();
        for (const auto& routed_artifact : artifact.relation_routed_summary_artifacts) {
            WriteRoutedSummaryArtifact(&writer, routed_artifact);
        }
        writer.EndArray();
        const RelationFusionBootstrapMetadata metadata =
            HasRelationFusionMetadata(artifact.relation_fusion_metadata)
                ? artifact.relation_fusion_metadata
                : MakeRelationFusionMetadata(artifact);
        WriteRelationFusionMetadata(&writer, metadata);
    }
    WriteStringField(&writer, "diagnostics", artifact.diagnostics);
    writer.EndObject();
    return {BaselineStatus::kOk, buf.GetString()};
}

BaselineStatus BootstrapEngine::LoadArtifact(std::string_view content,
                                             BaselineSerializationFormat format,
                                             BootstrapArtifact* out_artifact) const {
    if (!out_artifact) return BaselineStatus::kInvalidArgument;
    if (format != BaselineSerializationFormat::kJson) return BaselineStatus::kUnsupportedFormat;

    rapidjson::Document doc;
    doc.Parse(content.data(), content.size());
    if (doc.HasParseError() || !doc.IsObject()) return BaselineStatus::kParseFailed;
    if (!doc.HasMember("document_kind") || !doc["document_kind"].IsString() ||
        std::string(doc["document_kind"].GetString()) != "bootstrap_artifact") {
        return BaselineStatus::kIncompatibleArtifact;
    }
    if (!doc.HasMember("schema_version") || !doc["schema_version"].IsInt() ||
        doc["schema_version"].GetInt() != 1) {
        return BaselineStatus::kIncompatibleArtifact;
    }
    if (!doc.HasMember("algorithm_version") || !doc["algorithm_version"].IsString() ||
        std::string(doc["algorithm_version"].GetString()) != "b1-bootstrap-v1") {
        return BaselineStatus::kIncompatibleArtifact;
    }
    if (!doc.HasMember("artifact_kind") || !doc["artifact_kind"].IsString() ||
        !doc.HasMember("model") || !doc["model"].IsObject()) {
        return BaselineStatus::kParseFailed;
    }

    BootstrapArtifact artifact;
    artifact.artifact_kind = ParseArtifactKind(doc["artifact_kind"].GetString());
    artifact.train_status = BaselineStatus::kOk;
    if (doc.HasMember("model_version") && doc["model_version"].IsUint64()) {
        artifact.model_version = doc["model_version"].GetUint64();
    }
    if (artifact.model_version == 0 ||
        !ReadTaskIdentity(doc, &artifact.task_identity) ||
        !ReadSeriesIdentity(doc, &artifact.series_key) ||
        !ReadClockSpec(doc, &artifact.clock_spec) ||
        !ReadCalendarRef(doc, &artifact.calendar_ref)) {
        return BaselineStatus::kParseFailed;
    }
    ReadCoverage(doc, &artifact.coverage_report);
    (void)ReadStringVector(doc, "seeded_components", &artifact.seeded_components);
    (void)ReadStringVector(doc, "enabled_components", &artifact.enabled_components);
    (void)ReadString(doc, "diagnostics", &artifact.diagnostics);

    const auto& model = doc["model"];
    if (artifact.artifact_kind == BootstrapArtifactKind::kValue) {
        if (!ReadValueFormalModel(model, artifact.model_version, &artifact.value_model)) {
            return BaselineStatus::kParseFailed;
        }
    } else if (artifact.artifact_kind == BootstrapArtifactKind::kRatio) {
        if (!ReadRatioFormalModel(model, artifact.model_version, &artifact.ratio_model)) {
            return BaselineStatus::kParseFailed;
        }
    } else if (artifact.artifact_kind == BootstrapArtifactKind::kRelation) {
        if (!doc.HasMember("relation_basis_by_metric") ||
            !doc["relation_basis_by_metric"].IsArray() ||
            !doc.HasMember("relation_routed_summary_artifacts") ||
            !doc["relation_routed_summary_artifacts"].IsArray()) {
            return BaselineStatus::kParseFailed;
        }
        for (const auto& item : doc["relation_basis_by_metric"].GetArray()) {
            RelationServiceBasis basis;
            if (!ReadRelationBasis(item, &basis)) return BaselineStatus::kParseFailed;
            artifact.relation_basis_by_metric.push_back(std::move(basis));
        }
        for (const auto& item : doc["relation_routed_summary_artifacts"].GetArray()) {
            RelationRoutedBootstrapArtifact routed;
            if (!ReadRoutedSummaryArtifact(item, &routed)) {
                return BaselineStatus::kParseFailed;
            }
            artifact.relation_routed_summary_artifacts.push_back(std::move(routed));
        }
        for (auto& routed : artifact.relation_routed_summary_artifacts) {
            NormalizeRoutedSummaryScope(artifact.relation_basis_by_metric, &routed);
        }
        if (doc.HasMember("relation_fusion_metadata")) {
            if (!ReadRelationFusionMetadata(doc["relation_fusion_metadata"],
                                            &artifact.relation_fusion_metadata)) {
                return BaselineStatus::kParseFailed;
            }
        } else {
            artifact.relation_fusion_metadata = MakeRelationFusionMetadata(artifact);
            AppendDiagnostic(&artifact.diagnostics, "relation_fusion_metadata_defaulted");
        }
    } else {
        return BaselineStatus::kIncompatibleArtifact;
    }

    *out_artifact = std::move(artifact);
    return BaselineStatus::kOk;
}

BaselineStatus BootstrapEngine::ValidateArtifactCompatibility(
    const BootstrapArtifact& artifact,
    const BaselineTaskSpec& spec,
    BootstrapArtifactKind expected_kind) const {
    if (artifact.artifact_kind != expected_kind) return BaselineStatus::kIncompatibleArtifact;
    if (expected_kind == BootstrapArtifactKind::kValue && artifact.value_model &&
        artifact.value_model->transform_name != (spec.value_identity_transform ? "identity" : "log1p")) {
        return BaselineStatus::kIncompatibleArtifact;
    }
    const BootstrapTaskIdentity expected_identity = MakeIdentity(spec);
    if (artifact.task_identity.task_id != expected_identity.task_id ||
        artifact.task_identity.task_kind != expected_identity.task_kind ||
        artifact.task_identity.feature_type != expected_identity.feature_type ||
        artifact.task_identity.feature_id != expected_identity.feature_id ||
        artifact.task_identity.profile != expected_identity.profile) {
        return BaselineStatus::kIncompatibleArtifact;
    }
    const BootstrapClockSpec expected_clock = MakeClockSpec(spec);
    if (artifact.clock_spec.bucket_seconds != expected_clock.bucket_seconds ||
        artifact.clock_spec.timezone != expected_clock.timezone) {
        return BaselineStatus::kIncompatibleArtifact;
    }
    const BootstrapCalendarRef expected_calendar = MakeCalendarRef(spec);
    if (artifact.calendar_ref.calendar_id != expected_calendar.calendar_id ||
        artifact.calendar_ref.calendar_version != expected_calendar.calendar_version) {
        return BaselineStatus::kIncompatibleArtifact;
    }
    return BaselineStatus::kOk;
}

BaselineStatus BootstrapEngine::ValidateArtifactCompatibility(
    const BootstrapArtifact& artifact,
    const RelationTaskCreateSpec& spec) const {
    if (artifact.artifact_kind != BootstrapArtifactKind::kRelation) {
        return BaselineStatus::kIncompatibleArtifact;
    }
    if (artifact.task_identity.task_id != spec.task_spec.task_id ||
        artifact.task_identity.task_kind != spec.task_spec.task_kind ||
        artifact.task_identity.feature_type != "relation" ||
        artifact.task_identity.feature_id != spec.task_spec.feature_id ||
        artifact.task_identity.profile != spec.task_spec.profile) {
        return BaselineStatus::kIncompatibleArtifact;
    }
    const BootstrapClockSpec expected_clock = MakeClockSpec(spec);
    if (artifact.clock_spec.bucket_seconds != expected_clock.bucket_seconds ||
        artifact.clock_spec.timezone != expected_clock.timezone) {
        return BaselineStatus::kIncompatibleArtifact;
    }
    const BootstrapCalendarRef expected_calendar = MakeCalendarRef(spec);
    if (artifact.calendar_ref.calendar_id != expected_calendar.calendar_id ||
        artifact.calendar_ref.calendar_version != expected_calendar.calendar_version) {
        return BaselineStatus::kIncompatibleArtifact;
    }
    const auto& task = spec.task_spec;
    const std::string& feature_base = task.feature_base.empty() ? task.feature_id : task.feature_base;
    const auto has_metric = [&](const std::string& metric) {
        return std::find(task.metrics.begin(), task.metrics.end(), metric) != task.metrics.end();
    };
    const auto& fusion = artifact.relation_fusion_metadata;
    // Older artifacts used the task feature id as the fusion metadata fallback.
    if (fusion.feature_base != feature_base && fusion.feature_base != task.feature_id) {
        return BaselineStatus::kIncompatibleArtifact;
    }
    for (const auto& summary : fusion.summary_metadata) {
        if (!has_metric(summary.metric_name)) return BaselineStatus::kIncompatibleArtifact;
    }
    for (const auto& pattern : fusion.pattern_metadata) {
        for (const auto& metric : pattern.metrics) {
            if (!has_metric(metric)) return BaselineStatus::kIncompatibleArtifact;
        }
    }
    std::unordered_set<std::string> basis_metrics;
    for (const auto& basis : artifact.relation_basis_by_metric) {
        if (basis.group_space_id != task.group_space_id ||
            basis.group_space_version != task.group_space_version.value_or("") || basis.feature_base != feature_base ||
            basis.k_head != task.summary_policy.k_head || basis.other_group_idxs != task.other_group_idxs ||
            !has_metric(basis.metric_name) || !basis_metrics.insert(basis.metric_name).second) {
            return BaselineStatus::kIncompatibleArtifact;
        }
    }
    for (const auto& routed : artifact.relation_routed_summary_artifacts) {
        if (!has_metric(routed.metric_name)) {
            return BaselineStatus::kIncompatibleArtifact;
        }
        const BaselineTaskSpec routed_spec =
            MakeRoutedSummaryTaskSpec(spec, routed.metric_name, routed.summary_name, routed.task_kind);
        BootstrapArtifact routed_artifact;
        routed_artifact.artifact_kind = routed.task_kind == BaselineTaskKind::kRatio ? BootstrapArtifactKind::kRatio
                                                                                     : BootstrapArtifactKind::kValue;
        routed_artifact.task_identity = routed.task_identity;
        routed_artifact.clock_spec = routed.clock_spec;
        routed_artifact.calendar_ref = routed.calendar_ref;
        if (ValidateArtifactCompatibility(routed_artifact, routed_spec, routed_artifact.artifact_kind) !=
            BaselineStatus::kOk) {
            return BaselineStatus::kIncompatibleArtifact;
        }
        if (routed.basis_scoped && basis_metrics.find(routed.metric_name) == basis_metrics.end()) {
            return BaselineStatus::kIncompatibleArtifact;
        }
    }
    return BaselineStatus::kOk;
}

BaselineSerializationResult BootstrapEngine::ExportSeed(const BootstrapSeed& seed,
                                                        BaselineSerializationFormat format) const {
    if (format != BaselineSerializationFormat::kJson) {
        return {BaselineStatus::kUnsupportedFormat, ""};
    }
    if (seed.seed_status == BootstrapSeedStatus::kNone) {
        return {BaselineStatus::kNotTrained, ""};
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("schema_version");
    writer.Int(1);
    WriteStringField(&writer, "document_kind", "bootstrap_seed");
    WriteStringField(&writer, "algorithm_version", "b1-bootstrap-v1");
    WriteStringField(&writer, "artifact_kind", ArtifactKindName(seed.artifact_kind));
    writer.Key("seed_status");
    writer.String(SeedStatusName(seed.seed_status));
    writer.Key("source_artifact_version");
    writer.Uint64(seed.source_artifact_version);
    WriteTaskIdentity(&writer, seed.task_identity);
    WriteSeriesIdentity(&writer, seed.series_key);
    WriteClockSpec(&writer, seed.clock_spec);
    WriteCalendarRef(&writer, seed.calendar_ref);
    WriteCoverage(&writer, seed.coverage_report);
    WriteStringVector(&writer, "seeded_components", seed.seeded_components);
    WriteStringVector(&writer, "enabled_components", seed.enabled_components);
    WriteThetaInit(&writer, seed.theta_init);
    WriteMonthPosHint(&writer, seed.monthpos_hint);
    WriteEventHint(&writer, seed.event_hint);
    WriteSigmaInit(&writer, seed.sigma_init);
    WriteRatioPriorInit(&writer, seed.ratio_prior_init);
    WriteUncertaintyInit(&writer, seed.uncertainty_init);
    WriteMaturityInit(&writer, seed.maturity_init);
    if (!seed.relation_basis_by_metric.empty()) {
        writer.Key("relation_basis_by_metric");
        writer.StartArray();
        for (const auto& basis : seed.relation_basis_by_metric) {
            WriteRelationBasis(&writer, basis);
        }
        writer.EndArray();
    }
    if (!seed.relation_routed_summary_seeds.empty()) {
        writer.Key("relation_routed_summary_seeds");
        writer.StartArray();
        for (const auto& routed_seed : seed.relation_routed_summary_seeds) {
            WriteRoutedSummarySeed(&writer, routed_seed);
        }
        writer.EndArray();
    }
    if (seed.artifact_kind == BootstrapArtifactKind::kRelation &&
        HasRelationFusionMetadata(seed.relation_fusion_metadata)) {
        WriteRelationFusionMetadata(&writer, seed.relation_fusion_metadata);
    }
    WriteStringField(&writer, "diagnostics", seed.diagnostics);
    writer.EndObject();
    return {BaselineStatus::kOk, buf.GetString()};
}

}  // namespace baseline
}  // namespace flowsql
