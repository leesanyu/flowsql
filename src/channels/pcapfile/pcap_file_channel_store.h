// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_CHANNELS_PCAPFILE_PCAP_FILE_CHANNEL_STORE_H_
#define _FLOWSQL_CHANNELS_PCAPFILE_PCAP_FILE_CHANNEL_STORE_H_

#include <string>
#include <vector>

struct sqlite3;

namespace flowsql::channels::pcapfile {

struct PcapFileChannelRecord {
    std::string type;
    std::string name;
    std::string option;
};

class PcapFileChannelStore {
 public:
    PcapFileChannelStore() = default;
    ~PcapFileChannelStore();

    PcapFileChannelStore(const PcapFileChannelStore&) = delete;
    PcapFileChannelStore& operator=(const PcapFileChannelStore&) = delete;

    int Open(const std::string& db_path, std::string* error);
    int LoadAll(std::vector<PcapFileChannelRecord>* records, std::string* error);
    int Insert(const PcapFileChannelRecord& record, std::string* error);
    int Update(const PcapFileChannelRecord& record, std::string* error);
    int Erase(const std::string& type, const std::string& name, std::string* error);
    void Close();

 private:
    sqlite3* db_ = nullptr;
};

}  // namespace flowsql::channels::pcapfile

#endif  // _FLOWSQL_CHANNELS_PCAPFILE_PCAP_FILE_CHANNEL_STORE_H_
