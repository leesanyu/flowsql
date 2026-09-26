project(test_npm_basic)

find_package(Threads REQUIRED)

add_executable(${PROJECT_NAME} test_npm_basic.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_contract.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_module_catalog.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_basic_operator.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_basic_result_consumer.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/config/npm_basic_task_config.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/config/npm_parameters.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_basic_task_runtime.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_eof_flusher.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_packet_batch_view.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_packet_processor.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_protocol_context.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_session_key.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_session_table.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_task_budget.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/basic/npm_basic_result_projector.cpp)
target_sources(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/session/npm_session_analysis_module.cpp)
target_sources(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/session/npm_tcp_performance_tracker.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/output/npm_result_router.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/output/npm_basic_result_collector.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/output/npm_basic_result_encoder.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/output/npm_session_result_encoder.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npi/packet_decoder.cpp)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    FLOWSQL_NPI_PLUGIN_PATH="$<TARGET_FILE:flowsql_npi>"
    FLOWSQL_NPM_BASIC_PLUGIN_PATH="$<TARGET_FILE:flowsql_npm_basic>"
    FLOWSQL_NPI_PROTOCOLS_PATH="${CMAKE_SOURCE_DIR}/plugins/npi/conf/protocols.yml"
)

add_dependencies(${PROJECT_NAME} flowsql_common flowsql_npi flowsql_npm_basic)
if(TARGET flowsql_flow_labeling)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        FLOWSQL_FLOW_LABELING_PLUGIN_PATH="$<TARGET_FILE:flowsql_flow_labeling>"
    )
    add_dependencies(${PROJECT_NAME} flowsql_flow_labeling)
endif()
target_link_libraries(${PROJECT_NAME} flowsql_common flowsql_npi Threads::Threads)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

add_test(NAME test_npm_basic COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_basic)

add_executable(test_npm_protocol_contract
    test_npm_protocol_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
)
target_include_directories(test_npm_protocol_contract PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_protocol_contract PRIVATE -UNDEBUG)
add_thirddepen(test_npm_protocol_contract arrow)
add_dependencies(test_npm_protocol_contract flowsql_npm_basic)
target_link_libraries(test_npm_protocol_contract flowsql_common)
set_target_properties(test_npm_protocol_contract PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
add_test(NAME test_npm_protocol_contract COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_protocol_contract)

add_executable(test_npm_result_sqlite test_npm_result_sqlite.cpp)
target_include_directories(test_npm_result_sqlite PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_result_sqlite PRIVATE -UNDEBUG)
target_sources(test_npm_result_sqlite PRIVATE
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_contract.cpp
)
add_thirddepen(test_npm_result_sqlite arrow rapidjson sqlite)
target_link_libraries(test_npm_result_sqlite flowsql_common flowsql_npm_basic flowsql_database)
set_target_properties(test_npm_result_sqlite PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
add_test(NAME test_npm_result_sqlite COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_result_sqlite)

add_executable(test_npm_result_backends test_npm_result_backends.cpp)
target_include_directories(test_npm_result_backends PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_result_backends PRIVATE -UNDEBUG)
target_sources(test_npm_result_backends PRIVATE
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_contract.cpp
)
add_thirddepen(test_npm_result_backends arrow rapidjson)
target_link_libraries(test_npm_result_backends flowsql_common flowsql_npm_basic flowsql_database Threads::Threads)
set_target_properties(test_npm_result_backends PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
add_test(NAME test_npm_result_backends COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_result_backends)

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
