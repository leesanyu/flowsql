// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_SERVICES_WEB_MANAGED_CAPTURE_STORE_HPP_
#define FLOWSQL_SERVICES_WEB_MANAGED_CAPTURE_STORE_HPP_

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "pcap_upload_contract.hpp"

namespace flowsql::web {

class ManagedCaptureStore;

class ManagedCaptureUpload {
 public:
    ManagedCaptureUpload() = default;
    ~ManagedCaptureUpload();

    ManagedCaptureUpload(const ManagedCaptureUpload&) = delete;
    ManagedCaptureUpload& operator=(const ManagedCaptureUpload&) = delete;
    ManagedCaptureUpload(ManagedCaptureUpload&& other) noexcept;
    ManagedCaptureUpload& operator=(ManagedCaptureUpload&& other) noexcept;

    PcapUploadError Write(const char* data,
                          size_t size,
                          uint64_t max_bytes,
                          std::string* message);
    PcapUploadError Finalize(ManagedCaptureRef* capture, std::string* message);
    PcapUploadError Commit(std::string* message);
    void Rollback() noexcept;

    bool active() const { return active_; }
    bool finalized() const { return finalized_; }
    uint64_t size_bytes() const { return size_bytes_; }
    const std::filesystem::path& part_path() const { return part_path_; }
    const std::filesystem::path& final_path() const { return final_path_; }

 private:
    friend class ManagedCaptureStore;

    std::string channel_name_;
    std::string original_filename_;
    std::filesystem::path part_path_;
    std::filesystem::path final_path_;
    std::ofstream output_;
    uint64_t size_bytes_ = 0;
    bool active_ = false;
    bool finalized_ = false;
};

class ManagedCaptureStore {
 public:
    PcapUploadError Initialize(const std::filesystem::path& upload_dir,
                               std::string* message);
    PcapUploadError Begin(const PcapUploadRequest& request,
                          ManagedCaptureUpload* upload,
                          std::string* message) const;
    PcapUploadError RemoveManaged(const std::filesystem::path& path,
                                  std::string* message) const;

    bool initialized() const { return !root_.empty(); }
    const std::filesystem::path& root() const { return root_; }
    bool IsManagedPath(const std::filesystem::path& path) const;

 private:
    std::filesystem::path root_;
};

namespace capture_store_detail {

inline void ClearMessage(std::string* message) {
    if (message) message->clear();
}

inline PcapUploadError Fail(PcapUploadError error,
                            const char* text,
                            std::string* message) {
    if (message) *message = text;
    return error;
}

inline bool IsBelowRoot(const std::filesystem::path& root,
                        const std::filesystem::path& candidate) {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *root_it != *candidate_it) return false;
    }
    return candidate_it != candidate.end();
}

inline std::string NextFileToken() {
    static std::atomic<uint64_t> sequence{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(ticks) + "-" + std::to_string(sequence.fetch_add(1));
}

}  // namespace capture_store_detail

inline ManagedCaptureUpload::~ManagedCaptureUpload() {
    Rollback();
}

inline ManagedCaptureUpload::ManagedCaptureUpload(ManagedCaptureUpload&& other) noexcept {
    *this = std::move(other);
}

inline ManagedCaptureUpload& ManagedCaptureUpload::operator=(ManagedCaptureUpload&& other) noexcept {
    if (this == &other) return *this;
    Rollback();
    channel_name_ = std::move(other.channel_name_);
    original_filename_ = std::move(other.original_filename_);
    part_path_ = std::move(other.part_path_);
    final_path_ = std::move(other.final_path_);
    output_ = std::move(other.output_);
    size_bytes_ = other.size_bytes_;
    active_ = other.active_;
    finalized_ = other.finalized_;
    other.size_bytes_ = 0;
    other.active_ = false;
    other.finalized_ = false;
    return *this;
}

inline PcapUploadError ManagedCaptureUpload::Write(const char* data,
                                                   size_t size,
                                                   uint64_t max_bytes,
                                                   std::string* message) {
    capture_store_detail::ClearMessage(message);
    if (!active_ || finalized_ || !output_.is_open()) {
        return capture_store_detail::Fail(PcapUploadError::kInternal,
                                          "capture upload is not writable",
                                          message);
    }
    if (size != 0 && !data) {
        Rollback();
        return capture_store_detail::Fail(PcapUploadError::kInternal,
                                          "capture chunk is null",
                                          message);
    }
    uint64_t next_size = 0;
    const PcapUploadError size_error =
        CheckPcapUploadSize(size_bytes_, size, max_bytes, &next_size);
    if (size_error != PcapUploadError::kOk) {
        Rollback();
        return capture_store_detail::Fail(size_error, "capture exceeds size limit", message);
    }
    if (size != 0) {
        output_.write(data, static_cast<std::streamsize>(size));
        if (!output_) {
            Rollback();
            return capture_store_detail::Fail(PcapUploadError::kStorageFailure,
                                              "failed to write capture chunk",
                                              message);
        }
    }
    size_bytes_ = next_size;
    return PcapUploadError::kOk;
}

inline PcapUploadError ManagedCaptureUpload::Finalize(ManagedCaptureRef* capture,
                                                      std::string* message) {
    capture_store_detail::ClearMessage(message);
    if (!capture) {
        return capture_store_detail::Fail(PcapUploadError::kInternal,
                                          "capture output is null",
                                          message);
    }
    if (!active_ || finalized_ || !output_.is_open()) {
        return capture_store_detail::Fail(PcapUploadError::kInternal,
                                          "capture upload cannot be finalized",
                                          message);
    }

    output_.flush();
    if (!output_) {
        Rollback();
        return capture_store_detail::Fail(PcapUploadError::kStorageFailure,
                                          "failed to flush capture file",
                                          message);
    }
    output_.close();
    if (output_.fail()) {
        Rollback();
        return capture_store_detail::Fail(PcapUploadError::kStorageFailure,
                                          "failed to close capture file",
                                          message);
    }

    std::error_code error;
    std::filesystem::rename(part_path_, final_path_, error);
    if (error) {
        Rollback();
        return capture_store_detail::Fail(PcapUploadError::kStorageFailure,
                                          "failed to finalize capture file",
                                          message);
    }
    finalized_ = true;
    const std::filesystem::path canonical = std::filesystem::canonical(final_path_, error);
    if (error) {
        Rollback();
        return capture_store_detail::Fail(PcapUploadError::kStorageFailure,
                                          "failed to resolve finalized capture",
                                          message);
    }

    capture->channel_name = channel_name_;
    capture->original_filename = original_filename_;
    capture->canonical_path = canonical;
    capture->size_bytes = size_bytes_;
    return PcapUploadError::kOk;
}

inline PcapUploadError ManagedCaptureUpload::Commit(std::string* message) {
    capture_store_detail::ClearMessage(message);
    if (!active_ || !finalized_ || output_.is_open()) {
        return capture_store_detail::Fail(PcapUploadError::kInternal,
                                          "capture upload cannot be committed",
                                          message);
    }
    active_ = false;
    return PcapUploadError::kOk;
}

inline void ManagedCaptureUpload::Rollback() noexcept {
    if (output_.is_open()) output_.close();
    if (active_) {
        std::error_code ignored;
        if (!part_path_.empty()) std::filesystem::remove(part_path_, ignored);
        ignored.clear();
        if (!final_path_.empty()) std::filesystem::remove(final_path_, ignored);
    }
    active_ = false;
    finalized_ = false;
}

inline PcapUploadError ManagedCaptureStore::Initialize(
    const std::filesystem::path& upload_dir,
    std::string* message) {
    capture_store_detail::ClearMessage(message);
    root_.clear();
    if (upload_dir.empty()) {
        return capture_store_detail::Fail(PcapUploadError::kInvalidRequest,
                                          "upload directory is empty",
                                          message);
    }

    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(upload_dir, error);
    if (error) {
        return capture_store_detail::Fail(PcapUploadError::kStorageFailure,
                                          "failed to resolve upload directory",
                                          message);
    }
    const std::filesystem::path managed = absolute / "pcapfile";
    std::filesystem::create_directories(managed, error);
    if (error) {
        return capture_store_detail::Fail(PcapUploadError::kStorageFailure,
                                          "failed to create managed capture directory",
                                          message);
    }
    if (!std::filesystem::is_directory(managed, error) || error) {
        return capture_store_detail::Fail(PcapUploadError::kStorageFailure,
                                          "failed to create managed capture directory",
                                          message);
    }
    const std::filesystem::path canonical = std::filesystem::canonical(managed, error);
    if (error || !canonical.is_absolute()) {
        return capture_store_detail::Fail(PcapUploadError::kStorageFailure,
                                          "failed to canonicalize managed capture directory",
                                          message);
    }
    root_ = canonical;
    return PcapUploadError::kOk;
}

inline PcapUploadError ManagedCaptureStore::Begin(const PcapUploadRequest& request,
                                                  ManagedCaptureUpload* upload,
                                                  std::string* message) const {
    capture_store_detail::ClearMessage(message);
    if (!initialized() || !upload) {
        return capture_store_detail::Fail(PcapUploadError::kInternal,
                                          "managed capture store is not ready",
                                          message);
    }
    if (upload->active_) {
        return capture_store_detail::Fail(PcapUploadError::kInternal,
                                          "capture upload is already active",
                                          message);
    }
    if (!detail::IsSafeLogicalName(request.channel_name) ||
        !detail::IsSupportedCaptureFilename(request.original_filename)) {
        return capture_store_detail::Fail(PcapUploadError::kInvalidRequest,
                                          "invalid managed capture identity",
                                          message);
    }

    std::string extension = std::filesystem::path(request.original_filename).extension().string();
    for (char& ch : extension) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    for (int attempt = 0; attempt < 32; ++attempt) {
        const std::string filename = "capture-" + capture_store_detail::NextFileToken() + extension;
        const std::filesystem::path final_path = root_ / filename;
        std::filesystem::path part_path = final_path;
        part_path += ".part";
        std::error_code error;
        if (std::filesystem::exists(final_path, error) || error) continue;
        if (std::filesystem::exists(part_path, error) || error) continue;

        std::ofstream output(part_path, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!output.is_open()) continue;
        upload->channel_name_ = request.channel_name;
        upload->original_filename_ = request.original_filename;
        upload->part_path_ = part_path;
        upload->final_path_ = final_path;
        upload->output_ = std::move(output);
        upload->size_bytes_ = 0;
        upload->active_ = true;
        upload->finalized_ = false;
        return PcapUploadError::kOk;
    }
    return capture_store_detail::Fail(PcapUploadError::kStorageFailure,
                                      "failed to create managed capture file",
                                      message);
}

inline bool ManagedCaptureStore::IsManagedPath(const std::filesystem::path& path) const {
    if (!initialized() || !path.is_absolute() || path == root_) return false;
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    if (error || std::filesystem::is_symlink(status)) return false;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    return !error && capture_store_detail::IsBelowRoot(root_, canonical);
}

inline PcapUploadError ManagedCaptureStore::RemoveManaged(
    const std::filesystem::path& path,
    std::string* message) const {
    capture_store_detail::ClearMessage(message);
    if (!IsManagedPath(path)) {
        return capture_store_detail::Fail(PcapUploadError::kInvalidRequest,
                                          "capture path is not managed",
                                          message);
    }

    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    if (error) {
        return capture_store_detail::Fail(PcapUploadError::kStorageFailure,
                                          "failed to inspect managed capture",
                                          message);
    }
    if (!std::filesystem::exists(status)) return PcapUploadError::kOk;
    if (!std::filesystem::is_regular_file(status)) {
        return capture_store_detail::Fail(PcapUploadError::kInvalidRequest,
                                          "managed capture is not a regular file",
                                          message);
    }
    const std::filesystem::path canonical = std::filesystem::canonical(path, error);
    if (error || !capture_store_detail::IsBelowRoot(root_, canonical)) {
        return capture_store_detail::Fail(PcapUploadError::kInvalidRequest,
                                          "capture path escapes managed root",
                                          message);
    }
    if (!std::filesystem::remove(canonical, error) || error) {
        return capture_store_detail::Fail(PcapUploadError::kStorageFailure,
                                          "failed to remove managed capture",
                                          message);
    }
    return PcapUploadError::kOk;
}

}  // namespace flowsql::web

#endif  // FLOWSQL_SERVICES_WEB_MANAGED_CAPTURE_STORE_HPP_
