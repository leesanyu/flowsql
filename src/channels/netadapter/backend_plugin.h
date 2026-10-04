// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#ifndef FLOWSQL_CHANNELS_NETADAPTER_BACKEND_PLUGIN_H_
#define FLOWSQL_CHANNELS_NETADAPTER_BACKEND_PLUGIN_H_
#include <common/iplugin.h>
#include <framework/interfaces/icapture_backend.h>
#include <atomic>
#include <cerrno>
#include <mutex>
namespace flowsql::channels::netadapter {
using CaptureOpen = int (*)(const CaptureBackendConfigV1&, std::shared_ptr<ICaptureBackendSessionV1>*, std::string*);
class CaptureBackendLease final : public ICaptureBackendSessionV1 {
 public:
    CaptureBackendLease(std::shared_ptr<ICaptureBackendSessionV1> session,
                        std::shared_ptr<std::atomic<uint32_t>> active)
        : session_(std::move(session)), active_(std::move(active)) {
        ++*active_;
    }
    ~CaptureBackendLease() override {
        session_.reset();
        --*active_;
    }
    const std::vector<CaptureBackendInputV1>& Inputs() const override { return session_->Inputs(); }
    int TryRead(uint32_t i, CapturePacketViewV1* packet) override { return session_->TryRead(i, packet); }
    void ReleasePacket(uint32_t i) override { session_->ReleasePacket(i); }
    CaptureBacklogV1 Backlog(uint32_t i) const override { return session_->Backlog(i); }
    int64_t IdleTimeNs(uint32_t i) const override { return session_->IdleTimeNs(i); }
    int ReadCounters(uint32_t i, CaptureCountersV1* counters) override { return session_->ReadCounters(i, counters); }
    void Cancel() override { session_->Cancel(); }

 private:
    std::shared_ptr<ICaptureBackendSessionV1> session_;
    std::shared_ptr<std::atomic<uint32_t>> active_;
};
/** The active count remains held until the underlying session destructor has finished. */
class CaptureBackendPlugin : public IPlugin, public ICaptureBackendProviderV1 {
 public:
    CaptureBackendPlugin(const char* name, CaptureOpen open) : name_(name), open_(open) {}
    int Load(IQuerier*) override { return 0; }
    int Start() override {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = true;
        return 0;
    }
    int Stop() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (*active_) return EBUSY;
        started_ = false;
        return 0;
    }
    int Unload() override { return Stop(); }
    const char* Backend() const override { return name_; }
    int Open(const CaptureBackendConfigV1& config, std::shared_ptr<ICaptureBackendSessionV1>* output,
             std::string* error) override {
        if (!output) return EINVAL;
        output->reset();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return EPIPE;
        if (config.backend != name_) return EINVAL;
        std::shared_ptr<ICaptureBackendSessionV1> session;
        const int rc = open_(config, &session, error);
        if (rc) return rc;
        if (!session) return EFAULT;
        *output = std::make_shared<CaptureBackendLease>(std::move(session), active_);
        return 0;
    }

 private:
    const char* name_;
    CaptureOpen open_;
    bool started_ = false;
    std::shared_ptr<std::atomic<uint32_t>> active_ = std::make_shared<std::atomic<uint32_t>>(0);
    std::mutex mutex_;
};
}  // namespace flowsql::channels::netadapter
#endif
