project(flowsql_common)

# 公共基础设施：DataFrame、DataFrameChannel、SqlParser
# 不含 PluginRegistry（已删除）、Pipeline/ChannelAdapter（移入 scheduler.so）
add_library(${PROJECT_NAME} SHARED
    core/dataframe.cpp
    core/dataframe_channel.cpp
    core/memory_channel.cpp
    core/passthrough_operator.cpp
    core/sql_parser.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow rapidjson)

set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET default
)

# 覆盖全局 -fvisibility=hidden，共享库需要导出符号
target_compile_options(${PROJECT_NAME} PRIVATE -fvisibility=default)
