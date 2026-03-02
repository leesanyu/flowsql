/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2021-12-02 16:42:48
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#ifndef _FLOWSQL_PLUGINS_PROTOCOL_NPI_IRECOGNIZER_H_
#define _FLOWSQL_PLUGINS_PROTOCOL_NPI_IRECOGNIZER_H_

#include <common/network/netbase.h>
#include "iprotocol.h"
#include "layer.h"

namespace flowsql {
namespace protocol {

struct RecognizeContext {
    uint16_t layer = e2i(eLayer::NONE);
    uint16_t level = e2i(eLayer::NONE);
    union {
        uint16_t proto = 0;
        uint16_t dst_port;
        uint16_t w1;
    };
    union {
        uint16_t src_port = 0;
        uint16_t w2;
    };
};

interface IRecognizer {
    virtual int32_t Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size, const protocol::Layers* layers,
                             RecognizeContext* rctx) = 0;
    virtual ~IRecognizer() {}
};

// Just for chain invoke
class Recognized : public IRecognizer {
 public:
    explicit Recognized() : output_(UNKNOWN) {}
    explicit Recognized(int32_t val) : output_(val) {}
    inline void Set(int32_t value) { output_ = value; }
    virtual int32_t Identify(int32_t /* pipeno */, const uint8_t* packet, int32_t packet_size,
                             const protocol::Layers* layers, RecognizeContext* /* rctx */) {
        return output_;
    }

 private:
    int32_t output_ = UNKNOWN;
};

}  // namespace protocol
}  // namespace flowsql

#endif  //_FLOWSQL_PLUGINS_PROTOCOL_NPI_IRECOGNIZER_H_
