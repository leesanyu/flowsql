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
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_tcp_stream_direction.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_tcp_stream_shared.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_tcp_stream_provider.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/basic/npm_basic_result_projector.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/basic/npm_basic_periodic_stats.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/dns/npm_dns_contract.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/icmp/npm_icmp_contract.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/icmp/npm_icmp_parser.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/icmp/npm_icmp_echo.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/icmp/npm_icmp_result_encoder.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/icmp/npm_icmp_module.cpp)
target_sources(${PROJECT_NAME} PRIVATE test_npm_icmp_module.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/dns/npm_dns_module.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/dns/npm_dns_result_encoder.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/dns/npm_dns_tcp.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/dns/npm_dns_udp.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_contract.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_framer.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_transactions.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_result_encoder.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_module.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_contract.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_hello.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_framer.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_handshake.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_result_encoder.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_module.cpp)
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
add_test(NAME test_npm_periodic_runtime COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_basic --periodic-only)
add_test(NAME test_npm_tcp_stream_runtime COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_basic --tcp-stream-only)

add_executable(test_npm_protocol_contract
    test_npm_protocol_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/dns/npm_dns_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
)
target_include_directories(test_npm_protocol_contract PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_protocol_contract PRIVATE -UNDEBUG)
add_thirddepen(test_npm_protocol_contract arrow rapidjson)
add_dependencies(test_npm_protocol_contract flowsql_npm_basic)
target_link_libraries(test_npm_protocol_contract flowsql_common)
set_target_properties(test_npm_protocol_contract PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
add_test(NAME test_npm_protocol_contract COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_protocol_contract)

add_executable(test_npm_http1_contract
    test_npm_http1_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
)
target_include_directories(test_npm_http1_contract PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_http1_contract PRIVATE -UNDEBUG)
add_thirddepen(test_npm_http1_contract arrow rapidjson)
add_dependencies(test_npm_http1_contract flowsql_npm_basic)
target_link_libraries(test_npm_http1_contract flowsql_common)
set_target_properties(test_npm_http1_contract PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_http1_contract COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_http1_contract)

add_executable(test_npm_tls_contract
    test_npm_tls_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
)
target_include_directories(test_npm_tls_contract PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_tls_contract PRIVATE -UNDEBUG)
add_thirddepen(test_npm_tls_contract arrow rapidjson)
target_link_libraries(test_npm_tls_contract flowsql_common)
set_target_properties(test_npm_tls_contract PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_tls_contract COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_tls_contract)

add_executable(test_npm_icmp_contract
    test_npm_icmp_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/icmp/npm_icmp_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
)
target_include_directories(test_npm_icmp_contract PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_icmp_contract PRIVATE -UNDEBUG)
add_thirddepen(test_npm_icmp_contract arrow rapidjson)
target_link_libraries(test_npm_icmp_contract flowsql_common)
set_target_properties(test_npm_icmp_contract PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_icmp_contract COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_icmp_contract)

add_executable(test_npm_icmp_parser
    test_npm_icmp_parser.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/icmp/npm_icmp_parser.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
)
target_include_directories(test_npm_icmp_parser PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_icmp_parser PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wno-pessimizing-move
    -Wno-unused-parameter)
add_thirddepen(test_npm_icmp_parser arrow)
target_link_libraries(test_npm_icmp_parser flowsql_common)
set_target_properties(test_npm_icmp_parser PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_icmp_parser COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_icmp_parser)

add_executable(test_npm_icmp_echo
    test_npm_icmp_echo.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/icmp/npm_icmp_echo.cpp
)
target_include_directories(test_npm_icmp_echo PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_icmp_echo PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wno-pessimizing-move)
target_link_libraries(test_npm_icmp_echo flowsql_common)
set_target_properties(test_npm_icmp_echo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_icmp_echo COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_icmp_echo)

option(FLOWSQL_NPM_ICMP_SANITIZERS "Sanitize NPM ICMP parsing, echo tracking, and runtime tests" OFF)
if(FLOWSQL_NPM_ICMP_SANITIZERS)
    foreach(icmp_target IN ITEMS test_npm_icmp_parser test_npm_icmp_echo test_npm_basic)
        target_compile_options(${icmp_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${icmp_target} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()

add_executable(test_npm_tls_hello
    test_npm_tls_hello.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_hello.cpp
)
target_include_directories(test_npm_tls_hello PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_tls_hello PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wno-pessimizing-move)
set_target_properties(test_npm_tls_hello PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_tls_hello COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_tls_hello)

add_executable(test_npm_tls_framer
    test_npm_tls_framer.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_framer.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_hello.cpp
)
target_include_directories(test_npm_tls_framer PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_tls_framer PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wno-pessimizing-move)
set_target_properties(test_npm_tls_framer PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_tls_framer COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_tls_framer)

add_executable(test_npm_tls_handshake
    test_npm_tls_handshake.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_handshake.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_framer.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_hello.cpp
)
target_include_directories(test_npm_tls_handshake PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_tls_handshake PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wno-pessimizing-move)
set_target_properties(test_npm_tls_handshake PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_tls_handshake COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_tls_handshake)

add_executable(test_npm_tls_result_encoder
    test_npm_tls_result_encoder.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_result_encoder.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_handshake.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_framer.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_hello.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
)
target_include_directories(test_npm_tls_result_encoder PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_tls_result_encoder PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wno-pessimizing-move -Wno-unused-parameter)
add_thirddepen(test_npm_tls_result_encoder arrow rapidjson)
target_link_libraries(test_npm_tls_result_encoder flowsql_common)
set_target_properties(test_npm_tls_result_encoder PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_tls_result_encoder COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_tls_result_encoder)

add_executable(test_npm_tls_module
    test_npm_tls_module.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_module.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_result_encoder.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_handshake.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_framer.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_hello.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/tls/npm_tls_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
)
target_include_directories(test_npm_tls_module PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_tls_module PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wno-pessimizing-move -Wno-unused-parameter)
add_thirddepen(test_npm_tls_module arrow rapidjson)
target_link_libraries(test_npm_tls_module flowsql_common)
set_target_properties(test_npm_tls_module PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_tls_module COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_tls_module)

option(FLOWSQL_NPM_TLS_SANITIZERS "Sanitize isolated NPM TLS tests" OFF)
if(FLOWSQL_NPM_TLS_SANITIZERS)
    foreach(tls_target IN ITEMS test_npm_tls_framer test_npm_tls_handshake
                                test_npm_tls_result_encoder test_npm_tls_module)
        target_compile_options(${tls_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${tls_target} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()

add_executable(test_npm_http1_framer
    test_npm_http1_framer.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_framer.cpp
)
target_include_directories(test_npm_http1_framer PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_http1_framer PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wno-pessimizing-move)
set_target_properties(test_npm_http1_framer PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_http1_framer COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_http1_framer)

add_executable(test_npm_http1_transactions
    test_npm_http1_transactions.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_transactions.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_framer.cpp
)
target_include_directories(test_npm_http1_transactions PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_http1_transactions PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wno-pessimizing-move)
set_target_properties(test_npm_http1_transactions PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_http1_transactions COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_http1_transactions)

add_executable(test_npm_http1_result_encoder
    test_npm_http1_result_encoder.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_result_encoder.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_transactions.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_framer.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
)
target_include_directories(test_npm_http1_result_encoder PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_http1_result_encoder PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wno-pessimizing-move -Wno-unused-parameter)
add_thirddepen(test_npm_http1_result_encoder arrow rapidjson)
target_link_libraries(test_npm_http1_result_encoder flowsql_common)
set_target_properties(test_npm_http1_result_encoder PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_http1_result_encoder COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_http1_result_encoder)

add_executable(test_npm_http1_module
    test_npm_http1_module.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_module.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_result_encoder.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_transactions.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_framer.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
)
target_include_directories(test_npm_http1_module PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_http1_module PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wno-pessimizing-move -Wno-unused-parameter)
add_thirddepen(test_npm_http1_module arrow rapidjson)
target_link_libraries(test_npm_http1_module flowsql_common)
set_target_properties(test_npm_http1_module PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_http1_module COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_http1_module)

option(FLOWSQL_NPM_HTTP1_SANITIZERS "Sanitize isolated NPM HTTP/1 tests" OFF)
if(FLOWSQL_NPM_HTTP1_SANITIZERS)
    foreach(http1_target IN ITEMS test_npm_http1_framer test_npm_http1_transactions
                                  test_npm_http1_result_encoder test_npm_http1_module)
        target_compile_options(${http1_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${http1_target} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()

add_executable(test_npm_dns_udp
    test_npm_dns_udp.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/dns/npm_dns_udp.cpp
)
target_include_directories(test_npm_dns_udp PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_dns_udp PRIVATE -UNDEBUG -Wall -Wextra -Wno-pessimizing-move)
add_dependencies(test_npm_dns_udp flowsql_npm_basic)
target_link_libraries(test_npm_dns_udp flowsql_npm_basic)
set_target_properties(test_npm_dns_udp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_dns_udp COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_dns_udp)

add_executable(test_npm_dns_tcp
    test_npm_dns_tcp.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_task_budget.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_tcp_stream_direction.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_tcp_stream_shared.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/dns/npm_dns_tcp.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/dns/npm_dns_udp.cpp
)
target_include_directories(test_npm_dns_tcp PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_dns_tcp PRIVATE -UNDEBUG -Wall -Wextra -Wno-pessimizing-move)
add_dependencies(test_npm_dns_tcp flowsql_npm_basic)
target_link_libraries(test_npm_dns_tcp flowsql_npm_basic)
set_target_properties(test_npm_dns_tcp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_dns_tcp COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_dns_tcp)

add_executable(test_npm_dns_result_encoder
    test_npm_dns_result_encoder.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/dns/npm_dns_result_encoder.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/dns/npm_dns_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
)
target_include_directories(test_npm_dns_result_encoder PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_dns_result_encoder PRIVATE -UNDEBUG -Wall -Wextra -Wno-pessimizing-move)
add_thirddepen(test_npm_dns_result_encoder arrow)
target_link_libraries(test_npm_dns_result_encoder flowsql_npm_basic flowsql_common)
set_target_properties(test_npm_dns_result_encoder PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_dns_result_encoder COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_dns_result_encoder)

add_executable(test_npm_dns_module
    test_npm_dns_module.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/dns/npm_dns_module.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/dns/npm_dns_result_encoder.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/dns/npm_dns_tcp.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/dns/npm_dns_udp.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/dns/npm_dns_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
)
target_include_directories(test_npm_dns_module PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_dns_module PRIVATE -UNDEBUG -Wall -Wextra -Wno-pessimizing-move)
add_thirddepen(test_npm_dns_module arrow)
target_link_libraries(test_npm_dns_module flowsql_npm_basic flowsql_common)
set_target_properties(test_npm_dns_module PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_dns_module COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_dns_module)

option(FLOWSQL_NPM_DNS_SANITIZERS "Sanitize the isolated NPM DNS tests" OFF)
if(FLOWSQL_NPM_DNS_SANITIZERS)
    foreach(dns_target IN ITEMS test_npm_dns_udp test_npm_dns_tcp test_npm_dns_result_encoder test_npm_dns_module)
        target_compile_options(${dns_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${dns_target} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()

add_executable(test_npm_tcp_stream
    test_npm_tcp_stream.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_tcp_stream_direction.cpp
)
target_include_directories(test_npm_tcp_stream PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_tcp_stream PRIVATE -UNDEBUG -Wall -Wextra -Wno-pessimizing-move)
option(FLOWSQL_NPM_TCP_STREAM_SANITIZERS "Sanitize the isolated NPM TCP stream test" OFF)
if(FLOWSQL_NPM_TCP_STREAM_SANITIZERS)
    target_compile_options(test_npm_basic PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(test_npm_basic PRIVATE -fsanitize=address,undefined)
    target_compile_options(test_npm_tcp_stream PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_libraries(test_npm_tcp_stream PRIVATE -fsanitize=address,undefined)
endif()
set_target_properties(test_npm_tcp_stream PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_tcp_stream COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_tcp_stream)

add_executable(test_npm_tcp_stream_shared
    test_npm_tcp_stream_shared.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_tcp_stream_direction.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_tcp_stream_shared.cpp
)
target_include_directories(test_npm_tcp_stream_shared PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_tcp_stream_shared PRIVATE -UNDEBUG -Wall -Wextra -Wno-pessimizing-move)
if(FLOWSQL_NPM_TCP_STREAM_SANITIZERS)
    target_compile_options(test_npm_tcp_stream_shared PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_libraries(test_npm_tcp_stream_shared PRIVATE -fsanitize=address,undefined)
endif()
set_target_properties(test_npm_tcp_stream_shared PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_tcp_stream_shared COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_tcp_stream_shared)

add_executable(test_npm_result_sqlite test_npm_result_sqlite.cpp)
target_include_directories(test_npm_result_sqlite PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_result_sqlite PRIVATE -UNDEBUG)
target_sources(test_npm_result_sqlite PRIVATE
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/output/npm_basic_result_encoder.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/dns/npm_dns_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/http1/npm_http1_contract.cpp
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
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/output/npm_basic_result_encoder.cpp
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

add_executable(test_npm_periodic_contract
    test_npm_periodic_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_protocol_contract.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/output/npm_basic_result_encoder.cpp
)
target_include_directories(test_npm_periodic_contract PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_periodic_contract PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wno-pessimizing-move -Wno-unused-parameter)
add_thirddepen(test_npm_periodic_contract arrow)
target_link_libraries(test_npm_periodic_contract flowsql_common)
set_target_properties(test_npm_periodic_contract PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_periodic_contract COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_periodic_contract)

add_executable(test_npm_periodic_stats test_npm_periodic_stats.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/basic/npm_basic_periodic_stats.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/modules/basic/npm_basic_result_projector.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_protocol_context.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_session_table.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_session_key.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_task_budget.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/npm_analysis_contract.cpp
    ${CMAKE_SOURCE_DIR}/plugins/npi/packet_decoder.cpp
)
target_include_directories(test_npm_periodic_stats PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_npm_periodic_stats PRIVATE -UNDEBUG)
add_thirddepen(test_npm_periodic_stats arrow)
target_link_libraries(test_npm_periodic_stats flowsql_common)
set_target_properties(test_npm_periodic_stats PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npm_periodic_stats COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_periodic_stats)

option(FLOWSQL_NPM_PERIODIC_SANITIZERS "Sanitize Basic periodic contract, statistics, and runtime tests" OFF)
if(FLOWSQL_NPM_PERIODIC_SANITIZERS)
    foreach(periodic_target IN ITEMS test_npm_periodic_contract test_npm_periodic_stats test_npm_basic)
        target_compile_options(${periodic_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${periodic_target} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()
