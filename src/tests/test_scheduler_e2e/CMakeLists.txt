project(test_scheduler_e2e)

add_executable(${PROJECT_NAME} test_scheduler_e2e.cpp)

add_executable(test_scheduler_mutation_guard
    test_scheduler_mutation_guard.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/scheduler_plugin.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/scheduler_internal_utils.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/scheduler_channel_admin.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/scheduler_batch_runtime.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/scheduler_runtime_retention.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/scheduler_runtime_graph.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/scheduler_stream_executor.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/scheduler_stream_shared_source.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/scheduler_routes.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/scheduler_json_codec.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/scheduler_stream_group.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/scheduler_stream_group_request.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/scheduler_stream_group_planner.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/scheduler_stream_group_runtime.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/stream_execution_plan.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/shared_source_hub.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/stream_runtime.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/stream_task.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/stream_task_group.cpp
    ${CMAKE_SOURCE_DIR}/framework/core/pipeline.cpp
    ${CMAKE_SOURCE_DIR}/framework/core/channel_adapter.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(test_scheduler_mutation_guard PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow rapidjson)
add_thirddepen(test_scheduler_mutation_guard arrow httplib rapidjson)

add_dependencies(${PROJECT_NAME}
    flowsql_common
    flowsql_catalog
    flowsql_scheduler
    flowsql_database
    flowsql_pcapfile
    flowsql_stream
    flowsql_web
)
add_dependencies(test_scheduler_mutation_guard
    flowsql_common
)
target_link_libraries(${PROJECT_NAME} flowsql_common)
target_link_libraries(test_scheduler_mutation_guard flowsql_common)

target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)
target_compile_options(test_scheduler_mutation_guard PRIVATE -UNDEBUG)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
set_target_properties(test_scheduler_mutation_guard PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

add_test(NAME test_scheduler_e2e COMMAND ${CMAKE_BINARY_DIR}/output/test_scheduler_e2e)
add_test(NAME test_scheduler_mutation_guard COMMAND ${CMAKE_BINARY_DIR}/output/test_scheduler_mutation_guard)

add_executable(test_pcap_web_e2e test_pcap_web_e2e.cpp)
target_include_directories(test_pcap_web_e2e PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(test_pcap_web_e2e arrow httplib rapidjson)
add_dependencies(test_pcap_web_e2e
    flowsql_common
    flowsql_builtin
    flowsql_catalog
    flowsql_npi
    flowsql_pcapfile
    flowsql_scheduler
    flowsql_web
)
target_link_libraries(test_pcap_web_e2e flowsql_common)
target_compile_definitions(test_pcap_web_e2e PRIVATE
    FLOWSQL_NPI_PROTOCOLS_PATH="${CMAKE_SOURCE_DIR}/plugins/npi/conf/protocols.yml"
    FLOWSQL_PCAP_FIXTURE_PATH="${CMAKE_SOURCE_DIR}/tests/data/packets/ArpRequestPacket.pcap"
    FLOWSQL_PCAPNG_FIXTURE_PATH="${CMAKE_SOURCE_DIR}/tests/data/packets/igmp.pcapng"
)
target_compile_options(test_pcap_web_e2e PRIVATE -UNDEBUG)
set_target_properties(test_pcap_web_e2e PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
add_test(NAME test_pcap_web_e2e COMMAND ${CMAKE_BINARY_DIR}/output/test_pcap_web_e2e)
