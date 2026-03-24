#ifndef _FLOWSQL_COMMON_SPAN_H_
#define _FLOWSQL_COMMON_SPAN_H_

#include <cassert>
#include <cstddef>
#include <vector>

namespace flowsql {

// C++17 轻量 Span：只读视图，不拥有底层数据。
template <typename T>
struct Span {
    T* data = nullptr;
    size_t size = 0;

    Span() = default;
    Span(T* d, size_t s) : data(d), size(s) {}
    explicit Span(T& single) : data(&single), size(1) {}
    Span(std::vector<T>& v) : data(v.data()), size(v.size()) {}

    bool empty() const { return size == 0; }
    T& operator[](size_t i) {
        assert(i < size);
        return data[i];
    }
    const T& operator[](size_t i) const {
        assert(i < size);
        return data[i];
    }
};

}  // namespace flowsql

#endif  // _FLOWSQL_COMMON_SPAN_H_
