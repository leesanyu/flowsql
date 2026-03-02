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

#include "regexmatch.h"
#include <assert.h>
#include <string>
#include <tuple>

namespace flowsql {
namespace protocol {
namespace {
const char REGEX_FLAG_DELIMITER = '/';

uint32_t split2regexflags(const std::string& flagstr) {
    uint32_t flags = 0;
    for (auto ch : flagstr) {
        switch (ch) {
            case 'i':
                flags |= HS_FLAG_CASELESS;
                break;
            case 'm':
                flags |= HS_FLAG_MULTILINE;
                break;
            case 's':
                flags |= HS_FLAG_DOTALL;
                break;
            case 'H':
                flags |= HS_FLAG_SINGLEMATCH;
                break;
            case 'V':
                flags |= HS_FLAG_ALLOWEMPTY;
                break;
            case '8':
                flags |= HS_FLAG_UTF8;
                break;
            case 'W':
                flags |= HS_FLAG_UCP;
                break;
            default:
                break;
        }
    }

    return flags;
}

int32_t split2regexstring(const std::string& regexstr, std::function<int32_t(const std::string&, uint32_t)> inserter) {
    auto lflag = regexstr.find(REGEX_FLAG_DELIMITER);
    if (std::string::npos == lflag) {
        return inserter(regexstr, HS_FLAG_DOTALL);
    }
    auto rflag = regexstr.rfind(REGEX_FLAG_DELIMITER);
    if (rflag > lflag + 1) {
        std::string flagstr = regexstr.substr(rflag + 1);
        uint32_t flags = split2regexflags(flagstr);
        for (auto pos = lflag; pos < rflag;) {
            auto fpos = regexstr.find(REGEX_FLAG_DELIMITER, pos + 1);
            std::string regexstring = regexstr.substr(pos + 1, fpos - pos - 1);
            if (-1 == inserter(regexstring, flags)) return -1;
            pos = fpos;  // If not find, fpos = string::npos = size_t(-1) > rflag
        }
    }

    return 0;
}

/*
typedef int (*match_event_handler)(unsigned int id, unsigned long long from, unsigned long long to, unsigned int flags,
void *context)
**/
int hypermatching(unsigned int id, unsigned long long from, unsigned long long to, unsigned int flags, void* context) {
    RegexRecognizer::runtimecontext* rtctx = static_cast<RegexRecognizer::runtimecontext*>(context);
    return rtctx->confirm(id);
}
}  // namespace

RegexRecognizer::RegexRecognizer() {
    patterns_.reserve(65536);
    patterns_ptr_.reserve(65536);
    confirmers_.reserve(65536);
}

RegexRecognizer::~RegexRecognizer() {
    for (auto& scratch : hyper_scratchs_) {
        if (scratch) {
            hs_free_scratch(scratch);
            scratch = nullptr;
        }
    }
    hs_free_database(hyper_database_);
    hyper_database_ = nullptr;

    for (auto& confirm : confirmers_) {
        delete confirm;
        confirm = nullptr;
    }
}

int32_t RegexRecognizer::Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size,
                                  const protocol::Layers* layers, RecognizeContext* rctx) {
    const uint8_t* payload = layers->Data(packet, packet_size);
    uint16_t length = layers->Payload(packet, packet_size);

    if (length > 0) {
        runtimecontext rtctx(confirmers_, rctx);
        hs_error_t err =
            hs_scan(hyper_database_, (const char*)payload, length, 0, hyper_scratchs_[pipeno], hypermatching, &rtctx);
        if (err != HS_SUCCESS) {
            // cerr << "ERROR: Unable to scan packet. Exiting." << endl;
            return -1;
        }
        return rtctx.get();
    } else {
        return eLayer::NONE;
    }
}

int32_t RegexRecognizer::Regex(const std::string& regexpr, const std::vector<int32_t>& proports, int32_t level,
                               int32_t proid) {
    return split2regexstring(
        regexpr, [this, &proports, &level, &proid](const std::string& regexstring, uint32_t flags) -> int32_t {
            this->patterns_.push_back(regexstring);
            this->patterns_ptr_.push_back(patterns_.back().c_str());
            this->patterns_flag_.push_back(flags);
            this->patterns_id_.push_back(patterns_ptr_.size() - 1);

            IConfirmer* confirmer = nullptr;
            if (eLayer::NONE == level) {
                confirmer = new IConfirmer(proid);
            } else {
                auto proport_count = proports.size();
                if (0 == proport_count) {
                    confirmer = new LayerConfirmer(proid, level);
                } else if (1 == proport_count) {
                    confirmer = new SinglePortConfirmer(proid, level, proports.front());
                } else {
                    confirmer = new MultiPortConfirmer(proid, level, proports);
                }
            }
            confirmers_.push_back(confirmer);
            return 0;
        });
}

int32_t RegexRecognizer::Ready(int32_t concurrency) {
    assert(patterns_.size() == patterns_ptr_.size() && patterns_flag_.size() == patterns_ptr_.size() &&
           patterns_ptr_.size() == confirmers_.size());

    hs_compile_error_t* compileErr;
    hs_error_t err = hs_compile_multi(patterns_ptr_.data(), patterns_flag_.data(), patterns_id_.data(),
                                      patterns_ptr_.size(), HS_MODE_BLOCK, nullptr, &hyper_database_, &compileErr);
    if (err != HS_SUCCESS) {
        // ERROR MESSAGE
        hs_free_compile_error(compileErr);
        return -1;
    }

    for (int32_t i = 0; i < concurrency; ++i) {
        err = hs_alloc_scratch(hyper_database_, hyper_scratchs_ + i);
        if (err != HS_SUCCESS) {
            // ERROR MESSAGE : could not allocate scratch space.
            // hs_free_database(hyper_database_);
            // hyper_database_ = nullptr;
            return -1;
        }
    }

    return 0;
}

}  // namespace protocol
}  // namespace flowsql