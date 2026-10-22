// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <common/iplugin.h>
#include <httplib.h>

#include <string>
#include <thread>
#include "exploration_service.h"

namespace flowsql::visualization {

// Independent entry; no dependency on Web, Scheduler or Worker. Options are JSON.
struct EntryOptionsV1 {
    std::string host = "127.0.0.1";
    int port = 18910;
    std::string static_dir = "exploration";
    // Deployment-configured data plane; browsers can never supply either value.
    std::string dataframe_upstream;
    std::string dataframe_shared_dir;
};

class VisualizationPlugin : public IPlugin {
 public:
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Start() override;
    int Stop() override;
    int Unload() override;

 private:
    EntryOptionsV1 options_;
    httplib::Server server_;
    std::thread thread_;
    IQuerier* querier_ = nullptr;
    ExplorationService service_;
    QueryLimitsV1 limits_;
};

}  // namespace flowsql::visualization
