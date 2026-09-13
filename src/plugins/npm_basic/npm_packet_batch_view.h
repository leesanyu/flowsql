// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_NPM_BASIC_NPM_PACKET_BATCH_VIEW_H_
#define _FLOWSQL_PLUGINS_NPM_BASIC_NPM_PACKET_BATCH_VIEW_H_

#include <framework/interfaces/ipacket.h>

#include <cstdint>
#include <memory>
#include <string>

namespace arrow {
class RecordBatch;
}

namespace flowsql::npm {

enum class NpmPacketBatchError : uint8_t {
    kNone = 0,
    kNullInput,
    kNullOutput,
    kSchemaMismatch,
    kInvalidColumn,
    kInvalidRow,
    kAllocationFailed,
};

/**
 * Owns an Arrow packet batch and exposes rows as borrowed packet/layer views.
 * Returned byte spans remain valid only while this batch view is alive.
 */
class NpmPacketBatchView final {
 public:
    ~NpmPacketBatchView();

    NpmPacketBatchView(const NpmPacketBatchView&) = delete;
    NpmPacketBatchView& operator=(const NpmPacketBatchView&) = delete;
    NpmPacketBatchView(NpmPacketBatchView&&) = delete;
    NpmPacketBatchView& operator=(NpmPacketBatchView&&) = delete;

    /** Validates the complete batch before publishing output. Output is unchanged on error. */
    static NpmPacketBatchError Create(const std::shared_ptr<arrow::RecordBatch>& batch,
                                      std::unique_ptr<NpmPacketBatchView>* output,
                                      std::string* error = nullptr);

    int64_t num_rows() const;

    /** Reconstructs one row without copying raw_data. Both outputs are unchanged on error. */
    NpmPacketBatchError Get(int64_t row,
                            packet::PacketView* packet_view,
                            packet::PacketLayerInfo* layer_info) const;

 private:
    struct Columns;

    NpmPacketBatchView(std::shared_ptr<arrow::RecordBatch> batch, std::unique_ptr<Columns> columns);
    static NpmPacketBatchError ValidateRow(int64_t row, const Columns& columns, std::string* error);

    std::shared_ptr<arrow::RecordBatch> batch_;
    std::unique_ptr<Columns> columns_;
};

}  // namespace flowsql::npm

#endif  // _FLOWSQL_PLUGINS_NPM_BASIC_NPM_PACKET_BATCH_VIEW_H_
