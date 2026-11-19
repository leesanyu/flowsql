// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <list>
#include <map>
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
    DataSemanticsSnapshot snapshot;
    bool created_revision = false;
    std::string structure_check;
    std::string data_validation = "not_run";
};
struct SemanticsReferenceItem {
    std::string source_reference, field_path;
    bool is_current = false;
};
class DataSemanticsRegistry : public IDataSemanticsRegistry {
 public:
    // Includes owned text/DOM allocations and conservative index/node overhead.
    static constexpr size_t kDefinitionCacheBudget = 64 * 1024 * 1024;
    struct ReadStatistics {
        uint64_t body_queries = 0, parses = 0, digests = 0, publications = 0, hits = 0, evictions = 0;
        size_t retained_bytes = 0;
    };
    explicit DataSemanticsRegistry(size_t budget = kDefinitionCacheBudget) : cache_budget_(budget) {}
    ReadStatistics Statistics() const;
    ~DataSemanticsRegistry() override;
    DataSemanticsRegistry(const DataSemanticsRegistry&) = delete;
    DataSemanticsRegistry& operator=(const DataSemanticsRegistry&) = delete;
    int Open(const std::string& db_path, DataSemanticsError* error);
    void Close();
    int Preview(const std::string& kind, const std::string& content, rapidjson::Document* out,
                std::string* structure_check, DataSemanticsError* error);
    int32_t Control(const std::string& operation, const std::string& request, std::string& response);
    int Save(const SemanticsSaveRequest& request, SemanticsSaveResult* out, DataSemanticsError* error);
    // Trusted execution only; one write transaction publishes all output metadata.
    int SaveBatch(const std::vector<SemanticsSaveRequest>& requests, std::vector<SemanticsSaveResult>* out,
                  DataSemanticsError* error);
    int Resolve(const char* reference, DataSemanticsSnapshot* out, DataSemanticsError* error) override;

    int BindingLookup(const std::string& provider, const std::string& object, DataSemanticsSnapshot* out,
                      DataSemanticsError* error);
    int ObservationTrusted(const std::string& reference, bool* trusted, DataSemanticsError* error);
    int References(const std::string& target, bool history, std::vector<SemanticsReferenceItem>* out,
                   DataSemanticsError* error);

 private:
    int SaveUnlocked(const SemanticsSaveRequest& request, const DataSemanticsSnapshot& definition,
                     const std::vector<SemanticsReference>& references, bool execution_owned, SemanticsSaveResult* out,
                     DataSemanticsError* error);
    int WriteBatch(const std::vector<SemanticsSaveRequest>& requests, bool execution_owned,
                   std::vector<SemanticsSaveResult>* out, DataSemanticsError* error);
    int ValidateLinks(const std::string& kind, const rapidjson::Value& document,
                      const std::vector<SemanticsReference>& references, std::string* structure_check,
                      DataSemanticsError* error);
    int Read(const std::string& reference, DataSemanticsSnapshot* out, DataSemanticsError* error);
    int ReadBody(const std::string& reference, std::string* content, std::string* digest, DataSemanticsError* error);
    int Decode(const std::string& reference, std::string content, const std::string& digest, DataSemanticsSnapshot* out,
               DataSemanticsError* error);
    bool Cached(const std::string& reference, DataSemanticsSnapshot* out);
    int RefreshCache(DataSemanticsError* error);
    void PublishDefinition(const DataSemanticsSnapshot& snapshot);
    int StorageError(DataSemanticsError* error);
    std::mutex mutex_;
    sqlite3* db_ = nullptr;
    uint64_t generation_ = 0;
    int data_version_ = 0;
    // Lock order: mutex_ -> cache_mutex_. Cold loading never holds cache_mutex_ while acquiring mutex_.
    std::mutex cold_mutex_;
    mutable std::mutex cache_mutex_;
    struct CacheEntry {
        DataSemanticsSnapshot snapshot;
        size_t bytes;
    };
    const size_t cache_budget_;
    std::list<CacheEntry> cache_;
    std::map<std::string, std::list<CacheEntry>::iterator> cache_index_;
    ReadStatistics statistics_;
};
}  // namespace flowsql::channels::config
