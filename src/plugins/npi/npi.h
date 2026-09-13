// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_PROTOCOL_NPI_NPI_H_
#define _FLOWSQL_PLUGINS_PROTOCOL_NPI_NPI_H_

#include <common/guid.h>
#include <common/typedef.h>
#include <common/iplugin.h>

#include <cstdint>
#include <mutex>
#include <vector>

#include "iprotocol.h"

namespace flowsql {

namespace protocol {
class Dictionary;
class NetworkLayer;
class Engine;
class Config;
}  // namespace protocol

class NetworkProtocolIdentify : public IPlugin, public IProtocol, public IProtocolPipelinePoolV1 {
 public:
    NetworkProtocolIdentify();
    ~NetworkProtocolIdentify();

    // IPlugin
    virtual int Option(const char* arg);
    virtual int Load(IQuerier* querier);    // do not call any interface in this func.
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

    // IProtocolPipelinePoolV1
    ProtocolPipelinePoolError Acquire(int32_t* pipeno) override;
    ProtocolPipelinePoolError Release(int32_t pipeno) override;
    int32_t Capacity() const override;
    IProtocol* Protocol() override;

 protected:
    protocol::Config* config_ = nullptr;
    protocol::NetworkLayer* layer_ = nullptr;
    protocol::Engine* engine_ = nullptr;

 private:
    mutable std::mutex pipeline_mutex_;
    int32_t pipeline_capacity_ = 1;
    bool pipeline_available_ = false;
    std::vector<uint8_t> pipeline_leased_;
};

}  // namespace flowsql

#endif  //_FLOWSQL_PLUGINS_PROTOCOL_NPI_NPI_H_
