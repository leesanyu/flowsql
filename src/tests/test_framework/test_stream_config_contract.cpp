// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <framework/interfaces/istream_manager.h>
#include <rapidjson/document.h>
#include <sqlite3.h>
#include <unistd.h>
#include <common/loader.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
using namespace flowsql;
struct Runtime {
    PluginLoader* loader = PluginLoader::Single();
    ~Runtime() { loader->Unload(); }
    int Load(const std::string& directory, const std::string& yaml, const std::string& database) {
        const char* libraries[]{STREAM_CONFIG_PLUGIN, STREAM_BUILTIN_PLUGIN, STREAM_CATALOG_PLUGIN,
                                STREAM_PROVIDER_PLUGIN};
        const std::string config = "db_path=" + directory + "/semantics.db";
        const std::string catalog = "data_dir=" + directory + "/frames;operator_db_path=" + directory + "/catalog.db";
        const std::string stream = "config_file=" + yaml + ";db_path=" + database;
        const char* options[]{config.c_str(), nullptr, catalog.c_str(), stream.c_str()};
        return loader->Load(".", libraries, options, 4);
    }
    IStreamManager* Manager() { return static_cast<IStreamManager*>(loader->First(IID_STREAM_MANAGER)); }
};
void Write(const std::string& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
    assert(out.good());
}
std::string ReadOption(const std::string& path) {
    sqlite3* database = nullptr;
    assert(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    assert(sqlite3_prepare_v2(database, "SELECT option FROM stream_channel_store WHERE name='original'", -1, &statement,
                              nullptr) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    const std::string option = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return option;
}
}  // namespace
int main(int argc, char** argv) {
    char temporary[] = "/tmp/flowsql-stream-contract-XXXXXX";
    assert(mkdtemp(temporary));
    const std::string directory = temporary;
    const std::string yaml = directory + "/channels.yaml";
    const std::string database = argc == 2 ? argv[1] : directory + "/channels.db";
    Write(yaml,
          "channels:\n  stream_channels:\n    - type: ring\n      name: original\n"
          "      options: {ring_size: 64, finite: false}\n");
    {
        Runtime runtime;
        assert(runtime.Load(directory, yaml, database) == 0);
        assert(runtime.loader->StartAll() == 0);
        auto* manager = runtime.Manager();
        assert(manager);
        size_t count = 0;
        manager->QueryChannels([&](const auto&, const auto& name, const auto& option, const auto&) {
            rapidjson::Document object;
            object.Parse(option.c_str());
            assert(!object.HasParseError() && object.IsObject());
            if (argc == 1 && name == "original") {
                assert(object["ring_size"].IsInt() && object["ring_size"].GetInt() == 64);
                assert(object["finite"].IsBool() && !object["finite"].GetBool());
                assert(std::string(object["role"].GetString()) == "both");
            }
            ++count;
        });
        assert(count > 0);
        if (argc == 2) return 0;  // Validate restored definitions without adding or modifying channels.
        assert(manager->AddChannel("ring", "old", "ring_size=64") != 0);
        assert(manager->ModifyChannel("ring", "original", "ring_size=64") != 0);
        assert(manager->AddChannel("ring", "added", R"({"ring_size":64,"finite":false,"role":"both"})") == 0);
        assert(manager->ModifyChannel("ring", "added", R"({"ring_size":128,"finite":false,"role":"sink"})") == 0);
    }
    const auto original = ReadOption(database);
    {
        Runtime runtime;
        assert(runtime.Load(directory, yaml, database) == 0);
        size_t count = 0;
        runtime.Manager()->QueryChannels([&](const auto&, const auto& name, const auto& option, const auto&) {
            rapidjson::Document object;
            object.Parse(option.c_str());
            assert(object.IsObject());
            if (name == "added") {
                assert(object["ring_size"].GetInt() == 128);
                assert(std::string(object["role"].GetString()) == "sink");
            }
            ++count;
        });
        assert(count == 2);
    }
    assert(ReadOption(database) == original);
    for (const auto& definition :
         {"{type: ring, name: old, option: 'ring_size=64'}", "{config: {type: ring, name: wrapped, options: {}}}",
          "{type: ring, name: old, options: '{}'}"}) {
        Write(yaml, "channels:\n  stream_channels:\n    - " + std::string(definition) + "\n");
        Runtime runtime;
        assert(runtime.Load(directory, yaml, "") != 0);
    }
    sqlite3* stored = nullptr;
    assert(sqlite3_open(database.c_str(), &stored) == SQLITE_OK);
    assert(sqlite3_exec(stored, "UPDATE stream_channel_store SET option='ring_size=64' WHERE name='original'", nullptr,
                        nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(stored);
    {
        Runtime runtime;
        assert(runtime.Load(directory, yaml, database) != 0);
    }
    assert(ReadOption(database) == "ring_size=64");
    return 0;
}
