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

#ifndef _FLOWSQL_PLUGINS_PROTOCOL_NPI_MODEL_H_
#define _FLOWSQL_PLUGINS_PROTOCOL_NPI_MODEL_H_

#include <stdint.h>
#include <map>
#include <string>

namespace flowsql {
namespace protocol {

// Declaration
struct Item;
struct Feature;
struct Definition;
class Engine;

class Model {
 public:
    class KeyTyper {
     public:
        KeyTyper();
        int32_t operator()(const char* key);

     protected:
        std::map<std::string, int32_t> keytype_;
    };

    class ValueTyper {
     public:
        ValueTyper();
        int32_t operator()(const char* key);

     protected:
        std::map<std::string, int32_t> valtype_;
    };

    explicit Model(Engine* engine);
    int32_t operator()(Definition* defs);
    static int32_t GetKeyType(const char* key);
    static int32_t GetValueType(const char* key);

 protected:
    int32_t Convert(int32_t number, Feature* feature);

 protected:
    Engine* engine_ = nullptr;
};

}  // namespace protocol
}  // namespace flowsql

#endif  //_FLOWSQL_PLUGINS_PROTOCOL_NPI_MODEL_H_
