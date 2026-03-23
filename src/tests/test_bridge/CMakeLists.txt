project(test_bridge)

file(GLOB_RECURSE DIR_SRCS *.cpp)

add_executable(${PROJECT_NAME} ${DIR_SRCS})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow rapidjson httplib)

add_dependencies(${PROJECT_NAME} flowsql_common flowsql_bridge)
target_link_libraries(${PROJECT_NAME} flowsql_common)

# 直接链接 bridge 的 .o 文件（测试不需要加载 .so）
target_sources(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/services/bridge/arrow_ipc_serializer.cpp
    ${CMAKE_SOURCE_DIR}/services/bridge/bridge_plugin.cpp
    ${CMAKE_SOURCE_DIR}/services/bridge/python_operator_bridge.cpp
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
