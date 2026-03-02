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

#include "bitmapmatch.h"

namespace flowsql {
namespace protocol {
int32_t BitmapRecognizer::Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size,
                                   const protocol::Layers* layers, RecognizeContext* rctx) {
    return entries_[rctx->proto]->Identify(pipeno, packet, packet_size, layers, rctx);
}

int32_t BitmapDualRecognizer::Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size,
                                       const protocol::Layers* layers, RecognizeContext* rctx) {
    // TCP/UDP/SCTP
    int32_t pro = eLayer::NONE;
    IRecognizer* dst_recognizer = entries_[rctx->w1];
    IRecognizer* src_recognizer = entries_[rctx->w2];

    if (dst_recognizer == src_recognizer) {
        pro = dst_recognizer->Identify(pipeno, packet, packet_size, layers, rctx);
    } else {
        pro = dst_recognizer->Identify(pipeno, packet, packet_size, layers, rctx);
        if (!pro) {
            pro = src_recognizer->Identify(pipeno, packet, packet_size, layers, rctx);
        }
    }

    return pro;
}
}  // namespace protocol
}  // namespace flowsql
