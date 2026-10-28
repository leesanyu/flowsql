// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <poll.h>
#include <sys/socket.h>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace flowsql::database {
inline int64_t MetadataIoNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
inline bool MetadataPoll(int fd, short events, int64_t deadline) {
    while (true) {
        const auto remaining = deadline - MetadataIoNow();
        if (remaining <= 0) return false;
        pollfd p{fd, events, 0};
        const int rc = poll(&p, 1, static_cast<int>(remaining));
        if (rc > 0) return true;
        if (rc == 0 || errno != EINTR) return false;
    }
}
// Dedicated metadata sessions only. The session joins this owner before closing/reusing its socket.
class MetadataSocketDeadline {
 public:
    MetadataSocketDeadline(int fd, int64_t deadline)
        : timer_([this, fd, deadline] {
              std::unique_lock<std::mutex> lock(mutex_);
              if (!changed_.wait_until(lock, std::chrono::system_clock::time_point(std::chrono::milliseconds(deadline)),
                                       [this] { return done_; })) {
                  shutdown(fd, SHUT_RDWR);
              }
          }) {}
    ~MetadataSocketDeadline() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        changed_.notify_all();
        timer_.join();
    }

 private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool done_ = false;
    std::thread timer_;
};
}  // namespace flowsql::database
