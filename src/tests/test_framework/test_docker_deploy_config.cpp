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
constexpr const char* kFlowsqlConfigMount = "flowsql-config:/opt/flowsql/config";
constexpr const char* kPythonIpcMount = "python-ipc:/tmp";
constexpr const char* kPythonOperatorsMount = "python-operators:/opt/flowsql/bin/operators";
constexpr const char* kGatewayPlugin = "libflowsql_gateway.so";
constexpr const char* kWebPlugin = "libflowsql_web.so";
constexpr const char* kRouterPlugin = "libflowsql_router.so";
constexpr const char* kNpiPlugin = "libflowsql_npi.so:{\"ldfile\":\"/opt/flowsql/config/protocols.yml\"}";
constexpr const char* kPcapFilePlugin = "libflowsql_pcapfile.so";
constexpr const char* kConfigChannelPlugin = "libflowsql_config_channel.so";
constexpr const char* kFlowLabelingPlugin = "libflowsql_flow_labeling.so";
constexpr const char* kFlowLabelingOption = "eal_memory_mib=512";
constexpr const char* kNpmBasicPlugin = "libflowsql_npm_basic.so";
constexpr const char* kTaskPlugin = "libflowsql_task.so";
constexpr const char* kSchedulerPlugin = "libflowsql_scheduler.so";
constexpr const char* kBridgePlugin = "libflowsql_bridge.so";
constexpr const char* kBuiltinPlugin = "libflowsql_builtin.so";
constexpr const char* kCatalogPlugin = "libflowsql_catalog.so";
constexpr const char* kBinAddonPlugin = "libflowsql_binaddon.so";
constexpr const char* kDatabasePlugin = "libflowsql_database.so";
constexpr const char* kStreamPlugin = "libflowsql_stream.so";
constexpr const char* kPcapFileDbPath = "/opt/flowsql/uploads/.meta/pcapfile.db";
constexpr const char* kConfigChannelDbPath = "/opt/flowsql/uploads/.meta/flowsql_meta.db";
constexpr const char* kOperatorDbPath = "/opt/flowsql/uploads/.meta/flowsql_meta.db";
constexpr const char* kDataframeDir = "/opt/flowsql/uploads/dataframes";
constexpr const char* kBinAddonUploadDir = "/opt/flowsql/uploads/binaddon";

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
#if !FLOWSQL_FLOW_LABELING_BUILT
            if (library == kFlowLabelingPlugin) continue;
#endif
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
    ok = Contains(dockerfile, "FROM ubuntu:24.04 AS runtime",
                  "Docker runtime distribution must provide the DPDK 23.11 ABI") &&
         ok;
    ok = Contains(dockerfile, "librte-acl24", "Docker runtime must install the system DPDK ACL library") && ok;
    ok = Contains(dockerfile, "librte-eal24", "Docker runtime must install the system DPDK EAL library") && ok;
    ok = Contains(dockerfile, "libpq5", "Docker runtime must install the PostgreSQL client library") && ok;
    ok = Contains(dockerfile, "python3 -m venv", "Docker Python dependencies must use a virtual environment") && ok;
    ok = Contains(dockerfile, "pyarrow==22.0.0", "Docker Arrow runtime must match the built C++ ABI") && ok;
    ok = Contains(dockerfile, "-r ./python/requirements.txt", "Docker Worker must install all declared dependencies") &&
         ok;
    ok = Contains(dockerfile, "libyaml-cpp.so.0.9.0 ./bin/libyaml-cpp.so.0.9",
                  "Docker must copy the built yaml-cpp ABI into the runtime library path") &&
         ok;
    ok = Contains(dockerfile, "COPY src/plugins/npi/conf/protocols.yml ./config/protocols.yml",
                  "Dockerfile must install the NPI protocol definition at the runtime config path") &&
         ok;
    ok = Contains(dockerfile, "COPY build/output/lib*.so", "Dockerfile must copy built plugin libraries") && ok;
    ok = Contains(dockerfile, "COPY config/docker/flowsql.yml     ./config/flowsql.yml",
                  "Docker image must seed a Docker-specific writable database configuration") &&
         ok;
    ok = Expect(std::filesystem::is_regular_file(std::filesystem::path(FLOWSQL_BUILD_OUTPUT_PATH) / kNpmBasicPlugin),
                "Docker image input must contain the uploadable npm.basic plugin") &&
         ok;
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
    const YAML::Node pyworker = services["pyworker"];
    bool ok = Expect(!root["version"].IsDefined(), "Compose must omit the obsolete version field");
    ok = Expect(gateway.IsMap(), "Compose must define the Gateway service") && ok;
    ok = Expect(web.IsMap(), "Compose must define the Web service") && ok;
    ok = Expect(scheduler.IsMap(), "Compose must define the Scheduler service") && ok;
    ok = Expect(pyworker.IsMap(), "Compose must define the Python Worker service") && ok;
    ok = Expect(root["volumes"] && root["volumes"]["pcap-uploads"].IsDefined(),
                "Compose must declare the pcap-uploads named volume") &&
         ok;
    ok = Expect(root["volumes"] && root["volumes"]["flowsql-config"].IsDefined(),
                "Compose must persist writable database configuration") &&
         ok;
    ok = Expect(root["volumes"] && root["volumes"]["python-ipc"].IsDefined() &&
                    root["volumes"]["python-operators"].IsDefined(),
                "Compose must share Python IPC files and uploaded operators") &&
         ok;
    if (!gateway.IsMap() || !web.IsMap() || !scheduler.IsMap() || !pyworker.IsMap()) return false;

    const YAML::Node gateway_command = gateway["command"];
    const YAML::Node web_command = web["command"];
    const YAML::Node scheduler_command = scheduler["command"];
    const std::string gateway_plugins = CommandArgument(gateway_command, "--plugins");
    const std::string gateway_option = CommandArgument(gateway_command, "--option");
    const std::string web_plugins = CommandArgument(web_command, "--plugins");
    const std::string web_option = PluginOption(web_plugins, kWebPlugin);
    const std::string router_option = PluginOption(web_plugins, kRouterPlugin);
    const std::string scheduler_plugins = CommandArgument(scheduler_command, "--plugins");
    const std::string task_option = PluginOption(scheduler_plugins, kTaskPlugin);
    const std::string scheduler_router_option = PluginOption(scheduler_plugins, kRouterPlugin);
    const std::string scheduler_option = PluginOption(scheduler_plugins, kSchedulerPlugin);
    const std::string bridge_option = PluginOption(scheduler_plugins, kBridgePlugin);
    const std::string flow_labeling_option = PluginOption(scheduler_plugins, kFlowLabelingPlugin);
    const std::string pcapfile_option = PluginOption(scheduler_plugins, kPcapFilePlugin);
    const std::string config_channel_option = PluginOption(scheduler_plugins, kConfigChannelPlugin);
    const std::string catalog_option = PluginOption(scheduler_plugins, kCatalogPlugin);
    const std::string binaddon_option = PluginOption(scheduler_plugins, kBinAddonPlugin);
    const std::string database_option = PluginOption(scheduler_plugins, kDatabasePlugin);
    const std::string stream_option = PluginOption(scheduler_plugins, kStreamPlugin);

    ok = Expect(PluginSpec(gateway_plugins, kFlowLabelingPlugin).empty(), "Gateway must not load Flow Labeling") && ok;
    ok = Expect(PluginSpec(web_plugins, kFlowLabelingPlugin).empty(), "Web must not load Flow Labeling") && ok;

    ok = Expect(gateway_command.IsSequence(), "Gateway command must use an argv sequence") && ok;
    ok = Expect(gateway_plugins == kGatewayPlugin, "Gateway plugin must not receive a config path as its option") && ok;
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
    ok = Contains(web_option, "upload_dir=/opt/flowsql/uploads", "Web must use the shared absolute upload path") && ok;
    ok = Contains(web_option, "db_path=/opt/flowsql/uploads/web.db",
                  "Web metadata must persist in an existing volume directory") &&
         ok;
    ok =
        Contains(web_option, "worker_host=pyworker;worker_port=18900", "Web must reach Python Worker for reload") && ok;
    ok = Contains(router_option, "host=web", "Web Router must register a resolvable container address") && ok;
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
    ok = Expect(CommandArgument(scheduler_command, "--option").empty() &&
                    CommandArgument(scheduler_command, "--port").empty(),
                "Scheduler plugins must not receive one ambiguous default option") &&
         ok;
    ok =
        Expect(!PluginSpec(scheduler_plugins, kTaskPlugin).empty(), "Scheduler must load persistent Task routes") && ok;
    ok = Expect(!PluginSpec(scheduler_plugins, kStreamPlugin).empty(), "Scheduler must load Stream channels") && ok;
    ok = Expect(!PluginSpec(scheduler_plugins, kDatabasePlugin).empty(), "Scheduler must load Database channels") && ok;
    ok = Contains(scheduler_router_option, "host=scheduler", "Scheduler Router must register a resolvable address") &&
         Contains(scheduler_router_option, "gateway=gateway:18800", "Scheduler Router must use Gateway") && ok;
    ok = Contains(scheduler_option, "port=18803", "Scheduler must listen on port 18803") && ok;
    ok = Expect(task_option == "db_path=/opt/flowsql/uploads/.meta/flowsql_meta.db",
                "Task metadata must persist in the shared volume") &&
         ok;
    ok = Expect(bridge_option == "worker_host=pyworker;worker_port=18900",
                "Bridge must use the Python Worker container address") &&
         ok;
    ok = Expect(database_option == "config_file=/opt/flowsql/config/flowsql.yml",
                "Database plugin must receive only its own writable config file option") &&
         ok;
    ok = Expect(stream_option ==
                    "config_file=/opt/flowsql/config/flowsql.yml;db_path=/opt/flowsql/uploads/.meta/flowsql_meta.db",
                "Stream plugin must use persisted configuration") &&
         ok;
    ok = Expect(HasVolume(web, kCaptureMount), "Web must mount pcap-uploads at the shared absolute path") && ok;
    ok = Expect(HasVolume(scheduler, kCaptureMount), "Scheduler must mount pcap-uploads at the shared absolute path") &&
         ok;
    ok = Contains(scheduler_plugins, kNpiPlugin, "Scheduler must load NPI with the absolute protocol path") && ok;
    ok = Expect(!PluginSpec(scheduler_plugins, kPcapFilePlugin).empty(), "Scheduler must load the pcapfile provider") &&
         ok;
    ok = Expect(!PluginSpec(scheduler_plugins, kConfigChannelPlugin).empty(),
                "Scheduler must load the Config Channel provider") &&
         ok;
    ok = Expect(!PluginSpec(scheduler_plugins, kFlowLabelingPlugin).empty(),
                "Scheduler must load the Flow Labeling provider") &&
         ok;
    ok = Expect(flow_labeling_option == kFlowLabelingOption,
                "Scheduler Flow Labeling must set the process EAL default explicitly") &&
         ok;
    ok = Expect(PluginSpec(scheduler_plugins, kNpmBasicPlugin).empty(),
                "Scheduler must not statically load the npm.basic operator plugin") &&
         ok;
    ok = Expect(!PluginSpec(scheduler_plugins, kBuiltinPlugin).empty(),
                "Scheduler must load Builtin for Catalog initialization") &&
         ok;
    ok = Expect(!PluginSpec(scheduler_plugins, kCatalogPlugin).empty(),
                "Scheduler must load Catalog for operator and dataframe registration") &&
         ok;
    ok = Expect(!PluginSpec(scheduler_plugins, kBinAddonPlugin).empty(),
                "Scheduler must load BinAddon for C++ operator plugin lifecycle") &&
         ok;
    const size_t npi_index = scheduler_plugins.find(kNpiPlugin);
    const size_t pcapfile_index = scheduler_plugins.find(kPcapFilePlugin);
    const size_t config_channel_index = scheduler_plugins.find(kConfigChannelPlugin);
    const size_t flow_labeling_index = scheduler_plugins.find(kFlowLabelingPlugin);
    const size_t scheduler_index = scheduler_plugins.find(kSchedulerPlugin);
    const size_t builtin_index = scheduler_plugins.find(kBuiltinPlugin);
    const size_t catalog_index = scheduler_plugins.find(kCatalogPlugin);
    const size_t binaddon_index = scheduler_plugins.find(kBinAddonPlugin);
    ok = Expect(npi_index < pcapfile_index && pcapfile_index < config_channel_index &&
                    config_channel_index < builtin_index && builtin_index < flow_labeling_index &&
                    flow_labeling_index < scheduler_index && scheduler_index < catalog_index &&
                    catalog_index < binaddon_index && binaddon_index != std::string::npos,
                "Docker must order NPI, Config Channel, Builtin, Flow Labeling, Scheduler, Catalog and BinAddon") &&
         ok;
    ok = Expect(pcapfile_option == std::string("db_path=") + kPcapFileDbPath,
                "Docker pcapfile metadata must persist inside the capture named volume") &&
         ok;
    ok = Expect(pcapfile_option.rfind("db_path=/opt/flowsql/uploads/", 0) == 0,
                "Docker pcapfile database must be covered by the Scheduler capture mount") &&
         ok;
    ok = Expect(config_channel_option == std::string("db_path=") + kConfigChannelDbPath,
                "Docker Config Channel metadata must use the persistent runtime meta database") &&
         ok;
    ok = Expect(config_channel_option.rfind("db_path=/opt/flowsql/uploads/", 0) == 0,
                "Docker Config Channel database must be covered by the Scheduler named volume") &&
         ok;
    ok = Expect(catalog_option == std::string("data_dir=") + kDataframeDir + ";operator_db_path=" + kOperatorDbPath,
                "Docker Catalog data and operator metadata must persist in the shared volume") &&
         ok;
    ok = Expect(binaddon_option ==
                    std::string("operator_db_path=") + kOperatorDbPath + ";upload_dir=" + kBinAddonUploadDir,
                "Docker BinAddon must share Catalog metadata and persist uploaded plugins") &&
         ok;
    ok = Expect(config_channel_option.find(kOperatorDbPath) != std::string::npos &&
                    catalog_option.find(kOperatorDbPath) != std::string::npos &&
                    binaddon_option.find(kOperatorDbPath) != std::string::npos,
                "Docker Config Channel, Catalog and BinAddon must use the persistent runtime meta database") &&
         ok;
    ok = Expect(scheduler_plugins.find("libflowsql_example.so") == std::string::npos,
                "Scheduler must not reference the absent example plugin") &&
         ok;
    ok = Expect(HasVolume(scheduler, kFlowsqlConfigMount), "Scheduler config directory must be writable") && ok;
    ok = Expect(HasVolume(scheduler, kPythonIpcMount) && HasVolume(pyworker, kPythonIpcMount),
                "Scheduler and Worker must share Python fallback IPC files") &&
         ok;
    ok = Expect(HasVolume(scheduler, kPythonOperatorsMount) && HasVolume(pyworker, kPythonOperatorsMount),
                "Scheduler and Worker must share uploaded Python operators") &&
         ok;
    ok = Expect(scheduler["ipc"].as<std::string>() == "service:pyworker" &&
                    pyworker["ipc"].as<std::string>() == "shareable",
                "Scheduler and Worker must share the same /dev/shm namespace") &&
         ok;
    ok = Expect(pyworker["environment"] &&
                    pyworker["environment"]["FLOWSQL_GATEWAY_ADDR"].as<std::string>() == "gateway:18800",
                "Python Worker must register with Gateway") &&
         ok;
    ok = Contains(CommandText(pyworker["command"]), "--host pyworker",
                  "Python Worker must listen and register a resolvable address") &&
         ok;
    ok = Expect(pyworker["healthcheck"].IsMap(), "Python Worker must expose a startup healthcheck") && ok;
    ok = Expect(scheduler["depends_on"] && scheduler["depends_on"]["pyworker"].IsMap(),
                "Scheduler must wait for Python Worker readiness") &&
         ok;
    ok = Expect(!HasVolume(scheduler, "./config:/opt/flowsql/config"),
                "Scheduler must not hide image-owned protocols.yml with a config directory bind mount") &&
         ok;
    return ExpectDeclaredPluginsExist(services) && ok;
}

bool TestFullComposePostgres() {
    YAML::Node root;
    try {
        root = YAML::LoadFile(std::filesystem::path(FLOWSQL_DOCKER_COMPOSE_PATH).parent_path() /
                              "docker-compose.databases.yml");
    } catch (const YAML::Exception& error) {
        std::cerr << "FAILED: docker-compose.databases.yml must parse: " << error.what() << '\n';
        return false;
    }

    const YAML::Node postgres = root["services"]["postgres"];
    bool ok = Expect(postgres.IsMap(), "Full Compose must define PostgreSQL");
    if (!postgres.IsMap()) return false;

    ok =
        Expect(postgres["image"].as<std::string>() == "postgres:16", "PostgreSQL must use the supported version") && ok;
    ok = Expect(postgres["environment"]["POSTGRES_DB"].as<std::string>() == "flowsql_db" &&
                    postgres["environment"]["POSTGRES_USER"].as<std::string>() == "flowsql_user",
                "PostgreSQL bootstrap database and user must match the standalone deployment") &&
         ok;
    ok = Expect(HasVolume(postgres, "postgres_data:/var/lib/postgresql/data"),
                "PostgreSQL data must persist in a named volume") &&
         ok;
    ok = Expect(root["volumes"]["postgres_data"].IsDefined(), "Full Compose must declare PostgreSQL data volume") && ok;
    ok = Expect(postgres["networks"].IsSequence() && postgres["networks"][0].as<std::string>() == "flowsql-net",
                "PostgreSQL must join the Scheduler network") &&
         ok;
    ok = Contains(CommandText(postgres["healthcheck"]["test"]), "pg_isready -U flowsql_user -d flowsql_db",
                  "PostgreSQL must report database readiness") &&
         ok;
    ok = Expect(root["services"]["scheduler"]["depends_on"]["postgres"]["condition"].as<std::string>() ==
                    "service_healthy",
                "Scheduler must wait for PostgreSQL readiness") &&
         ok;
    return ok;
}

}  // namespace

int main() {
    bool ok = TestDockerfile();
    ok = TestCompose() && ok;
    ok = TestFullComposePostgres() && ok;
    if (!ok) return 1;
    std::cout << "Docker deployment contract tests passed\n";
    return 0;
}
