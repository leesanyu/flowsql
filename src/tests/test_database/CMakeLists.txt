project(test_database)

# 主测试（通过插件加载）
add_executable(${PROJECT_NAME} main.cpp)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow rapidjson)

add_dependencies(${PROJECT_NAME} flowsql_common flowsql_database)
target_link_libraries(${PROJECT_NAME} flowsql_common)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

# 连接池单元测试
add_executable(test_connection_pool test_connection_pool.cpp)
target_include_directories(test_connection_pool PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(test_connection_pool arrow rapidjson)
add_dependencies(test_connection_pool flowsql_common flowsql_database)
target_link_libraries(test_connection_pool flowsql_common flowsql_database)
set_target_properties(test_connection_pool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

# Session 端到端测试
add_executable(test_session_e2e test_session_e2e.cpp)
target_include_directories(test_session_e2e PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(test_session_e2e arrow rapidjson)
add_dependencies(test_session_e2e flowsql_common flowsql_database)
target_link_libraries(test_session_e2e flowsql_common flowsql_database)
set_target_properties(test_session_e2e PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
