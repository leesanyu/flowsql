// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#include "workflow.h"

namespace flowsql::visualization {
struct NumericValueV1 {
    double value;
    bool exact;
};
struct NumericDisplayV1 {
    std::string field_id;
    std::string mode = "exact";
    std::string origin = "decimal";
    std::vector<ExplorationValueV1> exact_values;
};

// Canonical decimal text, including scientific notation from native numeric results.
inline std::string CanonicalDecimal(std::string text) {
    const bool negative = !text.empty() && text[0] == '-';
    size_t pos = !text.empty() && (text[0] == '-' || text[0] == '+') ? 1 : 0;
    std::string digits;
    int64_t point = 0;
    bool fractional = false;
    while (pos < text.size() && text[pos] != 'e' && text[pos] != 'E') {
        const auto c = text[pos++];
        if (c == '.' && !fractional) {
            fractional = true;
            continue;
        }
        if (c < '0' || c > '9') throw std::invalid_argument("decimal");
        digits += c;
        if (!fractional) ++point;
    }
    if (digits.empty()) throw std::invalid_argument("decimal");
    const auto first = digits.find_first_not_of('0');
    if (first == std::string::npos) return "0";
    if (pos < text.size()) {
        size_t consumed = 0;
        const auto exponent = std::stoll(text.substr(pos + 1), &consumed);
        if (consumed != text.size() - pos - 1 || exponent > int64_t(text.size()) + 1100 ||
            exponent < -int64_t(text.size()) - 1100)
            throw std::invalid_argument("decimal exponent");
        point += exponent;
    }
    digits.erase(0, first);
    point -= first;
    std::string result;
    if (point <= 0)
        result = "0." + std::string(-point, '0') + digits;
    else if (size_t(point) >= digits.size())
        result = digits + std::string(point - digits.size(), '0');
    else
        result = digits.substr(0, point) + "." + digits.substr(point);
    if (result.find('.') != std::string::npos) {
        while (result.back() == '0') result.pop_back();
        if (result.back() == '.') result.pop_back();
    }
    return negative ? "-" + result : result;
}
inline NumericValueV1 DecimalForDisplay(const std::string& text) {
    try {
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size() || !std::isfinite(value)) throw std::invalid_argument("finite");
        const auto canonical = CanonicalDecimal(text);
        if (value == 0 && canonical != "0") throw std::invalid_argument("underflow");
        const auto absolute = canonical[0] == '-' ? canonical.substr(1) : canonical;
        const auto integer = absolute.substr(0, absolute.find('.'));
        const std::string maximum = "9007199254740991";
        if (integer.size() > maximum.size() ||
            (integer.size() == maximum.size() && (integer > maximum || (integer == maximum && absolute != integer))))
            throw std::invalid_argument("range");
        std::ostringstream precise;
        precise << std::fixed << std::setprecision(1074) << value;
        return {value, CanonicalDecimal(precise.str()) == canonical};
    } catch (const std::exception&) {
        throw QueryFailure("UNSUPPORTED_TYPE", "Decimal 非有限、下溢或超出浏览器安全数值范围，不能定量绘图");
    }
}
}  // namespace flowsql::visualization
