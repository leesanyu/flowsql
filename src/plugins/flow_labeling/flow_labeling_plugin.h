// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_FLOW_LABELING_FLOW_LABELING_PLUGIN_H_
#define _FLOWSQL_PLUGINS_FLOW_LABELING_FLOW_LABELING_PLUGIN_H_

#include <common/iplugin.h>
#include <framework/interfaces/iflow_labeling.h>

#include <sched.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace flowsql {

struct FlowLabelingStartupOptions {
    uint32_t eal_memory_mib = 512;
    std::optional<uint32_t> eal_lcore_cpu;
};

inline std::vector<std::string> BuildFlowLabelingEalArguments(const FlowLabelingStartupOptions& options,
                                                              uint32_t current_cpu) {
    const uint32_t cpu = options.eal_lcore_cpu.value_or(current_cpu);
    return {"flowsql-flow-labeling",
            "--lcores=0@" + std::to_string(cpu),
            "--main-lcore=0",
            "-m",
            std::to_string(options.eal_memory_mib),
            "--no-huge",
            "--no-pci",
            "--no-telemetry",
            "--no-shconf"};
}

inline int ParseFlowLabelingStartupOptions(const char* arg, FlowLabelingStartupOptions* output) {
    if (output == nullptr) return -1;

    FlowLabelingStartupOptions parsed;
    if (arg == nullptr || arg[0] == '\0') {
        *output = parsed;
        return 0;
    }

    const auto parse_decimal = [](const std::string& value, uint32_t* result) {
        if (value.empty() || !std::all_of(value.begin(), value.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
            return false;
        }
        uint32_t number = 0;
        const auto conversion = std::from_chars(value.data(), value.data() + value.size(), number);
        if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size()) return false;
        *result = number;
        return true;
    };

    bool have_memory = false;
    bool have_cpu = false;
    const std::string option(arg);
    size_t begin = 0;
    while (begin < option.size()) {
        size_t end = option.find(';', begin);
        if (end == std::string::npos) end = option.size();
        if (end == begin) return -1;

        const std::string field = option.substr(begin, end - begin);
        const size_t equal = field.find('=');
        if (equal == std::string::npos || equal == 0 || equal + 1 == field.size() ||
            field.find('=', equal + 1) != std::string::npos) {
            return -1;
        }
        const std::string key = field.substr(0, equal);
        const std::string value = field.substr(equal + 1);
        uint32_t number = 0;
        if (!parse_decimal(value, &number)) return -1;

        if (key == "eal_memory_mib") {
            if (have_memory || number < 512 || number > 4096) return -1;
            parsed.eal_memory_mib = number;
            have_memory = true;
        } else if (key == "eal_lcore_cpu") {
            if (have_cpu || number >= CPU_SETSIZE) return -1;
            cpu_set_t affinity;
            CPU_ZERO(&affinity);
            if (sched_getaffinity(0, sizeof(affinity), &affinity) != 0 || !CPU_ISSET(number, &affinity)) return -1;
            parsed.eal_lcore_cpu = number;
            have_cpu = true;
        } else {
            return -1;
        }

        if (end == option.size()) break;
        begin = end + 1;
        if (begin == option.size()) return -1;
    }

    *output = parsed;
    return 0;
}

class FlowLabelingPlugin final : public IPlugin, public IFlowLabelingProviderV1 {
 public:
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

    FlowLabelingErrorV1 RuntimeStatus(FlowLabelingDiagnosticV1* diagnostic) const override;
    FlowLabelingErrorV1 CreateMatcher(const FlowLabelingCompileRequestV1& request, IFlowLabelMatcherV1** output,
                                      FlowLabelingDiagnosticV1* diagnostic) override;

 private:
    class Matcher;

    void ReleaseMatcher() noexcept;

    mutable std::mutex mutex_;
    bool loaded_ = false;
    bool ready_ = false;
    bool eal_initialized_ = false;
    uint64_t active_matchers_ = 0;
    FlowLabelingStartupOptions startup_options_;
};

}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_FLOW_LABELING_FLOW_LABELING_PLUGIN_H_
