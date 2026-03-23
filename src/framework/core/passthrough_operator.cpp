#include "passthrough_operator.h"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <thread>

#include <framework/core/dataframe.h>
#include <framework/interfaces/idataframe_channel.h>

namespace flowsql {

int PassthroughOperator::Configure(const char* key, const char* value) {
    if (!key || !value) return 0;
    if (std::string(key) == "force_fail") {
        force_fail_ = (std::string(value) == "1" || std::string(value) == "true");
        return 0;
    }
    if (std::string(key) != "delay_ms") return 0;
    char* end = nullptr;
    long v = std::strtol(value, &end, 10);
    if (end == value || (end && *end != '\0')) return 0;
    if (v < 0) v = 0;
    if (v > 60000) v = 60000;
    delay_ms_ = static_cast<int>(v);
    return 0;
}

int PassthroughOperator::Work(IChannel* in, IChannel* out) {
    if (force_fail_) {
        printf("PassthroughOperator::Work: forced failure\n");
        return -1;
    }

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

    if (delay_ms_ > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
    }

    return 0;
}

}  // namespace flowsql
