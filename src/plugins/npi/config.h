/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2021-11-15 04:31:50
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2021-01-28 03:03:51
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#ifndef _FLOWSQL_PLUGINS_PROTOCOL_NPI_CONFIG_H_
#define _FLOWSQL_PLUGINS_PROTOCOL_NPI_CONFIG_H_

#include <common/algo/bitmap.hpp>
#include <common/algo/objects_pool.hpp>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "iprotocol.h"

namespace flowsql {
namespace protocol {

class Dictionary : public IDictionary {
 public:
    Dictionary();
    ~Dictionary();

    // interface
    virtual int32_t Count() const;
    virtual const Entry* Query(int32_t number) const;
    virtual int32_t Traverse(std::function<int32_t(const Entry*)> traverser) const;

    // Set
    int32_t Insert(int32_t number, Entry* entry);

 protected:
    int32_t entries_number_ = 0;
    int32_t min_id_ = 65535;
    int32_t max_id_ = 0;
    Entry empty_entry_ = {
        .number = UNKNOWN, .parents = UNKNOWN, .name = "UNKNOWN", .desc_en = "UNKNOWN", .desc_ch = "UNKNOWN"};
    Bitmap<65536, Entry*> entries_;
};

struct Item {
    enum class eValue { INVALID = -1, Number = 0, MultiNumber, EnumerateString, RegexString };
    const char* key;
    eValue itype;  // 0 Number 1:Multi-number 2:RegexString

    union integer {
        uint64_t qword;
        uint32_t dword;
        uint16_t word;  // for l2.protocol udp.port tcp.port
        uint8_t byte;   // for l3.protocol
    } number;
    std::vector<integer> numbers;
    std::string str;

    Item* next;
};

struct Feature {
    Item* items;
    Feature* next;
};

struct Definition : public Entry {
    Feature* features;
    Definition* next;
};

class Config {
 public:
    typedef BufferPool<1024 * 1024> MemoryBuffer;

    Config();
    ~Config();
    int32_t Load(const char* file);
    IDictionary* Dict();
    int32_t Modeling(std::function<int32_t(Definition*)> model);
    inline MemoryBuffer* Mbuf() { return &buffer_pool_; }

 protected:
    Definition* GenerateProtocolNode(int32_t proid, int32_t parents, const std::string& proname,
                                     const std::string& desc_ch, const std::string& desc_en);

 protected:
    Dictionary* dictionary_;
    Definition* definitions_;
    MemoryBuffer buffer_pool_;
    ObjectsPool<Definition, 1024> definition_pool_;
    ObjectsPool<Feature, 1024> feature_pool_;
    ObjectsPool<Item, 1024> item_pool_;
};

}  // namespace protocol
}  // namespace flowsql

#endif  //_FLOWSQL_PLUGINS_PROTOCOL_NPI_CONFIG_H_
