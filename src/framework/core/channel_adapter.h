#ifndef _FLOWSQL_FRAMEWORK_CORE_CHANNEL_ADAPTER_H_
#define _FLOWSQL_FRAMEWORK_CORE_CHANNEL_ADAPTER_H_

#include "framework/interfaces/idatabase_channel.h"
#include "framework/interfaces/idataframe_channel.h"

namespace flowsql {

// ChannelAdapter — 通道间格式转换工具类
// 封装 Database ↔ DataFrame 的数据搬运逻辑，供 Scheduler 自动适配使用
class ChannelAdapter {
 public:
    // Database → DataFrame：执行查询并将结果写入 DataFrameChannel
    // query 为空时读取整表（需要 table 参数拼接 SELECT *）
    static int ReadToDataFrame(IDatabaseChannel* db, const char* query,
                               IDataFrameChannel* df_out);

    // DataFrame → Database：将 DataFrameChannel 数据写入数据库表
    // 返回：成功返回写入的行数（>= 0），失败返回 -1
    static int64_t WriteFromDataFrame(IDataFrameChannel* df_in,
                                      IDatabaseChannel* db, const char* table);

    // DataFrame → DataFrame：纯数据搬运（无算子场景）
    static int CopyDataFrame(IDataFrameChannel* src, IDataFrameChannel* dst);
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_CHANNEL_ADAPTER_H_
