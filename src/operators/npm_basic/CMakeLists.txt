project(flowsql_npm_basic)

add_library(${PROJECT_NAME} SHARED
    npm_analysis_contract.cpp
    npm_basic_operator.cpp
    npm_basic_result_collector.cpp
    npm_basic_result_encoder.cpp
    npm_basic_result_projector.cpp
    npm_basic_task_config.cpp
    npm_basic_task_runtime.cpp
    npm_eof_flusher.cpp
    npm_packet_batch_view.cpp
    npm_packet_processor.cpp
    npm_parameters.cpp
    npm_protocol_context.cpp
    npm_session_key.cpp
    npm_session_analysis_module.cpp
    npm_session_result_encoder.cpp
    npm_session_table.cpp
    npm_task_budget.cpp
    npm_tcp_performance_tracker.cpp
    npm_basic_plugin_exports.cpp
    ${CMAKE_SOURCE_DIR}/plugins/npi/packet_decoder.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(${PROJECT_NAME} arrow rapidjson)
add_dependencies(${PROJECT_NAME} flowsql_common)
target_link_libraries(${PROJECT_NAME} flowsql_common)

set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
