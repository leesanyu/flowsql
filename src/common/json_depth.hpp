// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_COMMON_JSON_DEPTH_HPP_
#define _FLOWSQL_COMMON_JSON_DEPTH_HPP_

#include <cstddef>
#include <string>

namespace flowsql {

inline bool JsonNestingWithin(const std::string& input, size_t max_depth) {
    size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (char ch : input) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '{' || ch == '[') {
            if (++depth > max_depth) return false;
        } else if ((ch == '}' || ch == ']') && depth > 0) {
            --depth;
        }
    }
    return true;
}

}  // namespace flowsql

#endif  // _FLOWSQL_COMMON_JSON_DEPTH_HPP_
