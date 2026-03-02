/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2021-01-28 04:04:19
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */

#include "config.h"
// #include <common/logger_helper.h>
#include <yaml-cpp/yaml.h>
#include <common/algo/list_base.hpp>
#include <map>
#include <set>
#include "model.h"

namespace flowsql {
namespace protocol {
namespace {
void gen_scalar_value(Config* the, Item* itm, const std::string key, int32_t vtype, const YAML::Node& vnode) {
    itm->key = the->Mbuf()->Copy(key.c_str(), key.length());
    itm->next = nullptr;
    switch (vtype) {
        case e2i(Item::eValue::Number):
            itm->itype = Item::eValue::Number;
            itm->number.qword = vnode.as<uint64_t>();
            break;
        case e2i(Item::eValue::RegexString):
        case e2i(Item::eValue::EnumerateString):
            itm->itype = static_cast<Item::eValue>(vtype);
            itm->number.qword = 0;
            itm->str = vnode.as<std::string>();
            break;
        default:
            break;
    }
}

void gen_sequence_value(Config* the, Item* itm, const std::string key, int32_t vtype, const YAML::Node& vnode) {
    itm->key = the->Mbuf()->Copy(key.c_str(), key.length());
    itm->next = nullptr;
    switch (vtype) {
        case e2i(Item::eValue::Number):
            itm->itype = Item::eValue::MultiNumber;
            itm->number.qword = 0;
            for (auto& n : vnode) {
                Item::integer nv;
                nv.qword = n.as<uint64_t>();
                itm->numbers.push_back(nv);
            }
            break;
        default:
            break;
    }
}
}  // namespace

Config::Config() {
    dictionary_ = new Dictionary;
    definitions_ = nullptr;
}

Config::~Config() {
    delete dictionary_;
    dictionary_ = nullptr;
}

Dictionary::Dictionary() {
    for (int32_t pos = 0; pos < 65536; ++pos) {
        entries_[pos] = &empty_entry_;
    }
}

Dictionary::~Dictionary() {}

int32_t Dictionary::Count() const { return entries_number_; }

const Entry* Dictionary::Query(int32_t number) const { return entries_[number]; }

int32_t Dictionary::Traverse(std::function<int32_t(const Entry*)> traverser) const {
    int32_t traverser_times = 0;
    for (int32_t pos = 0; pos < 65536; ++pos) {
        Entry* ent = entries_[pos];
        if (ent->number != UNKNOWN) {
            ++traverser_times;
            if (-1 == traverser(ent)) {
                break;
            }
        }
    }

    return traverser_times;
}

int32_t Dictionary::Insert(int32_t number, Entry* entry) {
    if (entries_[number]->number == UNKNOWN) {
        entries_[number] = entry;
        return 0;
    }
    return -1;
}

int32_t Config::Load(const char* file) {
    YAML::Node yaml = YAML::LoadFile(file);
    std::set<int32_t> pro_ids;
    std::set<std::string> pro_names;
    // Layer or Protocol
    for (YAML::const_iterator root = yaml.begin(); root != yaml.end(); ++root) {
        for (YAML::const_iterator pro = root->second.begin(); pro != root->second.end(); ++pro) {
            auto keynode = pro->begin();
            std::string name = keynode->first.as<std::string>();
            int32_t proid = keynode->second.as<int32_t>();
            if (pro_ids.find(proid) != pro_ids.end()) {
                // LOG_W() << "Protocol ID:" << id << " already existed.";
                continue;
            }

            if (pro_names.find(name) != pro_names.end()) {
                // LOG_W() << "Protocol name:" << name << " already existed.";
                continue;
            }

            std::string desc_ch = name;
            std::string desc_en = name;
            int32_t parents = proid;
            try {
                desc_ch = pro->operator[]("desc_ch").as<std::string>();
                desc_en = pro->operator[]("desc_en").as<std::string>();
            } catch (...) {
                // LOG_W() << "The descrition of Protocol " << name << ":" << proid << " is deficiency.";
                continue;
            }

            try {
                parents = pro->operator[]("parents").as<int32_t>();
            } catch (...) {
                // parents == proid
            }

            Definition* def = GenerateProtocolNode(proid, parents, name, desc_ch, desc_en);
            LIST_INSERT_TAIL(definitions_, def);

            auto fts = pro->operator[]("features");
            for (YAML::const_iterator ft = fts.begin(); ft != fts.end(); ++ft) {
                Feature* ftr = feature_pool_.Alloc();
                memset(ftr, 0, sizeof(Feature));

                auto ftnode = ft->operator[]("feature");
                for (YAML::const_iterator it = ftnode.begin(); it != ftnode.end(); ++it) {
                    auto key = it->begin()->first.as<std::string>();
                    auto vtype = Model::GetValueType(key.c_str());
                    Item* itm = item_pool_.Alloc();
                    switch (it->begin()->second.Type()) {
                        case YAML::NodeType::Scalar:
                            gen_scalar_value(this, itm, key, vtype, it->begin()->second);
                            break;
                        case YAML::NodeType::Sequence:
                            gen_sequence_value(this, itm, key, vtype, it->begin()->second);
                            break;
                        default:
                            // Error
                            // LOG_W() << "The features of Protocol " << name << ":" << proid << " is invalid.";
                            continue;
                    }
                    LIST_INSERT_TAIL(ftr->items, itm);
                }
                LIST_INSERT_TAIL(def->features, ftr);
            }
        }
    }

    return 0;
}

Definition* Config::GenerateProtocolNode(int32_t proid, int32_t pproid, const std::string& proname,
                                         const std::string& desc_ch, const std::string& desc_en) {
    auto def = definition_pool_.Alloc();
    def->number = proid;
    def->parents = pproid;
    def->name = buffer_pool_.Copy(proname.c_str(), proname.length());
    def->desc_ch = buffer_pool_.Copy(desc_ch.c_str(), desc_ch.length());
    def->desc_en = buffer_pool_.Copy(desc_en.c_str(), desc_en.length());
    def->features = nullptr;
    def->next = nullptr;
    dictionary_->Insert(proid, def);
    return def;
}

IDictionary* Config::Dict() { return dictionary_; }

int32_t Config::Modeling(std::function<int32_t(Definition*)> model) { return model(definitions_); }

}  // namespace protocol
}  // namespace flowsql