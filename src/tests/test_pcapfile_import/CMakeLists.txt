project(test_pcapfile_import)

add_executable(${PROJECT_NAME}
    test_pcapfile_import.cpp
    ${CMAKE_SOURCE_DIR}/channels/pcapfile/packet_filter_domain.cpp
    ${CMAKE_SOURCE_DIR}/channels/pcapfile/pcap_file_channel.cpp
    ${CMAKE_SOURCE_DIR}/channels/pcapfile/pcap_file_channel_store.cpp
    ${CMAKE_SOURCE_DIR}/plugins/npi/packet_decoder.cpp
)
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)
add_thirddepen(${PROJECT_NAME} arrow rapidjson sqlite)
add_dependencies(${PROJECT_NAME} flowsql_common flowsql_npi flowsql_pcapfile)
target_link_libraries(${PROJECT_NAME} flowsql_common)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    FLOWSQL_NPI_PLUGIN_PATH=\"$<TARGET_FILE:flowsql_npi>\"
    FLOWSQL_PCAPFILE_PLUGIN_PATH=\"$<TARGET_FILE:flowsql_pcapfile>\"
    FLOWSQL_NPI_PROTOCOLS_PATH=\"${CMAKE_SOURCE_DIR}/plugins/npi/conf/protocols.yml\"
)
set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_pcapfile_import COMMAND ${CMAKE_BINARY_DIR}/output/test_pcapfile_import)
