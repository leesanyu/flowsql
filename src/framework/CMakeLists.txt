project(flowsql_common)

# 公共基础设施：DataFrame、DataFrameChannel、SqlParser
# 不含 PluginRegistry（已删除）、Pipeline/ChannelAdapter（移入 scheduler.so）
add_library(${PROJECT_NAME} SHARED
    builtin/dataframe/concat_operator.cpp
    builtin/dataframe/dataframe_dispatch_stream_operator.cpp
    builtin/dataframe/hstack_operator.cpp
    builtin/dataframe/passthrough_operator.cpp
    builtin/stream/count_window_stream_operator.cpp
    builtin/stream/passthrough_stream_operator.cpp
    builtin/stream/tcp_service_merge_stream_operator.cpp
    builtin/stream/tcp_session_mock_stream_channel.cpp
    core/dataframe.cpp
    core/dataframe_channel.cpp
    core/error_contract.cpp
    core/fan_in_stream_channel.cpp
    core/fan_out_stream_channel.cpp
    core/filter_expression.cpp
    core/filter_binding.cpp
    core/filter_executor.cpp
    core/filter_planner.cpp
    core/json_error_builder.cpp
    core/memory_channel.cpp
    core/packet_codec.cpp
    core/ring_stream_channel.cpp
    core/scheduler_control_client.cpp
    core/sql_text_splitter.cpp
    core/stream_channel_adapter.cpp
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
