// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr const char* kCaptureMount = "pcap-uploads:/opt/flowsql/uploads";
constexpr const char* kFlowsqlConfigMount = "./config/flowsql.yml:/opt/flowsql/config/flowsql.yml:ro";
constexpr const char* kGatewayPlugin = "libflowsql_gateway.so";
constexpr const char* kWebPlugin = "libflowsql_web.so";
constexpr const char* kRouterPlugin = "libflowsql_router.so";
constexpr const char* kNpiPlugin =
    "libflowsql_npi.so:{\"ldfile\":\"/opt/flowsql/config/protocols.yml\"}";
constexpr const char* kPcapFilePlugin = "libflowsql_pcapfile.so";
constexpr const char* kPcapFileDbPath = "/opt/flowsql/uploads/.meta/pcapfile.db";

bool Expect(bool condition, const std::string& message) {
    if (condition) return true;
    std::cerr << "FAILED: " << message << '\n';
    return false;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool Contains(const std::string& text, const std::string& expected, const std::string& message) {
    return Expect(text.find(expected) != std::string::npos, message);
}

bool HasVolume(const YAML::Node& service, const std::string& expected) {
    const YAML::Node volumes = service["volumes"];
    if (!volumes || !volumes.IsSequence()) return false;
    for (const auto& volume : volumes) {
        if (volume.IsScalar() && volume.as<std::string>() == expected) return true;
    }
    return false;
}

std::vector<std::string> Split(const std::string& text, char delimiter) {
    std::vector<std::string> values;
    std::stringstream stream(text);
    std::string value;
    while (std::getline(stream, value, delimiter)) values.push_back(value);
    return values;
}

std::string CommandText(const YAML::Node& command) {
    if (command.IsScalar()) return command.as<std::string>();
    if (!command.IsSequence()) return {};

    std::string text;
    for (const auto& argument : command) {
        if (!text.empty()) text += ' ';
        text += argument.as<std::string>();
    }
    return text;
}

std::string CommandArgument(const YAML::Node& command, const std::string& option) {
    if (command.IsSequence()) {
        for (size_t i = 0; i + 1 < command.size(); ++i) {
            if (command[i].as<std::string>() == option) return command[i + 1].as<std::string>();
        }
        return {};
    }

    const std::string text = CommandText(command);
    const std::string prefix = option + " ";
    const size_t begin = text.find(prefix);
    if (begin == std::string::npos) return {};
    const size_t value_begin = begin + prefix.size();
    const size_t end = text.find(" --", value_begin);
    return text.substr(value_begin, end == std::string::npos ? end : end - value_begin);
}

std::string PluginSpec(const std::string& plugins, const std::string& library) {
    for (const auto& plugin : Split(plugins, ',')) {
        if (plugin == library || plugin.rfind(library + ":", 0) == 0) return plugin;
    }
    return {};
}

std::string PluginOption(const std::string& plugins, const std::string& library) {
    const std::string plugin = PluginSpec(plugins, library);
    const size_t delimiter = plugin.find(':');
    return delimiter == std::string::npos ? std::string() : plugin.substr(delimiter + 1);
}

bool ExpectDeclaredPluginsExist(const YAML::Node& services) {
    bool ok = true;
    for (const auto& entry : services) {
        const std::string service_name = entry.first.as<std::string>();
        const YAML::Node command = entry.second["command"];
        if (!command) continue;
        for (const auto& plugin : Split(CommandArgument(command, "--plugins"), ',')) {
            if (plugin.empty()) continue;
            const std::string library = plugin.substr(0, plugin.find(':'));
            const std::filesystem::path output = std::filesystem::path(FLOWSQL_BUILD_OUTPUT_PATH) / library;
            ok = Expect(std::filesystem::is_regular_file(output),
                        service_name + " declares a plugin absent from build/output: " + library) &&
                 ok;
        }
    }
    return ok;
}

bool TestDockerfile() {
    const std::string dockerfile = ReadFile(FLOWSQL_DOCKERFILE_PATH);
    bool ok = Expect(!dockerfile.empty(), "Dockerfile must be readable");
    ok = Contains(dockerfile, "COPY src/plugins/npi/conf/protocols.yml ./config/protocols.yml",
                  "Dockerfile must install the NPI protocol definition at the runtime config path") &&
         ok;
    ok = Contains(dockerfile, "COPY build/output/lib*.so", "Dockerfile must copy built plugin libraries") && ok;
    return ok;
}

bool TestCompose() {
    YAML::Node root;
    try {
        root = YAML::LoadFile(FLOWSQL_DOCKER_COMPOSE_PATH);
    } catch (const YAML::Exception& error) {
        std::cerr << "FAILED: docker-compose.yml must parse: " << error.what() << '\n';
        return false;
    }

    const YAML::Node services = root["services"];
    const YAML::Node gateway = services["gateway"];
    const YAML::Node web = services["web"];
    const YAML::Node scheduler = services["scheduler"];
    bool ok = Expect(!root["version"].IsDefined(), "Compose must omit the obsolete version field");
    ok = Expect(gateway.IsMap(), "Compose must define the Gateway service") && ok;
    ok = Expect(web.IsMap(), "Compose must define the Web service") && ok;
    ok = Expect(scheduler.IsMap(), "Compose must define the Scheduler service") && ok;
    ok = Expect(root["volumes"] && root["volumes"]["pcap-uploads"].IsDefined(),
                "Compose must declare the pcap-uploads named volume") &&
         ok;
    if (!gateway.IsMap() || !web.IsMap() || !scheduler.IsMap()) return false;

    const YAML::Node gateway_command = gateway["command"];
    const YAML::Node web_command = web["command"];
    const YAML::Node scheduler_command = scheduler["command"];
    const std::string gateway_plugins = CommandArgument(gateway_command, "--plugins");
    const std::string gateway_option = CommandArgument(gateway_command, "--option");
    const std::string web_plugins = CommandArgument(web_command, "--plugins");
    const std::string web_option = PluginOption(web_plugins, kWebPlugin);
    const std::string router_option = PluginOption(web_plugins, kRouterPlugin);
    const std::string scheduler_plugins = CommandArgument(scheduler_command, "--plugins");
    const std::string pcapfile_option = PluginOption(scheduler_plugins, kPcapFilePlugin);

    ok = Expect(gateway_command.IsSequence(), "Gateway command must use an argv sequence") && ok;
    ok = Expect(gateway_plugins == kGatewayPlugin, "Gateway plugin must not receive a config path as its option") &&
         ok;
    ok = Expect(CommandArgument(gateway_command, "--port") == "18800", "Gateway must listen on port 18800") && ok;
    ok = Contains(gateway_option, "host=0.0.0.0", "Gateway must be reachable from the Compose network") && ok;
    ok = Contains(gateway_option, "heartbeat_interval_s=10", "Gateway must preserve its heartbeat interval") && ok;
    ok = Contains(gateway_option, "heartbeat_timeout_count=3", "Gateway must preserve its heartbeat timeout") && ok;

    ok = Expect(web_command.IsSequence(), "Web command must preserve per-plugin options with an argv sequence") && ok;
    ok = Expect(CommandArgument(web_command, "--port") == "18802", "Web service Router must expose port 18802") && ok;
    ok = Expect(!PluginSpec(web_plugins, kWebPlugin).empty(), "Web service must load the Web plugin") && ok;
    ok = Expect(!PluginSpec(web_plugins, kRouterPlugin).empty(), "Web service must load the Router plugin") && ok;
    ok = Contains(web_option, "host=0.0.0.0", "Web must listen on the container network") && ok;
    ok = Contains(web_option, "port=8081", "Web must listen on its external port") && ok;
    ok = Contains(web_option, "gateway=gateway:18800", "Web control requests must use Gateway") && ok;
    ok = Contains(web_option, "upload_dir=/opt/flowsql/uploads", "Web must use the shared absolute upload path") &&
         ok;
    ok = Contains(router_option, "host=0.0.0.0", "Web Router must listen on the container network") && ok;
    ok = Contains(router_option, "port=18802", "Web Router must listen on its internal port") && ok;
    ok = Contains(router_option, "gateway=gateway:18800", "Web Router must register with Gateway") && ok;
    ok = Expect(CommandArgument(web_command, "--option").empty(),
                "Web and Router must not share one ambiguous default option") &&
         ok;
    ok = Expect(CommandText(web_command).find("router_host") == std::string::npos &&
                    CommandText(web_command).find("router_port") == std::string::npos,
                "Compose must not use unsupported Router option aliases") &&
         ok;

    ok = Expect(scheduler_command.IsSequence(), "Scheduler command must preserve JSON with an argv sequence") && ok;
    ok = Expect(HasVolume(web, kCaptureMount), "Web must mount pcap-uploads at the shared absolute path") && ok;
    ok = Expect(HasVolume(scheduler, kCaptureMount),
                "Scheduler must mount pcap-uploads at the shared absolute path") &&
         ok;
    ok = Contains(scheduler_plugins, kNpiPlugin, "Scheduler must load NPI with the absolute protocol path") && ok;
    ok = Expect(!PluginSpec(scheduler_plugins, kPcapFilePlugin).empty(),
                "Scheduler must load the pcapfile provider") &&
         ok;
    ok = Expect(pcapfile_option == std::string("db_path=") + kPcapFileDbPath,
                "Docker pcapfile metadata must persist inside the capture named volume") &&
         ok;
    ok = Expect(pcapfile_option.rfind("db_path=/opt/flowsql/uploads/", 0) == 0,
                "Docker pcapfile database must be covered by the Scheduler capture mount") &&
         ok;
    ok = Expect(scheduler_plugins.find("libflowsql_example.so") == std::string::npos,
                "Scheduler must not reference the absent example plugin") &&
         ok;
    ok = Expect(HasVolume(scheduler, kFlowsqlConfigMount), "Scheduler config file mount must be read-only") && ok;
    ok = Expect(!HasVolume(scheduler, "./config:/opt/flowsql/config"),
                "Scheduler must not hide image-owned protocols.yml with a config directory bind mount") &&
         ok;
    return ExpectDeclaredPluginsExist(services) && ok;
}

}  // namespace

int main() {
    bool ok = TestDockerfile();
    ok = TestCompose() && ok;
    if (!ok) return 1;
    std::cout << "Docker deployment contract tests passed\n";
    return 0;
}
