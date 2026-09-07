// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_SERVICES_WEB_PCAP_UPLOAD_TRANSACTION_HPP_
#define FLOWSQL_SERVICES_WEB_PCAP_UPLOAD_TRANSACTION_HPP_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include <services/web/managed_capture_store.hpp>

namespace flowsql::web {

using PcapSchedulerAddCallback =
    std::function<PcapUploadError(const std::string& request_json, std::string* message)>;

class PcapUploadTransaction {
 public:
    PcapUploadTransaction(const ManagedCaptureStore* store,
                          PcapUploadRequest request,
                          uint64_t max_bytes,
                          PcapSchedulerAddCallback scheduler_add)
        : store_(store),
          request_(std::move(request)),
          max_bytes_(max_bytes),
          scheduler_add_(std::move(scheduler_add)) {}

    PcapUploadTransaction(const PcapUploadTransaction&) = delete;
    PcapUploadTransaction& operator=(const PcapUploadTransaction&) = delete;

    PcapUploadError Begin(std::string* message);
    PcapUploadError Write(const char* data, size_t size, std::string* message);
    PcapUploadError Complete(std::string* public_json, std::string* message);
    void Rollback() noexcept { upload_.Rollback(); }

 private:
    const ManagedCaptureStore* store_;
    PcapUploadRequest request_;
    uint64_t max_bytes_;
    PcapSchedulerAddCallback scheduler_add_;
    ManagedCaptureUpload upload_;
};

namespace pcap_upload_transaction_detail {

inline void Clear(std::string* value) {
    if (value) value->clear();
}

inline PcapUploadError Fail(PcapUploadError error,
                            const char* text,
                            std::string* message) {
    if (message) *message = text;
    return error;
}

}  // namespace pcap_upload_transaction_detail

inline PcapUploadError PcapUploadTransaction::Begin(std::string* message) {
    pcap_upload_transaction_detail::Clear(message);
    if (!store_) {
        return pcap_upload_transaction_detail::Fail(
            PcapUploadError::kInternal, "managed capture store is null", message);
    }
    if (!scheduler_add_) {
        return pcap_upload_transaction_detail::Fail(
            PcapUploadError::kInternal, "scheduler add callback is empty", message);
    }
    return store_->Begin(request_, &upload_, message);
}

inline PcapUploadError PcapUploadTransaction::Write(const char* data,
                                                    size_t size,
                                                    std::string* message) {
    return upload_.Write(data, size, max_bytes_, message);
}

inline PcapUploadError PcapUploadTransaction::Complete(std::string* public_json,
                                                       std::string* message) {
    pcap_upload_transaction_detail::Clear(message);
    if (public_json) public_json->clear();
    if (!public_json) {
        Rollback();
        return pcap_upload_transaction_detail::Fail(
            PcapUploadError::kInternal, "public upload response is null", message);
    }
    if (!scheduler_add_) {
        Rollback();
        return pcap_upload_transaction_detail::Fail(
            PcapUploadError::kInternal, "scheduler add callback is empty", message);
    }

    ManagedCaptureRef capture;
    PcapUploadError error = upload_.Finalize(&capture, message);
    if (error != PcapUploadError::kOk) return error;

    error = scheduler_add_(BuildSchedulerPcapAddJson(request_, capture), message);
    if (error != PcapUploadError::kOk) {
        Rollback();
        return error;
    }

    error = upload_.Commit(message);
    if (error != PcapUploadError::kOk) {
        Rollback();
        return error;
    }
    *public_json = BuildPublicPcapUploadJson(capture, "running");
    return PcapUploadError::kOk;
}

}  // namespace flowsql::web

#endif  // FLOWSQL_SERVICES_WEB_PCAP_UPLOAD_TRANSACTION_HPP_
