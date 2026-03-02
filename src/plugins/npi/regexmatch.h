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

#ifndef _FLOWSQL_PLUGINS_PROTOCOL_NPI_REGEXMATCH_H_
#define _FLOWSQL_PLUGINS_PROTOCOL_NPI_REGEXMATCH_H_

#include <hs.h>
#include <set>
#include <vector>
#include "irecognizer.h"

namespace flowsql {
namespace protocol {

const int32_t MAX_REGEX_CONCURRENCY = 16;

class RegexRecognizer : public IRecognizer {
 public:
    RegexRecognizer();
    ~RegexRecognizer();

    // interface
    virtual int32_t Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size, const protocol::Layers* layers,
                             RecognizeContext* rctx);

    // Insert regex expression
    int32_t Regex(const std::string& regexpr, const std::vector<int32_t>& proports, int32_t level, int32_t proid);

    // Compile Hyperscan database
    int32_t Ready(int32_t concurrency);

 protected:
    class IConfirmer {
     public:
        explicit IConfirmer(int32_t proid) : proid_(proid) {}
        virtual ~IConfirmer() {}
        virtual int32_t Confirm(const RecognizeContext* rctx) const { return proid_; }

     protected:
        int32_t proid_;
    };

    class LayerConfirmer : public IConfirmer {
     public:
        LayerConfirmer(int32_t proid, int32_t level) : IConfirmer(proid), level_(level) {}

        virtual int32_t Confirm(const RecognizeContext* rctx) const {
            if (rctx->level == level_ || rctx->layer == level_) return proid_;
            return eLayer::NONE;
        }

        // L2={ETHERNET,VLAN}, L3={IPv4,IPv6}, L4={TCP,UDP,SCTP}
     protected:
        int32_t level_;
    };

    class SinglePortConfirmer : public LayerConfirmer {
     public:
        SinglePortConfirmer(int32_t proid, int32_t level, int32_t proport)
            : LayerConfirmer(proid, level), proport_(proport) {}

        virtual int32_t Confirm(const RecognizeContext* rctx) const {
            if (rctx->level == level_) {
                if (rctx->w1 == proport_ || rctx->w2 == proport_) return proid_;
            }

            return eLayer::NONE;
        }

     protected:
        int32_t proport_;
    };

    class MultiPortConfirmer : public LayerConfirmer {
     public:
        MultiPortConfirmer(int32_t proid, int32_t level, std::vector<int32_t> proports) : LayerConfirmer(proid, level) {
            for (auto& proport : proports) {
                ports_.insert(proport);
            }
        }
        virtual int32_t Confirm(const RecognizeContext* rctx) const {
            if (rctx->level == level_) {
                if (ports_.find(rctx->w1) != ports_.end() || ports_.find(rctx->w2) != ports_.end()) return proid_;
            }

            return eLayer::NONE;
        }

     protected:
        std::set<int32_t> ports_;
    };

    std::vector<std::string> patterns_;
    std::vector<const char*> patterns_ptr_;
    std::vector<uint32_t> patterns_flag_;
    std::vector<uint32_t> patterns_id_;
    std::vector<IConfirmer*> confirmers_;

    // Hyperscan ...
 public:
    class runtimecontext {
     public:
        runtimecontext(const std::vector<IConfirmer*>& confirmers, const RecognizeContext* rctx)
            : refconfirmers_(confirmers), rctx_(rctx) {}

        inline int32_t confirm(uint32_t id) {
            if (id < refconfirmers_.size())
                value_ = refconfirmers_[id]->Confirm(rctx_);
            else
                return -1;
            return 0;
        }

        inline int32_t get() const { return value_; }

     private:
        const std::vector<IConfirmer*>& refconfirmers_;
        const RecognizeContext* rctx_;
        int32_t value_ = eLayer::NONE;
    };

 protected:
    hs_database_t* hyper_database_ = nullptr;
    hs_scratch_t* hyper_scratchs_[MAX_REGEX_CONCURRENCY] = {nullptr};
};

}  // namespace protocol
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_PROTOCOL_NPI_REGEXMATCH_H_
