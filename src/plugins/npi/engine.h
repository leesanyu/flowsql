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

#ifndef _FLOWSQL_PLUGINS_PROTOCOL_NPI_ENGINE_H_
#define _FLOWSQL_PLUGINS_PROTOCOL_NPI_ENGINE_H_

#include <common/int2type.h>
#include <stdint.h>
#include <common/algo/objects_pool.hpp>
#include <functional>
#include <vector>
#include "irecognizer.h"

namespace flowsql {
namespace protocol {

class Config;
// class Model;
struct Layers;
class BitmapRecognizer;
class RegexRecognizer;
class EnumerateRecognizerPool;
class Engine {
 public:
    class Builder {
     public:
        Builder(Engine* owner) : owner_(owner) { prototype_unknown_ = create_recognized_recognizer(UNKNOWN); }

        inline int32_t Level(eLayer level) {
            if (eLayer::NONE == level_) {
                level_ = level;
            } else if (level_ != level) {
                return -1;
            }
            return 0;
        }

        inline void Proport(int32_t pro) { proports_.push_back(pro); }

        inline void String(int32_t strtype, const std::string& str) {
            strtype_ = strtype;
            string_ = str;
        }

        int32_t Commit(int32_t number);

        inline Recognized* create_recognized_recognizer(int32_t val) {
            Recognized* obj = recognized_pool_.Alloc();
            obj->Set(val);
            return obj;
        }

        inline Recognized* get_prototype_unknown() { return prototype_unknown_; }

        inline void clear() {
            level_ = eLayer::NONE;
            strtype_ = 0;
            proports_.clear();
            string_.clear();
        }

     protected:
        int32_t build(int32_t number, IRecognizer* reco);

     protected:
        eLayer level_ = eLayer::NONE;
        int32_t strtype_ = 0;
        std::vector<int32_t> proports_;
        std::string string_;

     private:
        flowsql::ObjectsPool<Recognized, 1024> recognized_pool_;
        Recognized* prototype_unknown_;
        Engine* owner_ = nullptr;
    };
    friend class Builder;

    Engine();
    ~Engine();

    /*0:Successfully Other:failed*/
    int32_t Create(Config* configure);

    Builder* Build();

    int32_t Ready();

    void Concurrency(int32_t number);

    int32_t Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size, const protocol::Layers* layers);

 protected:
    // For Builder
    BitmapRecognizer* Bitmaper(eLayer level);
    RegexRecognizer* Regexer();
    EnumerateRecognizerPool* Enumerater();
    int32_t TraverseRecognizer(eLayer level, std::vector<int32_t> proports, std::function<int32_t(IRecognizer*)>);

 protected:
    int32_t concurrency_ = 1;
    BitmapRecognizer* proport_recognizers_[eLayer::MAX] = {nullptr};
    RegexRecognizer* regex_recognizer_ = nullptr;
    EnumerateRecognizerPool* enum_recognizer_pool_ = nullptr;
    Builder builder_;
};

}  // namespace protocol
}  // namespace flowsql

#endif  //_FLOWSQL_PLUGINS_PROTOCOL_NPI_ENGINE_H_
