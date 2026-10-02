// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <dlfcn.h>

namespace flowsql::test {

class ScopedSharedLibrary final {
 public:
    explicit ScopedSharedLibrary(const char* path) : handle_(dlopen(path, RTLD_NOW | RTLD_LOCAL)) {}

    ~ScopedSharedLibrary() {
        if (handle_ != nullptr) dlclose(handle_);
    }

    ScopedSharedLibrary(const ScopedSharedLibrary&) = delete;
    ScopedSharedLibrary& operator=(const ScopedSharedLibrary&) = delete;
    ScopedSharedLibrary(ScopedSharedLibrary&&) = delete;
    ScopedSharedLibrary& operator=(ScopedSharedLibrary&&) = delete;

    explicit operator bool() const { return handle_ != nullptr; }

    void* Symbol(const char* name) const { return handle_ != nullptr ? dlsym(handle_, name) : nullptr; }

 private:
    void* handle_ = nullptr;
};

}  // namespace flowsql::test
