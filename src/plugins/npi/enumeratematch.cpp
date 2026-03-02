/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2021-01-24 14:57:26
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */

#include "enumeratematch.h"
#include <tuple>

namespace flowsql {
namespace protocol {

namespace {
const char ENUM_DELIMITER = ':';

std::tuple<uint64_t, int32_t, int32_t> split2enumerateparameter(const std::string& enumstr) {
    auto fpos_1st = enumstr.find(ENUM_DELIMITER);
    if (std::string::npos != fpos_1st && 0 != fpos_1st) {
        auto fpos_2nd = enumstr.find(ENUM_DELIMITER, fpos_1st + 1);
        if (std::string::npos != fpos_2nd && fpos_2nd > fpos_1st + 1) {
            // Create EnumerateRecognizer
            auto numberstr = enumstr.substr(0, fpos_1st);
            auto offsetstr = enumstr.substr(fpos_1st + 1, fpos_2nd - fpos_1st);
            auto lengthstr = enumstr.substr(fpos_2nd + 1);
            if (numberstr == "*") {
                return std::make_tuple(-1, std::stoi(offsetstr), std::stoi(lengthstr));
            }
            return std::make_tuple(std::stoll(numberstr), std::stoi(offsetstr), std::stoi(lengthstr));
        }
    }

    return std::make_tuple(0, 0, 0);
}
}  // namespace

EnumerateRecognizerPool::~EnumerateRecognizerPool() {
    for (auto& item : pool_) {
        delete item;
    }
    pool_.clear();
}

EnumerateRecognizer* EnumerateRecognizerPool::Create(const std::string& enumstr, int32_t proid) {
    // enum:offset:length
    EnumerateRecognizer* enumer = nullptr;
    uint64_t number = 0;
    int32_t offset = 0;
    int32_t length = 0;
    auto params = split2enumerateparameter(enumstr);
    std::tie(number, offset, length) = params;
    switch (length) {
        case 1:
            enumer = new SmallEnumerateRecognizer<uint8_t>(offset);
            break;
        case 2:
            enumer = new GrandEnumerateRecognizer<uint16_t>(offset, length);
            break;
        case 3:
        case 4:
            enumer = new GrandEnumerateRecognizer<uint32_t>(offset, length);
            break;
        case 5:
        case 6:
        case 7:
        case 8:
            enumer = new GrandEnumerateRecognizer<uint64_t>(offset, length);
        default:
            break;
    }

    if (enumer) {
        if (-1 == number) {
            enumer->Default(proid);
        } else {
            enumer->Set(number, proid);
        }
        pool_.push_back(enumer);
    }

    return enumer;
}

int32_t EnumerateRecognizerPool::Update(EnumerateRecognizer* enumer, const std::string& enumstr, int32_t proid) {
    uint64_t number = 0;
    int32_t position = 0;
    int32_t width = 0;
    auto params = split2enumerateparameter(enumstr);
    std::tie(number, position, width) = params;
    if (number != -1 && enumer->Position() == position && enumer->Width() == width) {
        enumer->Set(number, proid);
        return 0;
    }
    return -1;
}

}  // namespace protocol
}  // namespace flowsql
