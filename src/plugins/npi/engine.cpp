/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2021-01-24 14:57:37
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */

#include "engine.h"
#include <common/network/netbase.h>
#include "config.h"
#include "iprotocol.h"
#include "layer.h"
#include "model.h"

#include "bitmapmatch.h"
#include "enumeratematch.h"
#include "regexmatch.h"

namespace flowsql {
namespace protocol {

int32_t Engine::Builder::Commit(int32_t number) {
    switch (strtype_) {
        case e2i(Item::eValue::RegexString):
            build(number, owner_->Regexer());
            owner_->Regexer()->Regex(string_, proports_, level_, number);
            break;
        case e2i(Item::eValue::EnumerateString):
            if (0 == owner_->TraverseRecognizer(level_, proports_, [this, number](IRecognizer* reco) -> int32_t {
                    if (reco) {
                        EnumerateRecognizer* enumer = dynamic_cast<EnumerateRecognizer*>(reco);
                        if (enumer) {
                            owner_->Enumerater()->Update(enumer, string_, number);
                            return 1;
                        }
                    }
                    return 0;
                })) {
                build(number, owner_->Enumerater()->Create(string_, number));
            }

            break;
        default:
            build(number, nullptr);
            break;
    }

    clear();
    return 0;
}

int32_t Engine::Builder::build(int32_t number, IRecognizer* reco) {
    // Proports
    switch (level_) {
        case eLayer::L2:
            if (reco) {
                for (auto l2_pro : proports_) {
                    owner_->Bitmaper(eLayer::ETHERNET)->Set(l2_pro, reco);
                    owner_->Bitmaper(eLayer::VLAN)->Set(l2_pro, reco);
                }
            } else {
                auto recognized = create_recognized_recognizer(number);
                for (auto l2_pro : proports_) {
                    owner_->Bitmaper(eLayer::ETHERNET)->Set(l2_pro, recognized);
                    owner_->Bitmaper(eLayer::VLAN)->Set(l2_pro, recognized);
                }
            }
            break;
        case eLayer::IPv4:
        case eLayer::IPv6:
            if (reco) {
                for (auto l3_pro : proports_) {
                    owner_->Bitmaper(level_)->Set(l3_pro, reco);
                }
            } else {
                auto recognized = create_recognized_recognizer(number);
                for (auto l3_pro : proports_) {
                    owner_->Bitmaper(level_)->Set(l3_pro, recognized);
                }
            }
            break;
        case eLayer::L3:
            if (reco) {
                for (auto l3_pro : proports_) {
                    owner_->Bitmaper(eLayer::IPv4)->Set(l3_pro, reco);
                    owner_->Bitmaper(eLayer::IPv6)->Set(l3_pro, reco);
                }
            } else {
                auto recognized = create_recognized_recognizer(number);
                for (auto l3_pro : proports_) {
                    owner_->Bitmaper(eLayer::IPv4)->Set(l3_pro, recognized);
                    owner_->Bitmaper(eLayer::IPv6)->Set(l3_pro, recognized);
                }
            }
            break;
        case eLayer::TCP:
        case eLayer::UDP:
        case eLayer::SCTP:
            if (reco) {
                if (proports_.empty()) {
                    owner_->Bitmaper(level_)->SetEmpty(reco);
                } else {
                    for (auto port : proports_) {
                        owner_->Bitmaper(level_)->Set(port, reco);
                    }
                }
            } else {
                auto recognized = create_recognized_recognizer(number);
                for (auto port : proports_) {
                    owner_->Bitmaper(level_)->Set(port, recognized);
                }
            }
            break;
        case eLayer::L4:
            if (reco) {
                if (proports_.empty()) {
                    owner_->Bitmaper(eLayer::TCP)->SetEmpty(reco);
                    owner_->Bitmaper(eLayer::UDP)->SetEmpty(reco);
                    owner_->Bitmaper(eLayer::SCTP)->SetEmpty(reco);
                } else {
                    for (auto port : proports_) {
                        owner_->Bitmaper(eLayer::TCP)->Set(port, reco);
                        owner_->Bitmaper(eLayer::UDP)->Set(port, reco);
                        owner_->Bitmaper(eLayer::SCTP)->Set(port, reco);
                    }
                }
            } else {
                auto recognized_recognizer = create_recognized_recognizer(number);
                for (auto port : proports_) {
                    owner_->Bitmaper(eLayer::TCP)->Set(port, recognized_recognizer);
                    owner_->Bitmaper(eLayer::UDP)->Set(port, recognized_recognizer);
                    owner_->Bitmaper(eLayer::SCTP)->Set(port, recognized_recognizer);
                }
            }
            break;
        default:
            break;
    }

    return 0;
}

Engine::Engine() : builder_(this) {
    enum_recognizer_pool_ = EnumerateRecognizerPool::instance();
}

Engine::~Engine() {
    for (auto& bitrecognizer : proport_recognizers_) {
        delete bitrecognizer;
        bitrecognizer = nullptr;
    }
    delete regex_recognizer_;
    regex_recognizer_ = nullptr;
}

int32_t Engine::Create(Config* configure) {
    Model model(this);
    return configure->Modeling(model);
}

Engine::Builder* Engine::Build() { return &builder_; }

int32_t Engine::Ready() {
    for (auto& reco : proport_recognizers_) {
        if (reco) reco->SetEmpty(builder_.get_prototype_unknown());
    }

    if (regex_recognizer_) {
        return regex_recognizer_->Ready(concurrency_);
    }

    return 0;
}

void Engine::Concurrency(int32_t number) { concurrency_ = number; }

int32_t Engine::Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size, const protocol::Layers* layers) {
    RecognizeContext rctx;
    eLayer toplevel = layers->Top();
    rctx.level = toplevel;
    switch (toplevel) {
        case eLayer::ETHERNET: {
            rctx.layer = eLayer::L2;
            const EthernetHeader* etherhdr = layers->Top<EthernetHeader>(packet, packet_size);
            rctx.proto = n2h16(etherhdr->ether_type);
            break;
        }
        case eLayer::VLAN: {
            rctx.layer = eLayer::L2;
            const VlanHeader* vlanhdr = layers->Top<VlanHeader>(packet, packet_size);
            rctx.proto = n2h16(vlanhdr->ether_type);
            break;
        }
        case eLayer::IPv4: {
            rctx.layer = eLayer::L3;
            const Ipv4Header* ipv4_header = layers->Top<Ipv4Header>(packet, packet_size);
            rctx.proto = ipv4_header->protocol;
            break;
        }
        case eLayer::TCP: {
            rctx.layer = eLayer::L4;
            const TcpHeader* tcp_header = layers->Top<TcpHeader>(packet, packet_size);
            rctx.dst_port = n2h16(tcp_header->dst_port);
            rctx.src_port = n2h16(tcp_header->src_port);
            break;
        }
        case eLayer::UDP: {
            rctx.layer = eLayer::L4;
            const UdpHeader* udp_header = layers->Top<UdpHeader>(packet, packet_size);
            rctx.dst_port = n2h16(udp_header->dst_port);
            rctx.src_port = n2h16(udp_header->src_port);
            break;
        }
        case eLayer::IPv6: {
            rctx.layer = eLayer::L3;
            const Ipv6Header* ipv6_header = layers->Top<Ipv6Header>(packet, packet_size);
            rctx.proto = ipv6_header->protocol;
            break;
        }
        case eLayer::IPv6_EXT_HOPOPTS:
        case eLayer::IPv6_EXT_ROUTING:
        case eLayer::IPv6_EXT_FRAGMENT:
        case eLayer::IPv6_EXT_ESP:
        case eLayer::IPv6_EXT_AH:
        case eLayer::IPv6_EXT_DSTOPTS: {
            rctx.layer = eLayer::L3;
            const Ipv6ExtentHeader* ipv6ext_header = layers->Top<Ipv6ExtentHeader>(packet, packet_size);
            rctx.level = eLayer::IPv6;
            rctx.proto = ipv6ext_header->protocol;
            break;
        }
        case eLayer::SCTP: {
            rctx.layer = eLayer::L4;
            const SctpHeader* sctp_header = layers->Top<SctpHeader>(packet, packet_size);
            rctx.dst_port = n2h16(sctp_header->dst_port);
            rctx.src_port = n2h16(sctp_header->src_port);
            break;
        }
        default:
            if (regex_recognizer_) regex_recognizer_->Identify(pipeno, packet, packet_size, layers, &rctx);
            return eLayer::NONE;
    }

    auto reco = proport_recognizers_[rctx.level];
    if (reco) return reco->Identify(pipeno, packet, packet_size, layers, &rctx);
    return eLayer::NONE;
}

BitmapRecognizer* Engine::Bitmaper(eLayer level) {
    auto& recognizer = proport_recognizers_[level];
    if (!recognizer) {
        switch (level) {
            case eLayer::TCP:
            case eLayer::UDP:
            case eLayer::SCTP:
                recognizer = new BitmapDualRecognizer;
                break;

            default:
                recognizer = new BitmapRecognizer;
                break;
        }
    }

    return recognizer;
}

RegexRecognizer* Engine::Regexer() {
    if (!regex_recognizer_) {
        regex_recognizer_ = new RegexRecognizer;
    }

    return regex_recognizer_;
}

EnumerateRecognizerPool* Engine::Enumerater() { return enum_recognizer_pool_; }

int32_t Engine::TraverseRecognizer(eLayer level, std::vector<int32_t> proports,
                                   std::function<int32_t(IRecognizer* reco)> traversal) {
    int32_t hitting = 0;
    switch (level) {
        case eLayer::IPv4:
        case eLayer::IPv6:
        case eLayer::TCP:
        case eLayer::UDP:
            for (auto proport : proports) {
                hitting += traversal(this->Bitmaper(level)->Get(proport));
            }
            break;
        case eLayer::L2:
            for (auto l2_pro : proports) {
                hitting += traversal(this->Bitmaper(eLayer::ETHERNET)->Get(l2_pro));
                hitting += traversal(this->Bitmaper(eLayer::VLAN)->Get(l2_pro));
            }
            break;
        case eLayer::L3:
            for (auto l3_pro : proports) {
                hitting += traversal(this->Bitmaper(eLayer::IPv4)->Get(l3_pro));
                hitting += traversal(this->Bitmaper(eLayer::IPv6)->Get(l3_pro));
            }
            break;
        case eLayer::L4:
            for (auto port : proports) {
                hitting += traversal(this->Bitmaper(eLayer::TCP)->Get(port));
                hitting += traversal(this->Bitmaper(eLayer::UDP)->Get(port));
                hitting += traversal(this->Bitmaper(eLayer::SCTP)->Get(port));
            }
            break;
        default:
            break;
    }

    return hitting;
}

}  // namespace protocol
}  // namespace flowsql
