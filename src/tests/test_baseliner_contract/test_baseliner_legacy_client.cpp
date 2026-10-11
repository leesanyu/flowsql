// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
// This translation unit uses only the pre-V1 task API and never links a concrete Baseline implementation.
#include <common/iplugin.h>
#include <dlfcn.h>
#include <framework/interfaces/ibaseline_service.h>
#include <cassert>
#include <cstring>
#include <vector>
using namespace flowsql;
namespace {
struct Library final : IRegister, IQuerier {
    void* handle = dlopen("./libflowsql_baseline.so", RTLD_NOW | RTLD_LOCAL);
    IPlugin* plugin = nullptr;
    std::vector<std::pair<Guid, void*>> interfaces;
    Library() {
        assert(handle);
        using Register = IPlugin* (*)(IRegister*, const char*);
        auto reg = reinterpret_cast<Register>(dlsym(handle, "pluginregist"));
        assert(reg);
        plugin = reg(this, nullptr);
        assert(plugin && plugin->Load(this) == 0 && plugin->Start() == 0);
    }
    ~Library() {
        plugin->Stop();
        plugin->Unload();
        dlclose(handle);
    }
    void Regist(const Guid& id, void* p) override { interfaces.emplace_back(id, p); }
    void* First(const Guid& id) override {
        for (const auto& entry : interfaces)
            if (std::memcmp(&id, &entry.first, sizeof(Guid)) == 0) return entry.second;
        return nullptr;
    }
    int Traverse(const Guid& id, fntraverse f) override {
        auto* p = First(id);
        return p ? f(p) : 0;
    }
};
}  // namespace
int main() {
    Library library;
    auto* service = static_cast<IBaselineService*>(library.First(IID_BASELINE_SERVICE));
    assert(service);
    auto task =
        service
            ->CreateValueTask(
                R"({"schema_version":1,"task_id":"legacy","task_name":"legacy","task_kind":"value","feature_id":"v","feature_type":"value_basic","profile":"default","clock_spec":{"bucket_seconds":60,"timezone":"UTC"},"calendar_ref":{"calendar_id":"cn-holiday","calendar_version":"2026.1"}})",
                BaselineSerializationFormat::kJson)
            .second;
    assert(task);
    for (int64_t t = 0; t < 8; ++t) {
        auto r = task->SubmitObservation({"s", t, 100. + t, 1}, {});
        assert(r.status == BaselineStatus::kOk);
    }
    assert(task->PredictRolling("s", 8).status == BaselineStatus::kOk);
    assert(task->QuerySeriesSnapshot("s", BaselineSerializationFormat::kJson).first == BaselineStatus::kOk);
    assert(task->Close() == BaselineStatus::kOk);
}
