project(test_npm_basic)

find_package(Threads REQUIRED)

add_executable(${PROJECT_NAME} test_npm_basic.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_basic_operator.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_basic_result_collector.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_basic_result_encoder.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_basic_result_projector.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_basic_task_config.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_basic_task_runtime.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_eof_flusher.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_packet_batch_view.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_packet_processor.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_context.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_session_key.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_session_analysis_module.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_session_result_encoder.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_session_table.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_task_budget.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_tcp_performance_tracker.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npi/packet_decoder.cpp)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    FLOWSQL_NPI_PLUGIN_PATH="$<TARGET_FILE:flowsql_npi>"
    FLOWSQL_NPM_BASIC_PLUGIN_PATH="$<TARGET_FILE:flowsql_npm_basic>"
    FLOWSQL_NPI_PROTOCOLS_PATH="${CMAKE_SOURCE_DIR}/plugins/npi/conf/protocols.yml"
)

add_dependencies(${PROJECT_NAME} flowsql_common flowsql_npi flowsql_npm_basic)
target_link_libraries(${PROJECT_NAME} flowsql_common flowsql_npi Threads::Threads)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

add_test(NAME test_npm_basic COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_basic)

# Machine-dependent throughput is an observation, not a CTest pass/fail threshold.
add_executable(benchmark_npm_basic
    benchmark_npm_basic.cpp
    ${CMAKE_SOURCE_DIR}/plugins/npi/packet_decoder.cpp
)
target_include_directories(benchmark_npm_basic PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(benchmark_npm_basic arrow)
target_compile_definitions(benchmark_npm_basic PRIVATE
    FLOWSQL_NPI_PLUGIN_PATH="$<TARGET_FILE:flowsql_npi>"
    FLOWSQL_NPM_BASIC_PLUGIN_PATH="$<TARGET_FILE:flowsql_npm_basic>"
    FLOWSQL_NPI_PROTOCOLS_PATH="${CMAKE_SOURCE_DIR}/plugins/npi/conf/protocols.yml"
)
add_dependencies(benchmark_npm_basic flowsql_common flowsql_npi flowsql_npm_basic)
target_link_libraries(benchmark_npm_basic flowsql_common)
set_target_properties(benchmark_npm_basic PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
