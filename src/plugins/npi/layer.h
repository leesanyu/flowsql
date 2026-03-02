/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2021-12-02 16:36:27
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */

#ifndef _FLOWSQL_PLUGINS_PROTOCOL_NPI_LAYER_H_
#define _FLOWSQL_PLUGINS_PROTOCOL_NPI_LAYER_H_

#include "iprotocol.h"

namespace flowsql {
namespace protocol {

const uint32_t INVALID = 0;

class Delamination {
 public:
    union Result {
        uint32_t dword;
        struct {
            uint16_t offset;
            eLayer next;
        };
    };
    //  typedef std::function<Result(const uint8_t*, int32_t size, uint16_t base_length)> fnParser;
    typedef Result (*fnParser)(eLayer below, const uint8_t*, int32_t size, uint16_t base_length);

 public:
    Delamination(uint16_t base_length, fnParser parser) : parser_(parser), base_length_(base_length) {}
    inline Result operator()(eLayer below, const uint8_t* data, int32_t size) {
        return parser_(below, data, size, base_length_);
    }

 private:
    uint16_t base_length_;
    fnParser parser_;
};

class NetworkLayer {
 public:
    NetworkLayer();
    ~NetworkLayer();
    int32_t Layer(const uint8_t* packet, int32_t packet_size, protocol::Layers* layers);

 protected:
    Delamination* parsers_map_[eLayer::MAX] = {nullptr};
};

}  // namespace protocol
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_PROTOCOL_NPI_LAYER_H_
