#include "memory_channel.h"

namespace flowsql {

int MemoryChannel::Open() {
    opened_ = true;
    return 0;
}

int MemoryChannel::Close() {
    opened_ = false;
    data_.Clear();
    return 0;
}

int MemoryChannel::Write(IDataFrame* df) {
    if (!opened_ || !df) return -1;
    // 替换语义
    data_.Clear();
    data_.SetSchema(df->GetSchema());
    for (int32_t i = 0; i < df->RowCount(); ++i) {
        data_.AppendRow(df->GetRow(i));
    }
    return 0;
}

int MemoryChannel::Read(IDataFrame* df) {
    if (!opened_ || !df) return -1;
    // 快照语义
    df->SetSchema(data_.GetSchema());
    for (int32_t i = 0; i < data_.RowCount(); ++i) {
        df->AppendRow(data_.GetRow(i));
    }
    return 0;
}

}  // namespace flowsql
