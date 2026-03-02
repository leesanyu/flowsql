/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2021-01-29 02:20:25
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */

#include "model.h"
#include "config.h"
#include "engine.h"

namespace flowsql {
namespace protocol {
namespace {
inline eLayer keytype2level(int32_t keytype) { return static_cast<eLayer>(keytype); }
}  // namespace

Model::KeyTyper::KeyTyper() {
    keytype_["l2.protocol"] = eLayer::L2;
    keytype_["l3.protocol"] = eLayer::L3;
    keytype_["ipv4.protocol"] = eLayer::IPv4;
    keytype_["ipv6.protocol"] = eLayer::IPv6;
    keytype_["tcp.port"] = eLayer::TCP;
    keytype_["udp.port"] = eLayer::UDP;
    keytype_["l4.port"] = eLayer::L4;
    keytype_["port"] = eLayer::L4;
    keytype_["l2.payload.regex"] = eLayer::L2;
    keytype_["l3.payload.regex"] = eLayer::L3;
    keytype_["ipv4.payload.regex"] = eLayer::IPv4;
    keytype_["ipv6.payload.regex"] = eLayer::IPv6;
    keytype_["l4.payload.regex"] = eLayer::L4;
    keytype_["tcp.payload.regex"] = eLayer::TCP;
    keytype_["udp.payload.regex"] = eLayer::UDP;
    keytype_["payload.regex"] = eLayer::TOP;
    keytype_["l2.payload.enumerate"] = eLayer::L2;
    keytype_["l3.payload.enumerate"] = eLayer::L3;
    keytype_["ipv4.payload.enumerate"] = eLayer::IPv4;
    keytype_["ipv6.payload.enumerate"] = eLayer::IPv6;
    keytype_["l4.payload.enumerate"] = eLayer::L4;
    keytype_["tcp.payload.enumerate"] = eLayer::TCP;
    keytype_["udp.payload.enumerate"] = eLayer::UDP;
    keytype_["payload.enumerate"] = eLayer::TOP;
}

Model::ValueTyper::ValueTyper() {
    valtype_["l2.protocol"] = e2i(Item::eValue::Number);
    valtype_["l3.protocol"] = e2i(Item::eValue::Number);
    valtype_["ipv4.protocol"] = e2i(Item::eValue::Number);
    valtype_["ipv6.protocol"] = e2i(Item::eValue::Number);
    valtype_["l4.port"] = e2i(Item::eValue::Number);
    valtype_["port"] = e2i(Item::eValue::Number);
    valtype_["tcp.port"] = e2i(Item::eValue::Number);
    valtype_["udp.port"] = e2i(Item::eValue::Number);
    valtype_["l2.payload.regex"] = e2i(Item::eValue::RegexString);
    valtype_["l3.payload.regex"] = e2i(Item::eValue::RegexString);
    valtype_["ipv4.payload.regex"] = e2i(Item::eValue::RegexString);
    valtype_["ipv6.payload.regex"] = e2i(Item::eValue::RegexString);
    valtype_["l4.payload.regex"] = e2i(Item::eValue::RegexString);
    valtype_["tcp.payload.regex"] = e2i(Item::eValue::RegexString);
    valtype_["udp.payload.regex"] = e2i(Item::eValue::RegexString);
    valtype_["payload.regex"] = e2i(Item::eValue::RegexString);
    valtype_["l2.payload.enumerate"] = e2i(Item::eValue::EnumerateString);
    valtype_["l3.payload.enumerate"] = e2i(Item::eValue::EnumerateString);
    valtype_["ipv4.payload.enumerate"] = e2i(Item::eValue::EnumerateString);
    valtype_["ipv6.payload.enumerate"] = e2i(Item::eValue::EnumerateString);
    valtype_["l4.payload.enumerate"] = e2i(Item::eValue::EnumerateString);
    valtype_["tcp.payload.enumerate"] = e2i(Item::eValue::EnumerateString);
    valtype_["udp.payload.enumerate"] = e2i(Item::eValue::EnumerateString);
    valtype_["payload.enumerate"] = e2i(Item::eValue::EnumerateString);
}

int32_t Model::KeyTyper::operator()(const char* key) {
    auto found = keytype_.find(key);
    if (found != keytype_.end()) {
        return found->second;
    }
    return 0;
}
int32_t Model::ValueTyper::operator()(const char* key) {
    auto found = valtype_.find(key);
    if (found != valtype_.end()) {
        return found->second;
    }
    return e2i(Item::eValue::INVALID);
}

Model::Model(Engine* engine) : engine_(engine) {}

int32_t Model::operator()(Definition* defs) {
    for (Definition* def = defs; def; def = def->next) {
        for (Feature* ftr = def->features; ftr; ftr = ftr->next) {
            Convert(def->number, ftr);
        }
    }

    return engine_->Ready();
}

int32_t Model::Convert(int32_t number, Feature* feature) {
    for (Item* itm = feature->items; itm; itm = itm->next) {
        int32_t kt = GetKeyType(itm->key);
        switch (kt) {
            case eLayer::L2:
            case eLayer::L3:
            case eLayer::L4:
            case eLayer::IPv4:
            case eLayer::IPv6:
            case eLayer::TCP:
            case eLayer::UDP:
                if (-1 == engine_->Build()->Level(keytype2level(kt))) {
                    // Only one level
                    return -1;
                }
                if (itm->itype == Item::eValue::Number) {
                    engine_->Build()->Proport(itm->number.word);
                } else if (itm->itype == Item::eValue::MultiNumber) {
                    for (auto& num : itm->numbers) {
                        engine_->Build()->Proport(num.word);
                    }
                } else {
                    // RegexString|MatchString|EnumerateString
                    engine_->Build()->String(e2i(itm->itype), itm->str);
                }
                break;
            case eLayer::TOP:
                // RegexString
                if (Item::eValue::RegexString == itm->itype) {
                    engine_->Build()->String(e2i(itm->itype), itm->str);
                } else {
                    // Except Regex, Layer must be specified.
                }
                break;
            default:
                return -1;
        }
    }

    return engine_->Build()->Commit(number);
}

int32_t Model::GetKeyType(const char* key) {
    static KeyTyper sKeytyper;
    return sKeytyper(key);
}

int32_t Model::GetValueType(const char* key) {
    static ValueTyper sValtyper;
    return sValtyper(key);
}

}  // namespace protocol
}  // namespace flowsql
