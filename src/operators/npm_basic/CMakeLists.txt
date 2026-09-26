project(flowsql_npm_basic)

add_library(${PROJECT_NAME} SHARED
    npm_analysis_contract.cpp
    npm_protocol_contract.cpp
    core/npm_module_catalog.cpp
    npm_basic_operator.cpp
    npm_basic_result_consumer.cpp
    config/npm_basic_task_config.cpp
    config/npm_parameters.cpp
    core/npm_basic_task_runtime.cpp
    core/npm_eof_flusher.cpp
    core/npm_packet_batch_view.cpp
    core/npm_packet_processor.cpp
    core/npm_protocol_context.cpp
    core/npm_session_key.cpp
    core/npm_session_table.cpp
    core/npm_task_budget.cpp
    modules/basic/npm_basic_result_projector.cpp
    modules/session/npm_session_analysis_module.cpp
    modules/session/npm_tcp_performance_tracker.cpp
    output/npm_result_router.cpp
    output/npm_basic_result_collector.cpp
    output/npm_basic_result_encoder.cpp
    output/npm_session_result_encoder.cpp
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
