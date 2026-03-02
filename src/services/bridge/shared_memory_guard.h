#ifndef _FLOWSQL_BRIDGE_SHARED_MEMORY_GUARD_H_
#define _FLOWSQL_BRIDGE_SHARED_MEMORY_GUARD_H_

#include <cstdio>
#include <string>
#include <utility>

namespace flowsql {
namespace bridge {

// RAII 守卫，析构时自动 unlink 共享内存文件
class SharedMemoryGuard {
 public:
    SharedMemoryGuard(std::string in_path, std::string out_path)
        : in_path_(std::move(in_path)), out_path_(std::move(out_path)) {}

    ~SharedMemoryGuard() {
        if (!in_path_.empty()) {
            if (std::remove(in_path_.c_str()) == 0) {
                printf("SharedMemoryGuard: cleaned up %s\n", in_path_.c_str());
            }
        }
        if (!out_path_.empty()) {
            if (std::remove(out_path_.c_str()) == 0) {
                printf("SharedMemoryGuard: cleaned up %s\n", out_path_.c_str());
            }
        }
    }

    // non-copyable
    SharedMemoryGuard(const SharedMemoryGuard&) = delete;
    SharedMemoryGuard& operator=(const SharedMemoryGuard&) = delete;

    // movable
    SharedMemoryGuard(SharedMemoryGuard&& other) noexcept
        : in_path_(std::move(other.in_path_)), out_path_(std::move(other.out_path_)) {
        other.in_path_.clear();
        other.out_path_.clear();
    }

    SharedMemoryGuard& operator=(SharedMemoryGuard&& other) noexcept {
        if (this != &other) {
            in_path_ = std::move(other.in_path_);
            out_path_ = std::move(other.out_path_);
            other.in_path_.clear();
            other.out_path_.clear();
        }
        return *this;
    }

    const std::string& InputPath() const { return in_path_; }
    const std::string& OutputPath() const { return out_path_; }

 private:
    std::string in_path_;
    std::string out_path_;
};

}  // namespace bridge
}  // namespace flowsql

#endif  // _FLOWSQL_BRIDGE_SHARED_MEMORY_GUARD_H_
