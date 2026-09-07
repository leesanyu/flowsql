// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifdef PLUGIN_LOADER_FIXTURE_ID

#include <common/iplugin.h>

#include <cstdlib>
#include <fstream>
#include <string>

namespace {

class PluginLoaderFixture final : public flowsql::IPlugin {
 public:
    int Option(const char* option) override {
        log_path_.clear();
        option_rc_ = 0;
        load_rc_ = 0;
        start_rc_ = 0;
        ParseOption(option);
        Append("Option");
        return option_rc_;
    }

    int Load(flowsql::IQuerier*) override {
        Append("Load");
        return load_rc_;
    }

    int Unload() override {
        Append("Unload");
        return 0;
    }

    int Start() override {
        Append("Start");
        return start_rc_;
    }

    int Stop() override {
        Append("Stop");
        return 0;
    }

 private:
    void ParseOption(const char* option) {
        const std::string text = option ? option : "";
        size_t begin = 0;
        while (begin <= text.size()) {
            size_t end = text.find(';', begin);
            if (end == std::string::npos) end = text.size();
            const std::string field = text.substr(begin, end - begin);
            const size_t equal = field.find('=');
            if (equal != std::string::npos) {
                const std::string key = field.substr(0, equal);
                const std::string value = field.substr(equal + 1);
                if (key == "log") {
                    log_path_ = value;
                } else if (key == "option_rc") {
                    option_rc_ = std::atoi(value.c_str());
                } else if (key == "load_rc") {
                    load_rc_ = std::atoi(value.c_str());
                } else if (key == "start_rc") {
                    start_rc_ = std::atoi(value.c_str());
                }
            }
            if (end == text.size()) break;
            begin = end + 1;
        }
    }

    void Append(const char* phase) const {
        if (log_path_.empty()) return;
        std::ofstream output(log_path_, std::ios::app);
        output << PLUGIN_LOADER_FIXTURE_ID << '.' << phase << '\n';
    }

    std::string log_path_;
    int option_rc_ = 0;
    int load_rc_ = 0;
    int start_rc_ = 0;
};

}  // namespace

BEGIN_PLUGIN_REGIST(PluginLoaderFixture)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
#ifdef PLUGIN_LOADER_FIXTURE_EXTERNAL_ENTRY
    ____INTERFACE(flowsql::IID_PLUGIN_EXTERNAL_ENTRY, flowsql::IPlugin)
#endif
END_PLUGIN_REGIST()

#else

#include <common/loader.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

class TemporaryDirectory {
 public:
    TemporaryDirectory() {
        std::string pattern = (std::filesystem::temp_directory_path() / "flowsql-plugin-loader-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        char* created = ::mkdtemp(writable.data());
        if (created) path_ = created;
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    bool valid() const { return !path_.empty(); }

    std::string LogPath(const char* name) const { return (path_ / name).string(); }

 private:
    std::filesystem::path path_;
};

std::vector<std::string> ReadEvents(const std::string& path) {
    std::ifstream input(path);
    std::vector<std::string> events;
    std::string event;
    while (std::getline(input, event)) events.push_back(event);
    return events;
}

bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAILED: " << message << '\n';
    return false;
}

bool ExpectEvents(const std::string& path, const std::vector<std::string>& expected, const char* message) {
    const std::vector<std::string> actual = ReadEvents(path);
    if (actual == expected) return true;

    std::cerr << "FAILED: " << message << "\n  expected:";
    for (const auto& event : expected) std::cerr << ' ' << event;
    std::cerr << "\n  actual:";
    for (const auto& event : actual) std::cerr << ' ' << event;
    std::cerr << '\n';
    return false;
}

std::string MakeOption(const std::string& log_path, const char* extra = nullptr) {
    std::string option = "log=" + log_path;
    if (extra && extra[0] != '\0') option += ";" + std::string(extra);
    return option;
}

bool TestSuccessfulBatch(flowsql::PluginLoader* loader, const char* libraries[3], const std::string& log_path) {
    const std::string a = MakeOption(log_path);
    const std::string b = MakeOption(log_path);
    const std::string c = MakeOption(log_path);
    const char* options[] = {a.c_str(), b.c_str(), c.c_str()};

    bool ok = Expect(loader->Load(".", libraries, options, 3) == 0, "successful batch Load must succeed");
    ok = ExpectEvents(log_path,
                      {"a.Option", "b.Option", "c.Option", "a.Load", "b.Load", "c.Load"},
                      "all Option calls must precede every Load call") &&
         ok;
    ok = Expect(loader->StartAll() == 0, "successful batch StartAll must succeed") && ok;
    ok = ExpectEvents(log_path,
                      {"a.Option", "b.Option", "c.Option", "a.Load", "b.Load", "c.Load", "b.Start", "c.Start",
                       "a.Start"},
                      "internal plugins must start before the external entry") &&
         ok;
    loader->StopAll();
    ok = ExpectEvents(log_path,
                      {"a.Option", "b.Option", "c.Option", "a.Load", "b.Load", "c.Load", "b.Start", "c.Start",
                       "a.Start", "a.Stop", "c.Stop", "b.Stop"},
                      "StopAll must reverse the actual phase-aware start order") &&
         ok;
    loader->Unload();
    return ok;
}

bool TestPositiveOptionFailure(flowsql::PluginLoader* loader,
                               const char* libraries[3],
                               const std::string& log_path) {
    const std::string a = MakeOption(log_path);
    const std::string b = MakeOption(log_path, "option_rc=7");
    const std::string c = MakeOption(log_path);
    const char* options[] = {a.c_str(), b.c_str(), c.c_str()};

    bool ok = Expect(loader->Load(".", libraries, options, 3) != 0, "positive Option return must fail the batch");
    ok = ExpectEvents(log_path, {"a.Option", "b.Option"}, "Option failure must prevent every Load call") && ok;
    ok = Expect(loader->First(flowsql::IID_PLUGIN) == nullptr, "Option failure must remove batch registrations") && ok;
    loader->Unload();
    return ok;
}

bool TestPositiveLoadFailure(flowsql::PluginLoader* loader,
                             const char* libraries[3],
                             const std::string& log_path) {
    const std::string a = MakeOption(log_path);
    const std::string b = MakeOption(log_path, "load_rc=9");
    const std::string c = MakeOption(log_path);
    const char* options[] = {a.c_str(), b.c_str(), c.c_str()};

    bool ok = Expect(loader->Load(".", libraries, options, 3) != 0, "positive Load return must fail the batch");
    ok = ExpectEvents(log_path,
                      {"a.Option", "b.Option", "c.Option", "a.Load", "b.Load", "b.Unload", "a.Unload"},
                      "Load failure must unload attempted plugins in reverse order") &&
         ok;
    ok = Expect(loader->First(flowsql::IID_PLUGIN) == nullptr, "Load failure must remove batch registrations") && ok;
    loader->Unload();
    return ok;
}

bool TestPositiveStartFailure(flowsql::PluginLoader* loader,
                              const char* libraries[3],
                              const std::string& log_path) {
    const std::string a = MakeOption(log_path);
    const std::string b = MakeOption(log_path);
    const std::string c = MakeOption(log_path, "start_rc=11");
    const char* options[] = {a.c_str(), b.c_str(), c.c_str()};

    bool ok = Expect(loader->Load(".", libraries, options, 3) == 0, "Start failure fixture Load must succeed");
    ok = Expect(loader->StartAll() != 0, "positive Start return must fail StartAll") && ok;
    const std::vector<std::string> expected = {
        "a.Option", "b.Option", "c.Option", "a.Load", "b.Load", "c.Load",
        "b.Start",  "c.Start",  "b.Stop",
    };
    ok = ExpectEvents(log_path, expected, "internal Start failure must not activate the external entry") && ok;
    loader->StopAll();
    ok = ExpectEvents(log_path, expected, "Start failure rollback must leave no plugin marked started") && ok;
    loader->Unload();
    return ok;
}

bool TestExternalEntryStartFailure(flowsql::PluginLoader* loader,
                                   const char* libraries[3],
                                   const std::string& log_path) {
    const std::string a = MakeOption(log_path, "start_rc=13");
    const std::string b = MakeOption(log_path);
    const std::string c = MakeOption(log_path);
    const char* options[] = {a.c_str(), b.c_str(), c.c_str()};

    bool ok = Expect(loader->Load(".", libraries, options, 3) == 0, "external failure fixture Load must succeed");
    ok = Expect(loader->StartAll() != 0, "external entry Start failure must fail StartAll") && ok;
    const std::vector<std::string> expected = {
        "a.Option", "b.Option", "c.Option", "a.Load", "b.Load", "c.Load",
        "b.Start",  "c.Start",  "a.Start",  "c.Stop", "b.Stop",
    };
    ok = ExpectEvents(log_path, expected, "external failure must roll back internal starts in reverse order") && ok;
    loader->StopAll();
    ok = ExpectEvents(log_path, expected, "external failure rollback must leave no plugin marked started") && ok;
    loader->Unload();
    return ok;
}

bool TestProductionExternalEntryMarkers(flowsql::PluginLoader* loader, const char* libraries[3]) {
    bool ok = Expect(loader->Load(".", libraries, nullptr, 3) == 0,
                     "Gateway, Router and Web registration batch must load");
    std::vector<void*> plugins;
    std::vector<void*> external_entries;
    loader->Traverse(flowsql::IID_PLUGIN, [&](void* plugin) {
        plugins.push_back(plugin);
        return 0;
    });
    loader->Traverse(flowsql::IID_PLUGIN_EXTERNAL_ENTRY, [&](void* plugin) {
        external_entries.push_back(plugin);
        return 0;
    });
    ok = Expect(plugins.size() == 3, "production registration batch must expose three plugins") && ok;
    ok = Expect(external_entries.size() == 3, "Gateway, Router and Web must all be external entries") && ok;
    for (void* entry : external_entries) {
        ok = Expect(std::find(plugins.begin(), plugins.end(), entry) != plugins.end(),
                    "external entry marker must hold the same IPlugin pointer") &&
             ok;
    }
    loader->Unload();
    return ok;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 7) {
        std::cerr << "usage: test_plugin_loader <fixture-a> <fixture-b> <fixture-c> <gateway> <router> <web>\n";
        return 2;
    }

    TemporaryDirectory temporary;
    if (!temporary.valid()) {
        std::cerr << "FAILED: cannot create temporary directory\n";
        return 2;
    }

    const char* libraries[] = {argv[1], argv[2], argv[3]};
    flowsql::PluginLoader* loader = flowsql::PluginLoader::Single();
    loader->StopAll();
    loader->Unload();

    bool ok = true;
    ok = TestSuccessfulBatch(loader, libraries, temporary.LogPath("success.log")) && ok;
    ok = TestPositiveOptionFailure(loader, libraries, temporary.LogPath("option-failure.log")) && ok;
    ok = TestPositiveLoadFailure(loader, libraries, temporary.LogPath("load-failure.log")) && ok;
    ok = TestPositiveStartFailure(loader, libraries, temporary.LogPath("start-failure.log")) && ok;
    ok = TestExternalEntryStartFailure(loader, libraries, temporary.LogPath("external-start-failure.log")) && ok;
    const char* production_entries[] = {argv[4], argv[5], argv[6]};
    ok = TestProductionExternalEntryMarkers(loader, production_entries) && ok;

    std::cout << "PluginLoader lifecycle contract tests " << (ok ? "passed" : "failed") << '\n';
    return ok ? 0 : 1;
}

#endif
