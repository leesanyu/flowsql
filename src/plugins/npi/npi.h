/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2021-01-24 14:04:58
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#ifndef _FLOWSQL_PLUGINS_PROTOCOL_NPI_NPI_H_
#define _FLOWSQL_PLUGINS_PROTOCOL_NPI_NPI_H_

#include <common/guid.h>
#include <common/typedef.h>
#include <common/loader.hpp>
#include "iprotocol.h"

namespace flowsql {

namespace protocol {
class Dictionary;
class NetworkLayer;
class Engine;
class Config;
}  // namespace protocol

class NetworkProtocolIdentify : public IPlugin, public IProtocol {
 public:
    NetworkProtocolIdentify();
    ~NetworkProtocolIdentify();

    // IPlugin
    virtual int Option(const char* arg);
    virtual int Load();    // do not call any interface in this func.
    virtual int Unload();  // do not call any interface in this func.

    // IProrocol
    virtual void Concurrency(int32_t number);
    /*
    Return value:
       0 : unknown protocol
     > 0 : protocol No.
    */
    virtual protocol::Protocol Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size,
                                        const protocol::Layers* layers);

    virtual int32_t Layer(int32_t pipeno, const uint8_t* packet, int32_t packet_size, protocol::Layers* layers);

    virtual protocol::IDictionary* Dictionary();

 protected:
    protocol::Config* config_ = nullptr;
    protocol::NetworkLayer* layer_ = nullptr;
    protocol::Engine* engine_ = nullptr;
};

}  // namespace flowsql

#endif  //_FLOWSQL_PLUGINS_PROTOCOL_NPI_NPI_H_
