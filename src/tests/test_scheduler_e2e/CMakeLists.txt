project(test_scheduler_e2e)

add_executable(${PROJECT_NAME} test_scheduler_e2e.cpp)
target_sources(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_family_result_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_family_output_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/dns/npm_dns_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/icmp/npm_icmp_contract.cpp
)

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
    ${CMAKE_SOURCE_DIR}/services/scheduler/operator_contract_preflight.cpp
    ${CMAKE_SOURCE_DIR}/services/scheduler/scheduler_packet_sink.cpp
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
    flowsql_config_channel
    flowsql_builtin
    flowsql_catalog
    flowsql_binaddon
    flowsql_scheduler
    flowsql_database
    flowsql_npi
    flowsql_npm_basic
    flowsql_pcapfile
    flowsql_stream
    flowsql_web
)
add_dependencies(test_scheduler_mutation_guard
    flowsql_common
)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    FLOWSQL_NPI_PROTOCOLS_PATH="${CMAKE_SOURCE_DIR}/plugins/npi/conf/protocols.yml"
    FLOWSQL_NPM_BASIC_PLUGIN_PATH="$<TARGET_FILE:flowsql_npm_basic>"
)
target_link_libraries(${PROJECT_NAME} flowsql_common flowsql_npm_basic)
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

find_package(OpenSSL REQUIRED)
# New shared contract code uses the same SHA256 source identity as F2.
target_link_libraries(test_scheduler_mutation_guard OpenSSL::Crypto)
add_executable(test_operator_contract_execution test_operator_contract_execution.cpp)
target_include_directories(test_operator_contract_execution PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_operator_contract_execution PRIVATE -UNDEBUG)
add_thirddepen(test_operator_contract_execution arrow rapidjson)
target_link_libraries(test_operator_contract_execution flowsql_common OpenSSL::Crypto)
foreach(component IN ITEMS config_channel builtin catalog binaddon scheduler)
    string(TOUPPER ${component} upper)
    if(component STREQUAL "config_channel")
        set(upper CONFIG)
    endif()
    target_compile_definitions(test_operator_contract_execution PRIVATE ${upper}_PLUGIN_PATH="$<TARGET_FILE:flowsql_${component}>")
    add_dependencies(test_operator_contract_execution flowsql_${component})
endforeach()
target_compile_definitions(test_operator_contract_execution PRIVATE FIXTURE_EXECUTION_PATH="$<TARGET_FILE:fixture_contract_execution>")
add_dependencies(test_operator_contract_execution fixture_contract_execution)
add_test(NAME test_operator_contract_execution COMMAND $<TARGET_FILE:test_operator_contract_execution>)
set_tests_properties(test_operator_contract_execution PROPERTIES TIMEOUT 60)

add_executable(test_operator_contract_block test_operator_contract_block.cpp)
target_include_directories(test_operator_contract_block PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_operator_contract_block PRIVATE -UNDEBUG)
add_thirddepen(test_operator_contract_block arrow rapidjson)
target_link_libraries(test_operator_contract_block flowsql_common OpenSSL::Crypto)
foreach(component IN ITEMS config_channel builtin catalog binaddon scheduler database baseline baseliner)
    string(TOUPPER ${component} upper)
    if(component STREQUAL "config_channel")
        set(upper CONFIG)
    endif()
    target_compile_definitions(test_operator_contract_block PRIVATE ${upper}_PLUGIN_PATH="$<TARGET_FILE:flowsql_${component}>")
    add_dependencies(test_operator_contract_block flowsql_${component})
endforeach()
target_compile_definitions(test_operator_contract_block PRIVATE FIXTURE_BLOCK_PATH="$<TARGET_FILE:fixture_contract_block>")
add_dependencies(test_operator_contract_block fixture_contract_block)
add_test(NAME test_operator_contract_block COMMAND $<TARGET_FILE:test_operator_contract_block>)
set_tests_properties(test_operator_contract_block PROPERTIES TIMEOUT 60)

target_compile_definitions(test_operator_contract_block PRIVATE FLOWSQL_SOURCE_ROOT="${CMAKE_SOURCE_DIR}/..")

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

add_executable(test_netadapter_web_e2e test_netadapter_web_e2e.cpp)
target_include_directories(test_netadapter_web_e2e PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(test_netadapter_web_e2e arrow httplib rapidjson)
add_dependencies(test_netadapter_web_e2e flowsql_common flowsql_netadapter flowsql_builtin flowsql_scheduler flowsql_web)
target_link_libraries(test_netadapter_web_e2e flowsql_common)
target_compile_options(test_netadapter_web_e2e PRIVATE -UNDEBUG)
set_target_properties(test_netadapter_web_e2e PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_netadapter_web_e2e COMMAND ${CMAKE_BINARY_DIR}/output/test_netadapter_web_e2e)

add_executable(test_operator_contract_outputs test_operator_contract_outputs.cpp)
target_include_directories(test_operator_contract_outputs PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_operator_contract_outputs PRIVATE -UNDEBUG)
add_thirddepen(test_operator_contract_outputs arrow rapidjson sqlite)
target_link_libraries(test_operator_contract_outputs flowsql_common OpenSSL::Crypto)
foreach(component IN ITEMS config_channel builtin catalog binaddon scheduler)
    string(TOUPPER ${component} upper)
    if(component STREQUAL "config_channel")
        set(upper CONFIG)
    endif()
    target_compile_definitions(test_operator_contract_outputs PRIVATE ${upper}_PLUGIN_PATH="$<TARGET_FILE:flowsql_${component}>")
    add_dependencies(test_operator_contract_outputs flowsql_${component})
endforeach()
target_compile_definitions(test_operator_contract_outputs PRIVATE FIXTURE_OUTPUT_PATH="$<TARGET_FILE:fixture_contract_output>")
add_dependencies(test_operator_contract_outputs fixture_contract_output)
add_test(NAME test_operator_contract_outputs COMMAND $<TARGET_FILE:test_operator_contract_outputs>)
set_tests_properties(test_operator_contract_outputs PROPERTIES TIMEOUT 60)
