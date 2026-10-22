// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "dataframe_transfer.h"

#include <arrow/io/file.h>
#include <arrow/ipc/writer.h>
#include <openssl/rand.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>

namespace flowsql::catalog {
namespace {
constexpr uint64_t kMaxSnapshotBytes = 256ULL * 1024 * 1024;
constexpr uint64_t kMaxProcessBytes = 512ULL * 1024 * 1024;
constexpr size_t kMaxTransfers = 8;
constexpr uint32_t kMaxAcquires = 2;

bool EndsWith(const std::string& text, const char* suffix) {
    const size_t length = std::strlen(suffix);
    return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

std::string NewId() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1) return {};
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        result[i * 2] = digits[bytes[i] >> 4];
        result[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    return result;
}

bool IsPrivateDirectory(const std::string& directory) {
    struct stat status {};
    return lstat(directory.c_str(), &status) == 0 && S_ISDIR(status.st_mode) && !S_ISLNK(status.st_mode) &&
           status.st_uid == geteuid() && (status.st_mode & 077) == 0;
}

class BoundedOutputStream final : public arrow::io::OutputStream {
 public:
    BoundedOutputStream(std::shared_ptr<arrow::io::FileOutputStream> stream, uint64_t max_bytes,
                        std::function<bool(uint64_t)> reserve)
        : stream_(std::move(stream)), max_bytes_(max_bytes), reserve_(std::move(reserve)) {}

    // The IPC writer closes its sink before the publisher fsyncs and renames, so durability is
    // applied here while the descriptor is still owned by stream_.
    arrow::Status Close() override {
        ARROW_RETURN_NOT_OK(stream_->Flush());
        if (fsync(stream_->file_descriptor()) != 0) return arrow::Status::IOError("DataFrame IPC fsync failed");
        return stream_->Close();
    }
    bool closed() const override { return stream_->closed(); }
    arrow::Result<int64_t> Tell() const override { return stream_->Tell(); }
    arrow::Status Flush() override { return stream_->Flush(); }
    arrow::Status Write(const void* data, int64_t nbytes) override {
        if (nbytes < 0 || static_cast<uint64_t>(nbytes) > max_bytes_ - bytes_) {
            limit_reached_ = true;
            return arrow::Status::CapacityError("DataFrame IPC byte budget exceeded");
        }
        if (!reserve_(nbytes)) return arrow::Status::CapacityError("DataFrame IPC byte budget exceeded");
        ARROW_RETURN_NOT_OK(stream_->Write(data, nbytes));
        bytes_ += static_cast<uint64_t>(nbytes);
        return arrow::Status::OK();
    }
    uint64_t bytes_written() const { return bytes_; }
    // Reports a per-snapshot cap rejection so the caller maps it to SNAPSHOT_LIMIT, not a data-plane fault.
    bool limit_reached() const { return limit_reached_; }

 private:
    std::shared_ptr<arrow::io::FileOutputStream> stream_;
    uint64_t max_bytes_;
    uint64_t bytes_ = 0;
    bool limit_reached_ = false;
    std::function<bool(uint64_t)> reserve_;
};

}  // namespace

DataFrameTransferStore::~DataFrameTransferStore() { Stop(); }

int DataFrameTransferStore::Init(const std::string& directory, std::chrono::seconds lease) {
    if (directory.empty() || lease <= std::chrono::seconds::zero()) return -1;
    std::error_code ec;
    const bool existed = std::filesystem::exists(directory, ec);
    if (ec) return -1;
    std::filesystem::create_directories(directory, ec);
    if (ec || (!existed && chmod(directory.c_str(), 0700) != 0) || !IsPrivateDirectory(directory)) return -1;
    const int lock_fd = open((directory + "/.owner.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (lock_fd >= 0) close(lock_fd);
        return -1;
    }
    struct stat lock_status {};
    if (fstat(lock_fd, &lock_status) != 0 || !S_ISREG(lock_status.st_mode) || lock_status.st_uid != geteuid() ||
        (lock_status.st_mode & 077) != 0) {
        close(lock_fd);
        return -1;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        const auto name = entry.path().filename().string();
        // Published objects are "<32 hex>.arrow"; in-flight ones are ".<32 hex>.partial".
        const auto base = name.size() == 38 && EndsWith(name, ".arrow")                            ? name.substr(0, 32)
                          : name.size() == 41 && name.front() == '.' && EndsWith(name, ".partial") ? name.substr(1, 32)
                                                                                                   : std::string{};
        if (base.size() != 32 || !std::all_of(base.begin(), base.end(), [](unsigned char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }))
            continue;
        struct stat status {};
        if (lstat(entry.path().c_str(), &status) == 0 && S_ISREG(status.st_mode) && status.st_uid == geteuid())
            unlink(entry.path().c_str());
    }
    if (ec) {
        close(lock_fd);
        return -1;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (expiry_thread_.joinable()) {
        close(lock_fd);
        return -1;
    }
    directory_ = directory;
    lease_ = lease;
    directory_lock_fd_ = lock_fd;
    stopping_ = false;
    expiry_thread_ = std::thread(&DataFrameTransferStore::ExpireLoop, this);
    return 0;
}

int DataFrameTransferStore::Acquire(IDataFrameSnapshotProviderV1& provider, const std::string& request_id,
                                    const DataFrameSnapshotRequestV1& request, DataFrameTransferInfoV1* output,
                                    std::string* error) {
    if (!output || !error) return -1;
    *output = {};
    error->clear();
    if (request_id.empty() || request.source_id.empty() || request.max_bytes == 0 ||
        request.deadline <= std::chrono::steady_clock::now()) {
        *error = "INVALID_REQUEST";
        return -1;
    }
    auto cancelled = request.cancelled ? request.cancelled : std::make_shared<std::atomic_bool>(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || directory_.empty()) {
            *error = "DATA_PLANE_UNAVAILABLE";
            return -1;
        }
        if (pending_.find(request_id) != pending_.end()) {
            *error = "DUPLICATE_REQUEST";
            return -1;
        }
        for (const auto& [_, transfer] : transfers_) {
            if (transfer.request_id != request_id) continue;
            if (transfer.snapshot.source_id != request.source_id) {
                *error = "DUPLICATE_REQUEST";
                return -1;
            }
            output->source_id = transfer.snapshot.source_id;
            output->transfer_id = _;
            output->snapshot_id = transfer.snapshot.snapshot_id;
            output->schema_revision = transfer.snapshot.schema_revision;
            output->captured_at = transfer.snapshot.captured_at;
            output->row_count = transfer.snapshot.row_count;
            output->ipc_bytes = transfer.ipc_bytes;
            output->expires_at =
                std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                                       transfer.expires_at - std::chrono::steady_clock::now());
            output->schema = transfer.snapshot.batch->schema();
            return 0;
        }
        if (pending_.size() + transfers_.size() >= kMaxTransfers || active_acquires_ >= kMaxAcquires) {
            *error = "BUSY";
            return -1;
        }
        ++active_acquires_;
        pending_.emplace(request_id, Pending{request.source_id, request.max_bytes, cancelled});
    }
    auto finish_pending = [&] {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(request_id);
        if (active_acquires_ > 0) --active_acquires_;
        wake_.notify_all();
    };

    DataFrameSnapshotRequestV1 bounded_request = request;
    bounded_request.max_bytes = std::min(request.max_bytes, kMaxSnapshotBytes);
    bounded_request.cancelled = cancelled;
    DataFrameSnapshotV1 snapshot;
    std::vector<DataFrameSourceV1> sources;
    int status = provider.List(&sources, error);
    if (!status && std::none_of(sources.begin(), sources.end(),
                                [&](const auto& source) { return source.source_id == request.source_id; })) {
        *error = "SOURCE_UNAVAILABLE";
        status = -1;
    }
    if (!status) status = provider.Acquire(bounded_request, &snapshot, error);
    if (status || cancelled->load() || std::chrono::steady_clock::now() >= request.deadline || !snapshot.batch ||
        !snapshot.batch->schema() || snapshot.batch->schema()->num_fields() == 0) {
        // A provider-reported budget/cancel code reaches the caller unchanged; only an unexplained
        // failure is attributed to the source.
        if (error->empty()) *error = cancelled->load() ? "CANCELLED" : "SOURCE_UNAVAILABLE";
        finish_pending();
        return -1;
    }
    const uint64_t retained = snapshot.retained_bytes;
    if (retained > bounded_request.max_bytes || retained > kMaxSnapshotBytes) {
        *error = "SNAPSHOT_LIMIT";
        finish_pending();
        return -1;
    }

    const std::string id = NewId();
    if (id.empty()) {
        *error = "DATA_PLANE_UNAVAILABLE";
        finish_pending();
        return -1;
    }
    const uint64_t snapshot_budget = std::min(bounded_request.max_bytes, kMaxSnapshotBytes);
    if (retained >= snapshot_budget) {
        *error = "SNAPSHOT_LIMIT";
        finish_pending();
        return -1;
    }
    std::string directory;
    bool can_write = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        directory = directory_;
        if (stopping_ || directory.empty() || reserved_bytes_ + retained > kMaxProcessBytes) {
            *error = stopping_ ? "DATA_PLANE_UNAVAILABLE" : "SNAPSHOT_LIMIT";
        } else {
            reserved_bytes_ += retained;
            can_write = true;
        }
    }
    if (!can_write) {
        finish_pending();
        return -1;
    }
    const std::string final_path = directory + "/" + id + ".arrow";
    const std::string partial_path = directory + "/." + id + ".partial";

    const int fd = open(partial_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    bool published = false;
    uint64_t ipc_bytes = 0;
    bool budget_exceeded = false;
    std::shared_ptr<BoundedOutputStream> output_stream;
    if (fd >= 0) {
        auto file = arrow::io::FileOutputStream::Open(fd);
        if (file.ok()) {
            output_stream = std::make_shared<BoundedOutputStream>(
                *file, snapshot_budget - retained,
                [this, retained, &ipc_bytes, &budget_exceeded, cancelled, deadline = request.deadline](uint64_t bytes) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (bytes > kMaxProcessBytes - reserved_bytes_ ||
                        bytes > kMaxSnapshotBytes - retained - ipc_bytes) {
                        budget_exceeded = true;
                        return false;
                    }
                    if (stopping_ || cancelled->load() || std::chrono::steady_clock::now() >= deadline) return false;
                    reserved_bytes_ += bytes;
                    ipc_bytes += bytes;
                    return true;
                });
            auto writer = arrow::ipc::MakeFileWriter(output_stream, snapshot.batch->schema());
            // Close() fsyncs the descriptor; the writer's Close() is the only close, so flush order
            // matches the file lifetime.
            if (writer.ok() && (*writer)->WriteRecordBatch(*snapshot.batch).ok() && (*writer)->Close().ok()) {
                struct stat status {};
                if (fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
                    static_cast<uint64_t>(status.st_size) == output_stream->bytes_written() &&
                    rename(partial_path.c_str(), final_path.c_str()) == 0)
                    published = true;
            }
        } else {
            close(fd);
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(request_id);
        if (active_acquires_ > 0) --active_acquires_;
        if (!published || stopping_ || cancelled->load() || std::chrono::steady_clock::now() >= request.deadline) {
            reserved_bytes_ = reserved_bytes_ >= retained + ipc_bytes ? reserved_bytes_ - retained - ipc_bytes : 0;
            const bool over_budget = budget_exceeded || (output_stream && output_stream->limit_reached());
            *error = over_budget ? "SNAPSHOT_LIMIT" : cancelled->load() ? "CANCELLED" : "DATA_PLANE_UNAVAILABLE";
            wake_.notify_all();
        } else {
            Transfer transfer;
            transfer.request_id = request_id;
            transfer.snapshot = std::move(snapshot);
            transfer.path = final_path;
            transfer.ipc_bytes = ipc_bytes;
            transfer.expires_at = std::chrono::steady_clock::now() + lease_;
            const auto inserted = transfers_.emplace(id, std::move(transfer));
            if (!inserted.second) {
                reserved_bytes_ = reserved_bytes_ >= retained + ipc_bytes ? reserved_bytes_ - retained - ipc_bytes : 0;
                *error = "DATA_PLANE_UNAVAILABLE";
            } else {
                output->source_id = inserted.first->second.snapshot.source_id;
                output->transfer_id = id;
                output->snapshot_id = inserted.first->second.snapshot.snapshot_id;
                output->schema_revision = inserted.first->second.snapshot.schema_revision;
                output->captured_at = inserted.first->second.snapshot.captured_at;
                output->row_count = inserted.first->second.snapshot.row_count;
                output->ipc_bytes = ipc_bytes;
                output->expires_at = std::chrono::system_clock::now() + lease_;
                output->schema = inserted.first->second.snapshot.batch->schema();
            }
        }
    }
    if (!published || !error->empty()) {
        unlink(partial_path.c_str());
        if (published) unlink(final_path.c_str());
        return -1;
    }
    wake_.notify_all();
    return 0;
}

bool DataFrameTransferStore::Cancel(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = pending_.find(request_id);
    if (found == pending_.end()) return false;
    found->second.cancelled->store(true);
    return true;
}

bool DataFrameTransferStore::Release(const std::string& transfer_id) {
    std::string path;
    uint64_t bytes = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = transfers_.find(transfer_id);
        if (found == transfers_.end()) return false;
        bytes = found->second.snapshot.retained_bytes + found->second.ipc_bytes;
        path = std::move(found->second.path);
        transfers_.erase(found);
        reserved_bytes_ -= bytes;
    }
    unlink(path.c_str());
    wake_.notify_all();
    return true;
}

void DataFrameTransferStore::ExpireLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        wake_.wait_for(lock, std::chrono::milliseconds(250));
        const auto now = std::chrono::steady_clock::now();
        for (auto it = transfers_.begin(); it != transfers_.end();) {
            if (it->second.expires_at > now) {
                ++it;
                continue;
            }
            reserved_bytes_ -= it->second.snapshot.retained_bytes + it->second.ipc_bytes;
            const auto path = std::move(it->second.path);
            it = transfers_.erase(it);
            lock.unlock();
            unlink(path.c_str());
            lock.lock();
        }
    }
}

void DataFrameTransferStore::Stop() {
    std::vector<std::string> paths;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (directory_.empty() && !expiry_thread_.joinable()) return;
        stopping_ = true;
        for (auto& [_, pending] : pending_) pending.cancelled->store(true);
        wake_.notify_all();
        wake_.wait_for(lock, std::chrono::seconds(2), [this] { return active_acquires_ == 0; });
        for (const auto& [_, transfer] : transfers_) paths.push_back(transfer.path);
        transfers_.clear();
        pending_.clear();
        reserved_bytes_ = 0;
    }
    wake_.notify_all();
    if (expiry_thread_.joinable()) expiry_thread_.join();
    for (const auto& path : paths) unlink(path.c_str());
    std::lock_guard<std::mutex> lock(mutex_);
    directory_.clear();
    if (directory_lock_fd_ >= 0) {
        close(directory_lock_fd_);
        directory_lock_fd_ = -1;
    }
}

}  // namespace flowsql::catalog
