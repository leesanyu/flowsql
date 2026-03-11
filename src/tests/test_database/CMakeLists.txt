project(test_database)

# 插件层端到端测试（通过 PluginLoader 加载 .so，走 IDatabaseFactory/IDatabaseChannel）
add_executable(${PROJECT_NAME} test_plugin_e2e.cpp)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow rapidjson)

# 注意：不直接链接 flowsql_database，避免 .so 被加载两次导致堆损坏
# flowsql_database 通过 PluginLoader 在运行时动态加载
add_dependencies(${PROJECT_NAME} flowsql_common flowsql_database)
target_link_libraries(${PROJECT_NAME} flowsql_common)
# 测试二进制必须启用 assert，取消顶层 -DNDEBUG
target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

# 连接池单元测试
add_executable(test_connection_pool test_connection_pool.cpp)
target_include_directories(test_connection_pool PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(test_connection_pool arrow rapidjson)
add_dependencies(test_connection_pool flowsql_common flowsql_database)
target_link_libraries(test_connection_pool flowsql_common flowsql_database)
target_compile_options(test_connection_pool PRIVATE -UNDEBUG)
set_target_properties(test_connection_pool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

# SQLite 驱动独立测试（每个测试自建自清，不依赖其他测试）
add_executable(test_sqlite test_sqlite.cpp)
target_include_directories(test_sqlite PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(test_sqlite arrow rapidjson)
add_dependencies(test_sqlite flowsql_common flowsql_database)
target_link_libraries(test_sqlite flowsql_common flowsql_database)
target_compile_options(test_sqlite PRIVATE -UNDEBUG)
set_target_properties(test_sqlite PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

# MySQL 驱动独立测试（需要运行中的 MySQL，通过环境变量配置连接参数）
add_executable(test_mysql test_mysql.cpp)
target_include_directories(test_mysql PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(test_mysql arrow rapidjson)
add_dependencies(test_mysql flowsql_common flowsql_database)
target_link_libraries(test_mysql flowsql_common flowsql_database)
target_compile_options(test_mysql PRIVATE -UNDEBUG)
set_target_properties(test_mysql PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
