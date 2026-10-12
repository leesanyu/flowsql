// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <common/network/analysis_address.h>
#include <framework/interfaces/ipacket.h>

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>

namespace flowsql::npm {

// Family is expressed by the key type. All fields participate in session identity.
// a/b are ordered by network address octets, then host-order port; neither port is nested in an address wrapper.
struct NpmSessionKey4 {
    network::IPv4Address a_ip;
    network::IPv4Address b_ip;
    uint64_t observation_domain_id = 0;
    std::string input_namespace;
    uint16_t a_port = 0;
    uint16_t b_port = 0;
    uint8_t transport_protocol = 0;
};

struct NpmSessionKey6 {
    network::IPv6Address a_ip;
    network::IPv6Address b_ip;
    std::string input_namespace;
    uint64_t observation_domain_id = 0;
    uint16_t a_port = 0;
    uint16_t b_port = 0;
    uint8_t transport_protocol = 0;
};

// Borrows exactly one typed key for the current callback. The owner must keep that key alive and stable.
// Ended snapshots and asynchronous work own a Key4 or Key6 copy, never this view or a maximum-size key variant.
class NpmSessionKeyView {
 public:
    explicit NpmSessionKeyView(const NpmSessionKey4& key) : key_(&key) {}
    explicit NpmSessionKeyView(const NpmSessionKey6& key) : key_(&key) {}
    NpmSessionKeyView(const NpmSessionKey4&&) = delete;
    NpmSessionKeyView(const NpmSessionKey6&&) = delete;

    packet::AddressFamily Family() const {
        return key_.index() == 0 ? packet::AddressFamily::kIPv4 : packet::AddressFamily::kIPv6;
    }

    const NpmSessionKey4* Get4() const {
        const auto* key = std::get_if<const NpmSessionKey4*>(&key_);
        return key ? *key : nullptr;
    }

    const NpmSessionKey6* Get6() const {
        const auto* key = std::get_if<const NpmSessionKey6*>(&key_);
        return key ? *key : nullptr;
    }

    const std::string& Namespace() const { return Get4() ? Get4()->input_namespace : Get6()->input_namespace; }
    uint64_t Domain() const { return Get4() ? Get4()->observation_domain_id : Get6()->observation_domain_id; }
    uint8_t Protocol() const { return Get4() ? Get4()->transport_protocol : Get6()->transport_protocol; }
    uint16_t APort() const { return Get4() ? Get4()->a_port : Get6()->a_port; }
    uint16_t BPort() const { return Get4() ? Get4()->b_port : Get6()->b_port; }
    const uint8_t* ABytes() const { return Get4() ? Get4()->a_ip.bytes : Get6()->a_ip.bytes; }
    const uint8_t* BBytes() const { return Get4() ? Get4()->b_ip.bytes : Get6()->b_ip.bytes; }
    size_t OwnedKeyBytes() const {
        return (Get4() ? sizeof(NpmSessionKey4) : sizeof(NpmSessionKey6)) + Namespace().capacity() + 1;
    }

 private:
    std::variant<const NpmSessionKey4*, const NpmSessionKey6*> key_;
};

// Owns only a concrete family allocation. Copying deep-copies that key; borrowed views never outlive the owner.
// The dispatch object has no inline address storage and does not charge IPv4 for a Key6 allocation.
class NpmOwnedSessionKey {
 public:
    NpmOwnedSessionKey() = default;
    NpmOwnedSessionKey(const NpmSessionKey4& key) : key_(std::make_unique<NpmSessionKey4>(key)) {}
    NpmOwnedSessionKey(const NpmSessionKey6& key) : key_(std::make_unique<NpmSessionKey6>(key)) {}
    NpmOwnedSessionKey(NpmSessionKeyView key) { Assign(key); }
    NpmOwnedSessionKey(const NpmOwnedSessionKey& other) {
        if (other) Assign(other.View());
    }
    NpmOwnedSessionKey& operator=(const NpmOwnedSessionKey& other) {
        if (this != &other) {
            NpmOwnedSessionKey copy(other);
            key_.swap(copy.key_);
        }
        return *this;
    }
    NpmOwnedSessionKey(NpmOwnedSessionKey&&) noexcept = default;
    NpmOwnedSessionKey& operator=(NpmOwnedSessionKey&&) noexcept = default;
    explicit operator bool() const { return Get4() || Get6(); }
    NpmSessionKey4* Get4() {
        auto* p = std::get_if<std::unique_ptr<NpmSessionKey4>>(&key_);
        return p ? p->get() : nullptr;
    }
    NpmSessionKey6* Get6() {
        auto* p = std::get_if<std::unique_ptr<NpmSessionKey6>>(&key_);
        return p ? p->get() : nullptr;
    }
    const NpmSessionKey4* Get4() const {
        const auto* p = std::get_if<std::unique_ptr<NpmSessionKey4>>(&key_);
        return p ? p->get() : nullptr;
    }
    const NpmSessionKey6* Get6() const {
        const auto* p = std::get_if<std::unique_ptr<NpmSessionKey6>>(&key_);
        return p ? p->get() : nullptr;
    }
    NpmSessionKeyView View() const& {
        assert(*this);
        return Get4() ? NpmSessionKeyView(*Get4()) : NpmSessionKeyView(*Get6());
    }
    NpmSessionKeyView View() const&& = delete;
    operator NpmSessionKeyView() const& { return View(); }
    operator NpmSessionKeyView() const&& = delete;

 private:
    void Assign(NpmSessionKeyView key) {
        if (key.Get4())
            key_ = std::make_unique<NpmSessionKey4>(*key.Get4());
        else
            key_ = std::make_unique<NpmSessionKey6>(*key.Get6());
    }
    std::variant<std::unique_ptr<NpmSessionKey4>, std::unique_ptr<NpmSessionKey6>> key_;
};

}  // namespace flowsql::npm
