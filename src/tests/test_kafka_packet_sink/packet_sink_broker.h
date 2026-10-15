// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <channels/kafka/kafka_store.h>
#include <framework/interfaces/ikafka_producer_session.h>
#include <librdkafka/rdkafka.h>
#include <chrono>
#include <memory>
#include <stdexcept>
namespace packet_sink_real {
using namespace flowsql;
using namespace flowsql::channels::kafka;
using Clock = std::chrono::steady_clock;
inline void Check(bool ok, const char* code) {
    if (!ok) throw std::runtime_error(code);
}
inline void Set(rd_kafka_conf_t* c, const char* key, const std::string& value) {
    char error[512];
    Check(rd_kafka_conf_set(c, key, value.c_str(), error, sizeof(error)) == RD_KAFKA_CONF_OK, "fixture_config");
}
class Broker {
    rd_kafka_t* client_ = nullptr;
    std::string servers_;
    std::vector<std::string> topics_;
    std::vector<std::string> acls_;

 public:
    explicit Broker(const rapidjson::Value& plain) {
        servers_ = plain["connection"]["bootstrap_servers"][0].GetString();
        auto* c = rd_kafka_conf_new();
        Set(c, "bootstrap.servers", servers_);
        Set(c, "log_level", "0");
        Set(c, "allow.auto.create.topics", "false");
        char error[512];
        client_ = rd_kafka_new(RD_KAFKA_PRODUCER, c, error, sizeof(error));
        Check(client_, "fixture_admin");
    }
    void ForgetTopicsForBrokerShutdown() { topics_.clear(); }
    ~Broker() {
        for (const auto& topic : acls_) {
            char error[512];
            auto* filter = rd_kafka_AclBindingFilter_new(
                RD_KAFKA_RESOURCE_TOPIC, topic.c_str(), RD_KAFKA_RESOURCE_PATTERN_LITERAL, nullptr, nullptr,
                RD_KAFKA_ACL_OPERATION_ANY, RD_KAFKA_ACL_PERMISSION_TYPE_ANY, error, sizeof(error));
            if (filter) {
                auto* queue = rd_kafka_queue_new(client_);
                rd_kafka_DeleteAcls(client_, &filter, 1, nullptr, queue);
                auto* event = rd_kafka_queue_poll(queue, 5000);
                if (event) rd_kafka_event_destroy(event);
                rd_kafka_queue_destroy(queue);
                rd_kafka_AclBinding_destroy(filter);
            }
        }
        for (const auto& topic : topics_) {
            auto* item = rd_kafka_DeleteTopic_new(topic.c_str());
            auto* q = rd_kafka_queue_new(client_);
            rd_kafka_DeleteTopics(client_, &item, 1, nullptr, q);
            auto* e = rd_kafka_queue_poll(q, 5000);
            if (e) rd_kafka_event_destroy(e);
            rd_kafka_queue_destroy(q);
            rd_kafka_DeleteTopic_destroy(item);
        }
        rd_kafka_destroy(client_);
    }
    void Topic(const std::string& name, bool small = false) {
        char error[512];
        auto* item = rd_kafka_NewTopic_new(name.c_str(), 1, 1, error, sizeof(error));
        Check(item, "fixture_topic");
        Check(!rd_kafka_NewTopic_set_config(item, "cleanup.policy", "delete"), "fixture_retention");
        if (small) Check(rd_kafka_NewTopic_set_config(item, "max.message.bytes", "1024") == 0, "fixture_topic_limit");
        auto* q = rd_kafka_queue_new(client_);
        rd_kafka_CreateTopics(client_, &item, 1, nullptr, q);
        auto* e = rd_kafka_queue_poll(q, 10000);
        bool ok = e && !rd_kafka_event_error(e);
        if (ok) {
            size_t count = 0;
            auto** r = rd_kafka_CreateTopics_result_topics(rd_kafka_event_CreateTopics_result(e), &count);
            ok = count == 1 && !rd_kafka_topic_result_error(r[0]);
        }
        if (e) rd_kafka_event_destroy(e);
        rd_kafka_queue_destroy(q);
        rd_kafka_NewTopic_destroy(item);
        Check(ok, "fixture_create_topic");
        topics_.push_back(name);
    }
    void DenyWrite(const std::string& topic) {
        char error[512];
        std::vector<rd_kafka_AclBinding_t*> acl;
        acl.push_back(rd_kafka_AclBinding_new(RD_KAFKA_RESOURCE_TOPIC, topic.c_str(), RD_KAFKA_RESOURCE_PATTERN_LITERAL,
                                              "User:*", "*", RD_KAFKA_ACL_OPERATION_DESCRIBE,
                                              RD_KAFKA_ACL_PERMISSION_TYPE_ALLOW, error, sizeof(error)));
        acl.push_back(rd_kafka_AclBinding_new(RD_KAFKA_RESOURCE_TOPIC, topic.c_str(), RD_KAFKA_RESOURCE_PATTERN_LITERAL,
                                              "User:tester", "*", RD_KAFKA_ACL_OPERATION_WRITE,
                                              RD_KAFKA_ACL_PERMISSION_TYPE_DENY, error, sizeof(error)));
        acl.push_back(rd_kafka_AclBinding_new(RD_KAFKA_RESOURCE_TOPIC, topic.c_str(), RD_KAFKA_RESOURCE_PATTERN_LITERAL,
                                              "User:tester", "*", RD_KAFKA_ACL_OPERATION_DESCRIBE_CONFIGS,
                                              RD_KAFKA_ACL_PERMISSION_TYPE_ALLOW, error, sizeof(error)));
        for (auto* a : acl) Check(a, "fixture_acl_binding");
        auto* q = rd_kafka_queue_new(client_);
        rd_kafka_CreateAcls(client_, acl.data(), acl.size(), nullptr, q);
        auto* e = rd_kafka_queue_poll(q, 10000);
        bool ok = e && !rd_kafka_event_error(e);
        if (ok) {
            size_t count = 0;
            auto** r = rd_kafka_CreateAcls_result_acls(rd_kafka_event_CreateAcls_result(e), &count);
            ok = count == acl.size();
            for (size_t i = 0; i < count; ++i) ok = ok && !rd_kafka_acl_result_error(r[i]);
        }
        if (e) rd_kafka_event_destroy(e);
        rd_kafka_queue_destroy(q);
        for (auto* a : acl) rd_kafka_AclBinding_destroy(a);
        Check(ok, "fixture_create_acl");
        acls_.push_back(topic);
    }
    bool Exists(const std::string& topic) {
        const rd_kafka_metadata_t* m = nullptr;
        Check(rd_kafka_metadata(client_, 1, nullptr, &m, 5000) == 0, "fixture_metadata");
        bool found = false;
        for (int i = 0; i < m->topic_cnt; ++i) found |= topic == m->topics[i].topic;
        rd_kafka_metadata_destroy(m);
        return found;
    }
    std::vector<KafkaProduceMessageV1> Consume(const std::string& topic, const std::string& group) {
        auto* c = rd_kafka_conf_new();
        Set(c, "bootstrap.servers", servers_);
        Set(c, "group.id", group);
        Set(c, "log_level", "0");
        Set(c, "enable.auto.commit", "false");
        Set(c, "enable.auto.offset.store", "false");
        Set(c, "allow.auto.create.topics", "false");
        Set(c, "auto.offset.reset", "earliest");
        char error[512];
        auto* consumer = rd_kafka_new(RD_KAFKA_CONSUMER, c, error, sizeof(error));
        Check(consumer, "fixture_consumer");
        std::unique_ptr<rd_kafka_t, decltype(&rd_kafka_destroy)> owner(consumer, rd_kafka_destroy);
        rd_kafka_poll_set_consumer(consumer);
        int64_t low = 0, high = 0;
        Check(rd_kafka_query_watermark_offsets(consumer, topic.c_str(), 0, &low, &high, 5000) == 0,
              "fixture_watermark");
        Check(low == 0 && high >= 0, "fixture_offsets");
        auto* assignment = rd_kafka_topic_partition_list_new(1);
        rd_kafka_topic_partition_list_add(assignment, topic.c_str(), 0)->offset = 0;
        Check(rd_kafka_assign(consumer, assignment) == 0, "fixture_assign");
        rd_kafka_topic_partition_list_destroy(assignment);
        std::vector<KafkaProduceMessageV1> out;
        auto deadline = Clock::now() + std::chrono::seconds(10);
        while (out.size() < uint64_t(high) && Clock::now() < deadline) {
            auto* message = rd_kafka_consumer_poll(consumer, 100);
            if (!message) continue;
            if (message->err) {
                auto error = message->err;
                rd_kafka_message_destroy(message);
                if (error == RD_KAFKA_RESP_ERR__PARTITION_EOF) continue;
                throw std::runtime_error("fixture_consume");
            }
            Check(message->payload && message->key && message->partition == 0 && message->offset == int64_t(out.size()),
                  "fixture_partition_offset");
            out.push_back({std::string(static_cast<const char*>(message->key), message->key_len),
                           std::string(static_cast<const char*>(message->payload), message->len)});
            rd_kafka_message_destroy(message);
        }
        Check(out.size() == uint64_t(high), "fixture_count");
        rd_kafka_assign(consumer, nullptr);
        rd_kafka_consumer_close(consumer);
        return out;
    }
};
}  // namespace packet_sink_real
