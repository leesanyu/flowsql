#include "hstack_operator.h"

#include <framework/core/dataframe.h>
#include <framework/interfaces/idataframe_channel.h>

#include <sstream>
#include <vector>

namespace flowsql {
namespace catalog {

int HstackOperator::Configure(const char*, const char*) {
    return 0;
}

int HstackOperator::Work(IChannel*, IChannel*) {
    last_error_ = "hstack requires at least 2 input channels";
    return -1;
}

int HstackOperator::Work(Span<IChannel*> inputs, IChannel* out) {
    last_error_.clear();
    if (inputs.size < 2) {
        last_error_ = "hstack requires at least 2 input channels";
        return -1;
    }

    auto* df_out = dynamic_cast<IDataFrameChannel*>(out);
    if (!df_out) {
        last_error_ = "hstack output must be dataframe channel";
        return -1;
    }

    std::vector<DataFrame> frames(inputs.size);
    int32_t expected_rows = -1;
    std::vector<Field> merged_schema;
    size_t total_columns = 0;

    for (size_t i = 0; i < inputs.size; ++i) {
        auto* df_in = dynamic_cast<IDataFrameChannel*>(inputs[i]);
        if (!df_in) {
            last_error_ = "hstack input must be dataframe channel";
            return -1;
        }

        if (df_in->Read(&frames[i]) != 0) {
            last_error_ = "hstack failed to read input dataframe";
            return -1;
        }

        int32_t rows = frames[i].RowCount();
        if (expected_rows < 0) {
            expected_rows = rows;
        } else if (rows != expected_rows) {
            std::ostringstream oss;
            oss << "hstack row count mismatch at input index " << i
                << ": expected " << expected_rows << ", got " << rows;
            last_error_ = oss.str();
            return -1;
        }

        auto schema = frames[i].GetSchema();
        total_columns += schema.size();
        merged_schema.insert(merged_schema.end(), schema.begin(), schema.end());
    }

    DataFrame merged;
    merged.SetSchema(merged_schema);

    std::vector<FieldValue> row;
    row.reserve(total_columns);
    for (int32_t r = 0; r < expected_rows; ++r) {
        row.clear();
        for (size_t i = 0; i < frames.size(); ++i) {
            auto part = frames[i].GetRow(r);
            row.insert(row.end(), part.begin(), part.end());
        }
        if (merged.AppendRow(row) != 0) {
            last_error_ = "hstack failed to append row";
            return -1;
        }
    }

    if (df_out->Write(&merged) != 0) {
        last_error_ = "hstack failed to write output dataframe";
        return -1;
    }
    return 0;
}

}  // namespace catalog
}  // namespace flowsql
