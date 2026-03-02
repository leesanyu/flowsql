#include "service_client.h"

#include <httplib.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace flowsql {
namespace gateway {

ServiceClient::~ServiceClient() {
    StopHeartbeat();
}

void ServiceClient::SetGateway(const std::string& host, int port) {
    gateway_host_ = host;
    gateway_port_ = port;
}

int ServiceClient::RegisterRoute(const std::string& prefix, const std::string& local_address) {
    httplib::Client client(gateway_host_, gateway_port_);
    client.set_connection_timeout(3);
    client.set_read_timeout(3);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("prefix"); w.String(prefix.c_str());
    w.Key("address"); w.String(local_address.c_str());
    w.Key("service"); w.String(service_name_.c_str());
    w.EndObject();

    auto res = client.Post("/gateway/register", buf.GetString(), "application/json");
    if (!res || res->status != 200) {
        printf("ServiceClient: failed to register route %s\n", prefix.c_str());
        return -1;
    }
    return 0;
}

int ServiceClient::RegisterRoutes(const std::vector<std::string>& prefixes, const std::string& local_address) {
    for (const auto& prefix : prefixes) {
        if (RegisterRoute(prefix, local_address) != 0) return -1;
    }
    return 0;
}

int ServiceClient::UnregisterRoute(const std::string& prefix) {
    httplib::Client client(gateway_host_, gateway_port_);
    client.set_connection_timeout(3);
    client.set_read_timeout(3);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("prefix"); w.String(prefix.c_str());
    w.EndObject();

    auto res = client.Post("/gateway/unregister", buf.GetString(), "application/json");
    if (!res || res->status != 200) return -1;
    return 0;
}

void ServiceClient::StartHeartbeat(const std::string& service_name, int interval_s) {
    service_name_ = service_name;
    heartbeat_running_ = true;
    heartbeat_thread_ = std::thread([this, interval_s]() {
        while (heartbeat_running_) {
            httplib::Client client(gateway_host_, gateway_port_);
            client.set_connection_timeout(2);
            client.set_read_timeout(2);

            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> w(buf);
            w.StartObject();
            w.Key("service"); w.String(service_name_.c_str());
            w.EndObject();

            client.Post("/gateway/heartbeat", buf.GetString(), "application/json");

            // 分段 sleep，便于快速退出
            for (int i = 0; i < interval_s * 10 && heartbeat_running_; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });
}

void ServiceClient::StopHeartbeat() {
    heartbeat_running_ = false;
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
}

std::string ServiceClient::QueryRoutes() {
    httplib::Client client(gateway_host_, gateway_port_);
    client.set_connection_timeout(3);
    client.set_read_timeout(3);

    auto res = client.Get("/gateway/routes");
    if (res && res->status == 200) return res->body;
    return "{}";
}

}  // namespace gateway
}  // namespace flowsql
