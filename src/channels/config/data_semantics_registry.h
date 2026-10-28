// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <mutex>
#include "data_semantics_model.h"
struct sqlite3;
namespace flowsql::channels::config {
struct SemanticsSaveRequest {
    std::string kind, name, document_json, change_note;
    uint64_t expected_current_revision = 0, base_revision = 0;
    // Internal only: set by channels/apply after re-observing the provider. Never imported from JSON.
    bool source_observed = false;
};
struct SemanticsSaveResult {
    DataSemanticsSnapshotV1 snapshot;
    bool created_revision = false;
    std::string structure_check;
    std::string data_validation = "not_run";
};
struct SemanticsReferenceItem {
    std::string source_reference, field_path;
    bool is_current = false;
};
class DataSemanticsRegistry : public IDataSemanticsRegistryV1 {
 public:
    DataSemanticsRegistry() = default;
    ~DataSemanticsRegistry() override;
    DataSemanticsRegistry(const DataSemanticsRegistry&) = delete;
    DataSemanticsRegistry& operator=(const DataSemanticsRegistry&) = delete;
    int Open(const std::string& db_path, DataSemanticsErrorV1* error);
    void Close();
    int Preview(const std::string& kind, const std::string& content, rapidjson::Document* out,
                std::string* structure_check, DataSemanticsErrorV1* error);
    int32_t Control(const std::string& operation, const std::string& request, std::string& response);
    int Save(const SemanticsSaveRequest& request, SemanticsSaveResult* out, DataSemanticsErrorV1* error);
    int Resolve(const char* reference, DataSemanticsSnapshotV1* out, DataSemanticsErrorV1* error) override;

    int BindingLookup(const std::string& provider, const std::string& object, DataSemanticsSnapshotV1* out,
                      DataSemanticsErrorV1* error);
    int ObservationTrusted(const std::string& reference, bool* trusted, DataSemanticsErrorV1* error);
    int References(const std::string& target, bool history, std::vector<SemanticsReferenceItem>* out,
                   DataSemanticsErrorV1* error);

 private:
    int ValidateLinks(const std::string& kind, const rapidjson::Value& document,
                      const std::vector<SemanticsReference>& references, std::string* structure_check,
                      DataSemanticsErrorV1* error);
    int Read(const std::string& reference, DataSemanticsSnapshotV1* out, DataSemanticsErrorV1* error);
    int StorageError(DataSemanticsErrorV1* error);
    std::mutex mutex_;
    sqlite3* db_ = nullptr;
};
}  // namespace flowsql::channels::config
