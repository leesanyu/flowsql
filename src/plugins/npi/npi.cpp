/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2021-01-24 14:05:07
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#include "npi.h"
#include "config.h"
#include "engine.h"
#include "layer.h"

// #include <common/logger_helper.h>
// #include <common/path_util.h>
#include <rapidjson/document.h>

BEGIN_PLUGIN_REGIST(flowsql::NetworkProtocolIdentify)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_PROTOCOL, flowsql::IProtocol)
END_PLUGIN_REGIST()

namespace flowsql {
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
        dom.Parse(option);

        for (auto iter = dom.MemberBegin(); iter != dom.MemberEnd(); ++iter) {
            std::string key = iter->name.GetString();
            if (key == "ldfile") {
                std::string ldfile = iter->value.GetString();
                if (ldfile.size() > 0 && ldfile[0] == '.') {
                    std::string app_path;
                    // path_util::get_app_path(app_path);
                    // app_path = path_util::add_slash(app_path);
                    ldfile = app_path + ldfile;
                }

                if (0 == config_->Load(ldfile.c_str())) {
                    // LOG_I() << "Load protocol definition from " << ldfile << " successfully.";
                } else {
                    // LOG_E() << "Load protocol definition from " << ldfile << " failed.";
                }
            }
        }
    } catch (...) {
        // LOG_E() << "Parse config failed:\n" << option;
        return -1;
    }

    return 0;
}

int NetworkProtocolIdentify::Load() { return engine_->Create(config_); }

int NetworkProtocolIdentify::Unload() { return 0; }

void NetworkProtocolIdentify::Concurrency(int32_t number) { engine_->Concurrency(number); }

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
}  // namespace flowsql