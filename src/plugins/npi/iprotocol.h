/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2021-01-24 14:29:42
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */

#ifndef _FLOWSQL_PLUGINS_PROTOCOL_NPI_IPROTOCOL_H_
#define _FLOWSQL_PLUGINS_PROTOCOL_NPI_IPROTOCOL_H_

#include <common/guid.h>
#include <common/network/netbase.h>
#include <common/typedef.h>
#include <stdint.h>
#include <functional>
#include <memory>

namespace flowsql {
// {8F701981-9915-42BE-8126-186FE17449F3}
const Guid IID_PROTOCOL = {0x8f701981, 0x9915, 0x42be, {0x81, 0x26, 0x18, 0x6f, 0xe1, 0x74, 0x49, 0xf3}};

namespace protocol {

const int32_t MAX_LAYERS = 15;

enum eNumber { POSSIBLE = -1, UNKNOWN = 0 };

struct Entry {
    int32_t number;
    int32_t parents;
    const char* name;
    const char* desc_en;
    const char* desc_ch;
};

struct Layers {
    uint16_t layercount;  // Layer count
    uint16_t payload;
    struct {
        uint16_t offset;
        eLayer layer;  // eLayer
    } layers[MAX_LAYERS];

    union Levels {
        uint64_t qword = 0;
        struct {
            uint8_t count;
            uint8_t degree[7];
        };
    };

    inline Levels operator() (uint8_t layer) {
        Levels levels;
        for (int32_t dgr = 0; dgr < layercount; ++dgr) {
            if (layer == layers[dgr].layer) {
                levels.degree[levels.count++] = dgr;
            }
        }
        return levels;
    }

    inline Levels operator() (uint8_t layer1, uint8_t layer2) {
        Levels levels;
        for (int32_t dgr = 0; dgr < layercount; ++dgr) {
            if (layer1 == layers[dgr].layer || layer1 == layers[dgr].layer) {
                levels.degree[levels.count++] = dgr;
            }
        }
        return levels;
    }

    template <typename Header>
    inline const Header* Get(const uint8_t* packet, int32_t packet_size, uint8_t degree) const {
        if (degree < layercount) {
            return reinterpret_cast<const Header*>(packet + layers[degree].offset);
        }

        return nullptr;
    }

    template <typename Header>
    inline const Header* Forward(const uint8_t* packet, int32_t packet_size) const {
        for (int32_t dgr = 0; dgr < layercount; ++dgr) {
            if (Header::level == layers[dgr].layer) {
                return reinterpret_cast<const Header*>(packet + layers[dgr].offset);
            }
        }
        return nullptr;
    }

    template <typename Header>
    inline const Header* Backward(const uint8_t* packet, int32_t packet_size) const {
        for (int32_t dgr = layercount - 1; dgr >= 0; --dgr) {
            if (Header::level == layers[dgr].layer) {
                return reinterpret_cast<const Header*>(packet + layers[dgr].offset);
            }
        }
        return nullptr;
    }

    template <typename Header>
    inline const Header* Top(const uint8_t* packet, int32_t packet_size) const {
        return reinterpret_cast<const Header*>(packet + layers[layercount - 1].offset);
    }

    inline eLayer Top() const { return layers[layercount - 1].layer; }

    // Payload
    inline uint16_t Payload(const uint8_t* packet, int32_t packet_size) const { return packet_size - payload; }
    inline const uint8_t* Data(const uint8_t* packet, int32_t packet_size) const { return packet + payload; }
};

struct Protocol {
    Protocol() : id(0), subid(0) {}
    Protocol(uint16_t proto) : id(proto), subid(0) {}
    Protocol(uint16_t proto, uint16_t subpro) : id(proto), subid(subpro) {}
    inline operator uint32_t() { return (id << 16) | subid; }

    uint16_t id;     // protocol number, like ICMP
    uint16_t subid;  // sub protocol number, like ICMP-ECHO
};

interface IDictionary {
    virtual int32_t Count() const = 0;
    virtual const Entry* Query(int32_t number) const = 0;
    virtual int32_t Traverse(std::function<int32_t(const Entry*)> traverser) const = 0;
};

}  // namespace protocol

interface IProtocol {
    virtual ~IProtocol() {}

    virtual void Concurrency(int32_t number) = 0;
    /*
    Return value:
       0 : unknown protocol
     > 0 : protocol No.
    */
    virtual protocol::Protocol Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size,
                                        const protocol::Layers* layers) = 0;

    virtual int32_t Layer(int32_t pipeno, const uint8_t* packet, int32_t packet_size, protocol::Layers* layers) = 0;

    virtual protocol::IDictionary* Dictionary() = 0;
};

}  // namespace flowsql

#endif  //_FLOWSQL_PLUGINS_PROTOCOL_NPI_IPROTOCOL_H_
