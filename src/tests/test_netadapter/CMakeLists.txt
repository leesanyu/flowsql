add_executable(test_netadapter_contract test_netadapter_contract.cpp
    ${CMAKE_SOURCE_DIR}/channels/netadapter/netadapter_config.cpp)
target_include_directories(test_netadapter_contract PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_netadapter_contract PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_thirddepen(test_netadapter_contract arrow rapidjson)
target_link_libraries(test_netadapter_contract flowsql_common)
add_test(NAME test_netadapter_contract COMMAND test_netadapter_contract)
if(TARGET flowsql_netadapter)
    add_executable(test_netadapter_reader test_netadapter_reader.cpp
        ${CMAKE_SOURCE_DIR}/channels/netadapter/netadapter_config.cpp
        ${CMAKE_SOURCE_DIR}/channels/netadapter/netadapter_reader.cpp
        ${CMAKE_SOURCE_DIR}/channels/netadapter/netadapter_plugin.cpp
        ${CMAKE_SOURCE_DIR}/channels/pcapfile/packet_filter_domain.cpp
        ${CMAKE_SOURCE_DIR}/plugins/npi/packet_decoder.cpp)
    target_include_directories(test_netadapter_reader PRIVATE ${CMAKE_SOURCE_DIR})
    add_thirddepen(test_netadapter_reader arrow rapidjson sqlite)
    target_compile_options(test_netadapter_reader PRIVATE -UNDEBUG)
    target_link_libraries(test_netadapter_reader flowsql_common)
    add_test(NAME test_netadapter_reader COMMAND test_netadapter_reader)
endif()
if(TARGET flowsql_capture_af_packet)
    add_executable(test_af_packet_backend test_af_packet_backend.cpp
        ${CMAKE_SOURCE_DIR}/channels/netadapter/af_packet_backend.cpp)
    target_include_directories(test_af_packet_backend PRIVATE ${CMAKE_SOURCE_DIR})
    add_thirddepen(test_af_packet_backend arrow)
    target_compile_options(test_af_packet_backend PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    add_test(NAME test_af_packet_backend COMMAND test_af_packet_backend)
endif()
add_executable(test_pfring_backend test_pfring_backend.cpp
    ${CMAKE_SOURCE_DIR}/channels/netadapter/pfring_backend.cpp)
target_include_directories(test_pfring_backend PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(test_pfring_backend arrow)
target_compile_options(test_pfring_backend PRIVATE -UNDEBUG -Wall -Wextra -Werror)
if(FLOWSQL_PFRING_INCLUDE_DIR AND FLOWSQL_PFRING_LIBRARY)
    target_compile_definitions(test_pfring_backend PRIVATE FLOWSQL_HAVE_PFRING=1)
    target_include_directories(test_pfring_backend PRIVATE ${FLOWSQL_PFRING_INCLUDE_DIR})
    target_link_libraries(test_pfring_backend ${FLOWSQL_PFRING_LIBRARY})
endif()
add_test(NAME test_pfring_backend COMMAND test_pfring_backend)
add_executable(test_af_xdp_backend test_af_xdp_backend.cpp
    ${CMAKE_SOURCE_DIR}/channels/netadapter/af_xdp_backend.cpp)
target_include_directories(test_af_xdp_backend PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(test_af_xdp_backend arrow)
target_compile_options(test_af_xdp_backend PRIVATE -UNDEBUG -Wall -Wextra -Werror)
if(FLOWSQL_LIBXDP_FOUND AND FLOWSQL_LIBBPF_FOUND)
    target_compile_definitions(test_af_xdp_backend PRIVATE FLOWSQL_HAVE_AF_XDP=1)
    target_link_libraries(test_af_xdp_backend PkgConfig::FLOWSQL_LIBXDP PkgConfig::FLOWSQL_LIBBPF)
endif()
add_test(NAME test_af_xdp_backend COMMAND test_af_xdp_backend)

add_library(netadapter_validation_probe SHARED netadapter_validation_probe.cpp)
target_include_directories(netadapter_validation_probe PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(netadapter_validation_probe arrow rapidjson)
target_link_libraries(netadapter_validation_probe dl)
target_link_options(netadapter_validation_probe PRIVATE -Wl,--no-undefined)
target_compile_definitions(netadapter_validation_probe PRIVATE
    FLOWSQL_REAL_NPM_PLUGIN_PATH="$<TARGET_FILE:flowsql_npm_basic>")
add_dependencies(netadapter_validation_probe flowsql_npm_basic)
target_compile_options(netadapter_validation_probe PRIVATE -UNDEBUG -Wall -Wextra -Werror)

add_executable(netadapter_real_validation netadapter_real_validation.cpp)
target_include_directories(netadapter_real_validation PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(netadapter_real_validation arrow rapidjson)
target_link_libraries(netadapter_real_validation flowsql_common)
target_compile_options(netadapter_real_validation PRIVATE -UNDEBUG)
target_compile_definitions(netadapter_real_validation PRIVATE
    FLOWSQL_NPI_PROTOCOLS_PATH="${CMAKE_SOURCE_DIR}/plugins/npi/conf/protocols.yml"
    FLOWSQL_NPM_BASIC_PLUGIN_PATH="$<TARGET_FILE:flowsql_npm_basic>"
    FLOWSQL_VALIDATION_PROBE_PATH="$<TARGET_FILE:netadapter_validation_probe>")
add_dependencies(netadapter_real_validation flowsql_netadapter flowsql_capture_af_packet flowsql_capture_pfring
    flowsql_capture_af_xdp flowsql_database flowsql_builtin flowsql_catalog flowsql_npi flowsql_scheduler flowsql_binaddon flowsql_npm_basic netadapter_validation_probe)
