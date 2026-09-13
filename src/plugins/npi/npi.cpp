// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npi.h"
#include "config.h"
#include "engine.h"
#include "layer.h"
#include "regexmatch.h"

// #include <common/logger_helper.h>
// #include <common/path_util.h>
#include <rapidjson/document.h>

BEGIN_PLUGIN_REGIST(flowsql::NetworkProtocolIdentify)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_PROTOCOL, flowsql::IProtocol)
____INTERFACE(flowsql::IID_PROTOCOL_PIPELINE_POOL_V1, flowsql::IProtocolPipelinePoolV1)
END_PLUGIN_REGIST()

namespace flowsql {
static_assert(kProtocolPipelineMaxCapacityV1 == protocol::MAX_REGEX_CONCURRENCY,
              "pipeline pool capacity must match the Hyperscan scratch array");

NetworkProtocolIdentify::NetworkProtocolIdentify() {
    config_ = new protocol::Config;
    layer_ = new protocol::NetworkLayer;
    engine_ = new protocol::Engine;
}

NetworkProtocolIdentify::~NetworkProtocolIdentify() {
    delete engine_;
    engine_ = nullptr;
    delete layer_;
    layer_ = nullptr;
    delete config_;
    config_ = nullptr;
}

int NetworkProtocolIdentify::Option(const char* option) {
    try {
        rapidjson::Document dom;
        dom.Parse(option == nullptr || option[0] == '\0' ? "{}" : option);
        if (dom.HasParseError() || !dom.IsObject()) return -1;

        int32_t capacity = 1;
        bool has_ldfile = false;
        std::string ldfile;

        for (auto iter = dom.MemberBegin(); iter != dom.MemberEnd(); ++iter) {
            std::string key = iter->name.GetString();
            if (key == "ldfile") {
                if (!iter->value.IsString()) return -1;
                has_ldfile = true;
                ldfile = iter->value.GetString();
            } else if (key == "concurrency") {
                if (!iter->value.IsInt()) return -1;
                capacity = iter->value.GetInt();
                if (capacity < 1 || capacity > kProtocolPipelineMaxCapacityV1) return -1;
            }
        }

        {
            std::lock_guard<std::mutex> lock(pipeline_mutex_);
            if (pipeline_available_) return -1;
        }

        if (has_ldfile) {
            if (!ldfile.empty() && ldfile[0] == '.') {
                std::string app_path;
                // path_util::get_app_path(app_path);
                // app_path = path_util::add_slash(app_path);
                ldfile = app_path + ldfile;
            }
            if (config_->Load(ldfile.c_str()) != 0) return -1;
        }

        std::lock_guard<std::mutex> lock(pipeline_mutex_);
        pipeline_capacity_ = capacity;
        pipeline_leased_.clear();
        engine_->Concurrency(pipeline_capacity_);
    } catch (...) {
        // LOG_E() << "Parse config failed:\n" << option;
        return -1;
    }

    return 0;
}

int NetworkProtocolIdentify::Load(IQuerier* /* querier */) {
    {
        std::lock_guard<std::mutex> lock(pipeline_mutex_);
        if (pipeline_available_) return -1;
    }

    const int result = engine_->Create(config_);
    if (result != 0) return result;

    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    pipeline_leased_.assign(static_cast<size_t>(pipeline_capacity_), 0);
    pipeline_available_ = true;
    return 0;
}

int NetworkProtocolIdentify::Unload() {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    pipeline_available_ = false;
    pipeline_leased_.clear();
    return 0;
}

void NetworkProtocolIdentify::Concurrency(int32_t number) {
    if (number < 1 || number > kProtocolPipelineMaxCapacityV1) return;

    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    if (pipeline_available_) return;
    pipeline_capacity_ = number;
    pipeline_leased_.clear();
    engine_->Concurrency(pipeline_capacity_);
}

int32_t NetworkProtocolIdentify::Layer(int32_t /* pipeno */, const uint8_t* packet, int32_t packet_size,
                                       protocol::Layers* layers) {
    return layer_->Layer(packet, packet_size, layers);
}

protocol::Protocol NetworkProtocolIdentify::Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size,
                                                     const protocol::Layers* layers) {
    int32_t proid = engine_->Identify(pipeno, packet, packet_size, layers);
    int32_t pproid = Dictionary()->Query(proid)->parents;
    return protocol::Protocol(pproid, proid);
}

protocol::IDictionary* NetworkProtocolIdentify::Dictionary() { return config_->Dict(); }

ProtocolPipelinePoolError NetworkProtocolIdentify::Acquire(int32_t* pipeno) {
    if (pipeno == nullptr) return ProtocolPipelinePoolError::kNullOutput;

    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    if (!pipeline_available_) return ProtocolPipelinePoolError::kUnavailable;
    for (size_t index = 0; index < pipeline_leased_.size(); ++index) {
        if (pipeline_leased_[index] == 0) {
            pipeline_leased_[index] = 1;
            *pipeno = static_cast<int32_t>(index);
            return ProtocolPipelinePoolError::kNone;
        }
    }
    return ProtocolPipelinePoolError::kExhausted;
}

ProtocolPipelinePoolError NetworkProtocolIdentify::Release(int32_t pipeno) {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    if (!pipeline_available_) return ProtocolPipelinePoolError::kUnavailable;
    if (pipeno < 0 || pipeno >= pipeline_capacity_) return ProtocolPipelinePoolError::kInvalidPipeline;
    if (pipeline_leased_[static_cast<size_t>(pipeno)] == 0) return ProtocolPipelinePoolError::kNotLeased;
    pipeline_leased_[static_cast<size_t>(pipeno)] = 0;
    return ProtocolPipelinePoolError::kNone;
}

int32_t NetworkProtocolIdentify::Capacity() const {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    return pipeline_capacity_;
}

IProtocol* NetworkProtocolIdentify::Protocol() { return this; }
}  // namespace flowsql
