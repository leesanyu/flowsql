/*
 * Author       : LIHUO
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Date         : 2020-10-22 16:56:05
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */

#include <map>
#include <string.h>
#include <plugins/npi/iprotocol.h>
#include <stdio.h>
#include <common/launcher.hpp>
#include <common/loader.hpp>
#include <common/toolkit.hpp>

DEFINE_string(packetfile, "", "packetfile");
DEFINE_string(protocolfile, "", "protocolfile");

namespace {
struct packet_header {
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint32_t caplen;
    uint32_t len;
};
}  // namespace

const size_t BUFSIZE = 4 * 1024 * 1024;
// Reader : [] (const uint8_t* data, int32_t size) -> int32_t
template <typename Reader>
int32_t read_packets(const std::string& packets, Reader reader) {
    FILE* file = fopen(packets.c_str(), "rb");
    uint8_t* buffer = new uint8_t[BUFSIZE];
    fseek(file, 24, 0);
    size_t buffer_offset = 0;
    size_t readbytes = 0;
    size_t maxreadbytes = BUFSIZE - buffer_offset;
    do {
        maxreadbytes = BUFSIZE - buffer_offset;
        readbytes = fread(buffer + buffer_offset, 1, maxreadbytes, file);

        // get every packets
        size_t allbytes = readbytes + buffer_offset;
        for (size_t offset = 0; offset < allbytes;) {
            if (sizeof(packet_header) > allbytes - offset) {
                memcpy(buffer, buffer + offset, allbytes - offset);
                buffer_offset = allbytes - offset;
                break;
            }
            packet_header* header = (packet_header*)(buffer + offset);
            if (sizeof(packet_header) + header->caplen > allbytes - offset) {
                memcpy(buffer, buffer + offset, allbytes - offset);
                buffer_offset = allbytes - offset;
                break;
            }

            if (-1 == reader(buffer + offset + sizeof(packet_header), header->caplen)) {
                break;
            }

            offset += sizeof(packet_header) + header->caplen;
        }
    } while (readbytes == maxreadbytes);

    delete[] buffer;
    return 0;
}

int main(int argc, char* argv[]) {
    flowsql::Launcher launcher;
    launcher.Launch(argc, argv, []() -> int32_t {
        // for test load the plugins
        flowsql::PluginLoader* _loader = flowsql::PluginLoader::Single();
        const char* _plugins[] = {"libflowsql_npi.so"};
        std::string _protocolfile = FLAGS_protocolfile;
        char npioption[1024] = {0};
        snprintf(npioption, 1024, "{\"ldfile\":\"%s\"}", _protocolfile.c_str());
        const char* _options[] = {npioption};
        if (_loader->Load(flowsql::get_absolute_process_path(), _plugins, _options, sizeof(_plugins) / sizeof(char*)) < 0) {
            return -1;
        }
        flowsql::IProtocol* proto = (flowsql::IProtocol*)_loader->First(flowsql::IID_PROTOCOL);
        if (!proto) {
            printf("No IProtocol implementation found\n");
            _loader->Unload();
            return -1;
        }
        flowsql::protocol::IDictionary* dict = proto->Dictionary();
        std::string packetfile = FLAGS_packetfile;
        if (packetfile.empty()) {
            printf("Usage: test_npi --packetfile=<pcap> --protocolfile=<yml>\n");
            _loader->Unload();
            return 0;
        }
        flowsql::protocol::Layers layers;
        uint64_t packetno = 0;
        proto->Concurrency(2);
        std::map<uint32_t, uint32_t> stat;
        read_packets(packetfile,
                     [&packetno, &stat, &proto, &dict, &layers](const uint8_t* packet, int32_t size) -> int32_t {
                         flowsql::protocol::Protocol proid = 0;
                         proto->Layer(0, packet, size, &layers);
                         proid = proto->Identify(0, packet, size, &layers);
                         ++packetno;
                         if (0 != proid)
                             printf("%llu:%d:%d:%s\n", packetno, size, proid.subid, dict->Query(proid.subid)->name);
                         ++stat[proid];
                         return 0;
                     });

        for (auto& pair : stat) {
            uint16_t pid = pair.first >> 16;
            uint16_t psub = pair.first & 0x0000ffff;
            printf("%d-%s:%d-%s:%d\n", pid, dict->Query(pid)->name, psub, dict->Query(psub)->name, pair.second);
        }

        _loader->Unload();
        return 0;
    });

    return 0;
}
