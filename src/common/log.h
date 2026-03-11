#ifndef _FLOWSQL_COMMON_LOG_H_
#define _FLOWSQL_COMMON_LOG_H_

#include <cstdio>

// 日志级别
enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3, OFF = 4 };

// 全局日志级别，默认 INFO（生产环境可设为 WARN 或 OFF）
// 可在编译时通过 -DFLOWSQL_LOG_LEVEL=2 覆盖
#ifndef FLOWSQL_LOG_LEVEL
#define FLOWSQL_LOG_LEVEL 1  // INFO
#endif

#define FLOWSQL_LOG(level_val, prefix, fmt, ...)                          \
    do {                                                                   \
        if ((level_val) >= FLOWSQL_LOG_LEVEL) {                           \
            fprintf((level_val) >= 3 ? stderr : stdout,                   \
                    "[" prefix "] " fmt "\n", ##__VA_ARGS__);             \
        }                                                                  \
    } while (0)

#define LOG_DEBUG(fmt, ...) FLOWSQL_LOG(0, "DEBUG", fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  FLOWSQL_LOG(1, "INFO",  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  FLOWSQL_LOG(2, "WARN",  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) FLOWSQL_LOG(3, "ERROR", fmt, ##__VA_ARGS__)

#endif  // _FLOWSQL_COMMON_LOG_H_
