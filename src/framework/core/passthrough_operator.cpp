#include "passthrough_operator.h"

#include <cstdio>

#include <framework/core/dataframe.h>
#include <framework/interfaces/idataframe_channel.h>

namespace flowsql {

int PassthroughOperator::Work(IChannel* in, IChannel* out) {
    auto* df_in = dynamic_cast<IDataFrameChannel*>(in);
    auto* df_out = dynamic_cast<IDataFrameChannel*>(out);
    if (!df_in || !df_out) {
        printf("PassthroughOperator::Work: channel type mismatch\n");
        return -1;
    }

    DataFrame data;
    if (df_in->Read(&data) != 0) {
        printf("PassthroughOperator::Work: Read failed\n");
        return -1;
    }

    if (df_out->Write(&data) != 0) {
        printf("PassthroughOperator::Work: Write failed\n");
        return -1;
    }

    return 0;
}

}  // namespace flowsql
