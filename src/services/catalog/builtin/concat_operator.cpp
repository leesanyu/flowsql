#include "concat_operator.h"

#include <framework/core/dataframe.h>
#include <framework/interfaces/idataframe_channel.h>

#include <sstream>
#include <vector>

namespace flowsql {
namespace catalog {

bool ConcatOperator::SchemaCompatible(const std::vector<Field>& lhs, const std::vector<Field>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].name != rhs[i].name) return false;
        if (lhs[i].type != rhs[i].type) return false;
    }
    return true;
}

int ConcatOperator::Configure(const char*, const char*) {
    return 0;
}

int ConcatOperator::Work(IChannel*, IChannel*) {
    last_error_ = "concat requires at least 2 input channels";
    return -1;
}

int ConcatOperator::Work(Span<IChannel*> inputs, IChannel* out) {
    last_error_.clear();
    if (inputs.size < 2) {
        last_error_ = "concat requires at least 2 input channels";
        return -1;
    }

    auto* df_out = dynamic_cast<IDataFrameChannel*>(out);
    if (!df_out) {
        last_error_ = "concat output must be dataframe channel";
        return -1;
    }

    DataFrame merged;
    std::vector<Field> base_schema;
    bool has_schema = false;

    for (size_t i = 0; i < inputs.size; ++i) {
        auto* df_in = dynamic_cast<IDataFrameChannel*>(inputs[i]);
        if (!df_in) {
            last_error_ = "concat input must be dataframe channel";
            return -1;
        }

        DataFrame data;
        if (df_in->Read(&data) != 0) {
            last_error_ = "concat failed to read input dataframe";
            return -1;
        }

        std::vector<Field> schema = data.GetSchema();
        if (!has_schema) {
            base_schema = schema;
            merged.SetSchema(base_schema);
            has_schema = true;
        } else if (!SchemaCompatible(base_schema, schema)) {
            std::ostringstream oss;
            oss << "concat schema mismatch at input index " << i;
            last_error_ = oss.str();
            return -1;
        }

        int32_t rows = data.RowCount();
        for (int32_t r = 0; r < rows; ++r) {
            if (merged.AppendRow(data.GetRow(r)) != 0) {
                last_error_ = "concat failed to append row";
                return -1;
            }
        }
    }

    if (!has_schema) {
        last_error_ = "concat has no input schema";
        return -1;
    }

    if (df_out->Write(&merged) != 0) {
        last_error_ = "concat failed to write output dataframe";
        return -1;
    }
    return 0;
}

}  // namespace catalog
}  // namespace flowsql
