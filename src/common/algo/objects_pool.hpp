// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_COMMON_ALGO_OBJECTS_POOL_HPP_
#define _FLOWSQL_COMMON_ALGO_OBJECTS_POOL_HPP_

#include <stdint.h>
#include <string.h>
#include "list.hpp"

namespace flowsql {

template <size_t bsize>
class BufferPool {
 public:
    virtual ~BufferPool() {
        used_buffer_.Clear([](BufferNode* node) { delete node; });
        delete using_buffer_;
    }

    inline uint8_t* Alloc(size_t len) {
        uint8_t* ptr = nullptr;
        if (!using_buffer_) {
            using_buffer_ = new BufferNode;
        }

        if (bsize - using_buffer_->used_ >= len) {
            ptr = &(using_buffer_->values_[using_buffer_->used_]);
            using_buffer_->used_ += len;
        } else {
            used_buffer_.PushBack(using_buffer_);
            using_buffer_ = new BufferNode;
            ptr = &(using_buffer_->values_[using_buffer_->used_]);
            using_buffer_->used_ += len;
        }

        memset(ptr, 0, len);
        return ptr;
    }

    inline const char* Copy(const char* src, size_t len) {
        char* dst = (char*)Alloc(len + 1);
        memcpy(dst, src, len);
        dst[len] = 0;
        return dst;
    }

 protected:
    struct BufferNode : public SinglyNode<BufferNode> {
        uint8_t values_[bsize];
        size_t used_ = 0;
    };

    flowsql::SinglyList<BufferNode> used_buffer_;
    BufferNode* using_buffer_ = nullptr;
};

template <typename ValueType, size_t objects>
class ObjectsPool {
 public:
    virtual ~ObjectsPool() {
        used_objects_.Clear([](ObjectsNode* node) { delete node; });
        delete using_objects_;
    }

    inline ValueType* Alloc() {
        ValueType* ptr = nullptr;
        if (!using_objects_) {
            using_objects_ = new ObjectsNode;
        }

        if (using_objects_->used_ < objects) {
            ptr = &(using_objects_->values_[using_objects_->used_++]);
        } else {
            used_objects_.PushBack(using_objects_);
            using_objects_ = new ObjectsNode;
            ptr = &(using_objects_->values_[using_objects_->used_++]);
        }

        return ptr;
    }

 protected:
    struct ObjectsNode : public SinglyNode<ObjectsNode> {
        ValueType values_[objects];
        size_t used_ = 0;
    };

    flowsql::SinglyList<ObjectsNode> used_objects_;
    ObjectsNode* using_objects_ = nullptr;
};
}  // namespace flowsql

#endif  //_FLOWSQL_COMMON_ALGO_OBJECTS_POOL_HPP_
