// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <services/gateway/config.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

constexpr const char* kUploadRoot = "/tmp/flowsql/uploads";
constexpr const char* kNpiPlugin = "libflowsql_npi.so";
constexpr const char* kPcapFilePlugin = "libflowsql_pcapfile.so";
constexpr const char* kNpiOption = "{\"ldfile\":\"./config/protocols.yml\"}";
constexpr const char* kPcapFileOption = "db_path=./meta/flowsql_meta.db";

bool Expect(bool condition, const std::string& message) {
    if (condition) return true;
    std::cerr << "FAILED: " << message << '\n';
    return false;
}

const flowsql::gateway::ServiceConfig* FindService(const flowsql::gateway::GatewayConfig& config,
                                                    const std::string& name) {
    const auto it = std::find_if(config.services.begin(), config.services.end(), [&](const auto& service) {
        return service.name == name;
    });
    return it == config.services.end() ? nullptr : &*it;
}

const std::string* FindPlugin(const flowsql::gateway::ServiceConfig& service, const std::string& library) {
    const auto it = std::find_if(service.plugins.begin(), service.plugins.end(), [&](const auto& plugin) {
        return plugin == library || plugin.rfind(library + ":", 0) == 0;
    });
    return it == service.plugins.end() ? nullptr : &*it;
}

std::string PluginOption(const std::string& plugin) {
    const size_t colon = plugin.find(':');
    return colon == std::string::npos ? std::string() : plugin.substr(colon + 1);
}

bool ExpectSchedulerProviders(const flowsql::gateway::ServiceConfig& service, const std::string& deployment) {
    bool ok = true;
    const std::string* npi = FindPlugin(service, kNpiPlugin);
    ok = Expect(npi != nullptr, deployment + " Scheduler process must load NPI") && ok;
    if (npi) {
        ok = Expect(PluginOption(*npi) == kNpiOption, deployment + " must preserve the complete NPI JSON option") &&
             ok;
    }
    const std::string* pcapfile = FindPlugin(service, kPcapFilePlugin);
    ok = Expect(pcapfile != nullptr,
                deployment + " Scheduler process must load the pcapfile provider") &&
         ok;
    if (pcapfile) {
        ok = Expect(PluginOption(*pcapfile) == kPcapFileOption,
                    deployment + " pcapfile must persist in the runtime meta database") &&
             ok;
    }
    return ok;
}

bool ExpectWebUploadRoot(const flowsql::gateway::ServiceConfig& service, const std::string& deployment) {
    const std::string* web = FindPlugin(service, "libflowsql_web.so");
    bool ok = Expect(web != nullptr, deployment + " must load Web");
    if (!web) return false;

    const std::string option = PluginOption(*web);
    ok = Expect(option.find("upload_dir=" + std::string(kUploadRoot)) != std::string::npos,
                deployment + " Web must use the frozen absolute upload root") &&
         ok;
    ok = Expect(option.find("upload_dir=./") == std::string::npos,
                deployment + " Web must not depend on the process working directory") &&
         ok;
    return ok;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool TestSingleProcessConfig() {
    flowsql::gateway::GatewayConfig config;
    bool ok = Expect(flowsql::gateway::LoadConfig(FLOWSQL_DEPLOY_SINGLE_PATH, &config) == 0,
                     "single-process deployment YAML must parse");
    const auto* all = FindService(config, "all");
    ok = Expect(all != nullptr, "single-process deployment must contain the all service") && ok;
    if (!all) return false;
    const bool providers_ok = ExpectSchedulerProviders(*all, "single-process deployment");
    const bool web_ok = ExpectWebUploadRoot(*all, "single-process deployment");
    return providers_ok && web_ok && ok;
}

bool TestGuardianConfig() {
    flowsql::gateway::GatewayConfig config;
    bool ok = Expect(flowsql::gateway::LoadConfig(FLOWSQL_DEPLOY_MULTI_PATH, &config) == 0,
                     "Guardian deployment YAML must parse");
    const auto* web = FindService(config, "web");
    const auto* scheduler = FindService(config, "scheduler");
    ok = Expect(web != nullptr, "Guardian deployment must contain the Web service") && ok;
    ok = Expect(scheduler != nullptr, "Guardian deployment must contain the Scheduler service") && ok;
    if (!web || !scheduler) return false;
    const bool web_ok = ExpectWebUploadRoot(*web, "Guardian deployment");
    const bool providers_ok = ExpectSchedulerProviders(*scheduler, "Guardian deployment");
    return web_ok && providers_ok && ok;
}

bool ExpectMatchingAsset(const std::filesystem::path& source,
                         const std::filesystem::path& output,
                         const std::string& name) {
    bool ok = Expect(std::filesystem::is_regular_file(source), "source " + name + " must exist");
    ok = Expect(std::filesystem::is_regular_file(output), "flowsql build must stage output/config/" + name) && ok;
    if (!ok) return false;
    return Expect(ReadFile(source) == ReadFile(output), "staged " + name + " must match its source exactly");
}

bool TestRuntimeAssets() {
    bool ok = ExpectMatchingAsset(FLOWSQL_DEPLOY_SINGLE_PATH, FLOWSQL_DEPLOY_SINGLE_OUTPUT_PATH,
                                  "deploy-single.yaml");
    ok = ExpectMatchingAsset(FLOWSQL_DEPLOY_MULTI_PATH, FLOWSQL_DEPLOY_MULTI_OUTPUT_PATH, "deploy-multi.yaml") && ok;
    ok = ExpectMatchingAsset(FLOWSQL_NPI_PROTOCOLS_SOURCE_PATH, FLOWSQL_NPI_PROTOCOLS_OUTPUT_PATH, "protocols.yml") &&
         ok;
    return ok;
}

bool TestStartScriptRuntimeLayout() {
    const std::filesystem::path repository_root =
        std::filesystem::path(FLOWSQL_DEPLOY_SINGLE_PATH).parent_path().parent_path();
    const std::filesystem::path start_script = repository_root / "start.sh";
    const std::filesystem::path runtime_root = repository_root / "build/output";
    const std::string script = ReadFile(start_script);

    bool ok = Expect(!script.empty(), "start.sh must be readable");
    ok = Expect(script.find("cd \"$(dirname \"$0\")/build/output\"") != std::string::npos,
                "start.sh must enter build/output before starting FlowSQL") &&
         ok;
    ok = Expect(script.find("LD_LIBRARY_PATH") != std::string::npos,
                "start.sh must expose build/output through LD_LIBRARY_PATH") &&
         ok;
    ok = Expect(script.find("exec ./flowsql --config config/deploy-single.yaml") != std::string::npos,
                "start.sh must use the staged single-process config") &&
         ok;
    ok = Expect(std::filesystem::is_regular_file(runtime_root / "flowsql"),
                "start.sh runtime must contain the FlowSQL executable") &&
         ok;
    ok = Expect(std::filesystem::is_regular_file(runtime_root / "config/deploy-single.yaml"),
                "start.sh runtime must contain the staged single-process config") &&
         ok;
    ok = Expect(std::filesystem::is_regular_file(runtime_root / "config/protocols.yml"),
                "start.sh runtime must contain the staged NPI protocol definition") &&
         ok;
    return ok;
}

bool TestStartScriptFrontendBuildOption() {
    const std::filesystem::path repository_root =
        std::filesystem::path(FLOWSQL_DEPLOY_SINGLE_PATH).parent_path().parent_path();
    const std::string script = ReadFile(repository_root / "start.sh");

    bool ok = Expect(script.find("--build-frontend") != std::string::npos,
                     "start.sh must expose the optional frontend build flag");
    ok = Expect(script.find("npm run build --prefix") != std::string::npos,
                "frontend build flag must run the production frontend build") &&
         ok;
    ok = Expect(script.find("src/frontend") != std::string::npos,
                "frontend build must target the repository frontend project") &&
         ok;
    ok = Expect(script.find("cmake -E remove_directory") != std::string::npos,
                "frontend deployment must remove stale static assets") &&
         ok;
    ok = Expect(script.find("cmake -E copy_directory") != std::string::npos,
                "frontend deployment must copy the complete dist directory") &&
         ok;
    ok = Expect(script.find("src/frontend/dist") != std::string::npos,
                "frontend deployment must use the production dist directory") &&
         ok;
    ok = Expect(script.find("build/output/static") != std::string::npos,
                "frontend deployment must target the Web runtime static directory") &&
         ok;
    ok = Expect(script.find("build/output/static/index.html") != std::string::npos,
                "frontend deployment must verify the deployed entry point") &&
         ok;
    ok = Expect(script.find("Unknown option") != std::string::npos,
                "start.sh must reject unknown options before starting the service") &&
         ok;
    return ok;
}

bool TestPcapFilePersistenceDocumentation() {
    const std::filesystem::path repository_root =
        std::filesystem::path(FLOWSQL_DEPLOY_SINGLE_PATH).parent_path().parent_path();
    const std::string readme = ReadFile(repository_root / "README.md");

    bool ok = Expect(readme.find("PCAP 文件通道持久化") != std::string::npos,
                     "README must document pcapfile persistence");
    ok = Expect(readme.find(kPcapFileOption) != std::string::npos,
                "README must document the native pcapfile database path") &&
         ok;
    ok = Expect(readme.find("/opt/flowsql/uploads/.meta/pcapfile.db") != std::string::npos,
                "README must document the Docker pcapfile database path") &&
         ok;
    ok = Expect(readme.find("SQL 任务执行完成") != std::string::npos,
                "README must explain that completed SQL does not remove the base channel") &&
         ok;
    ok = Expect(readme.find("显式删除通道") != std::string::npos,
                "README must distinguish explicit channel deletion") &&
         ok;
    ok = Expect(readme.find("pcap-uploads") != std::string::npos,
                "README must identify the Docker persistence volume") &&
         ok;
    return ok;
}

}  // namespace

int main() {
    bool ok = TestSingleProcessConfig();
    ok = TestGuardianConfig() && ok;
    ok = TestRuntimeAssets() && ok;
    ok = TestStartScriptRuntimeLayout() && ok;
    ok = TestStartScriptFrontendBuildOption() && ok;
    ok = TestPcapFilePersistenceDocumentation() && ok;
    if (!ok) return 1;
    std::cout << "Native deployment contract tests passed\n";
    return 0;
}
