project(test_npm_basic)

find_package(Threads REQUIRED)

add_executable(${PROJECT_NAME} test_npm_basic.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npm_basic/npm_analysis_contract.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npm_basic/npm_basic_result_collector.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npm_basic/npm_basic_result_encoder.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npm_basic/npm_basic_result_projector.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npm_basic/npm_basic_task_config.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npm_basic/npm_basic_task_runtime.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npm_basic/npm_eof_flusher.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npm_basic/npm_packet_batch_view.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npm_basic/npm_packet_processor.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npm_basic/npm_protocol_context.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npm_basic/npm_session_key.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npm_basic/npm_session_table.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npm_basic/npm_task_budget.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npi/packet_decoder.cpp)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    FLOWSQL_NPI_PROTOCOLS_PATH="${CMAKE_SOURCE_DIR}/plugins/npi/conf/protocols.yml"
)

add_dependencies(${PROJECT_NAME} flowsql_common flowsql_npi)
target_link_libraries(${PROJECT_NAME} flowsql_common flowsql_npi Threads::Threads)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

add_test(NAME test_npm_basic COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_basic)
