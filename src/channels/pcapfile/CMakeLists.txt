project(flowsql_pcapfile)

add_library(${PROJECT_NAME} SHARED
    packet_filter_domain.cpp
    pcap_file_channel.cpp
    pcap_file_channel_store.cpp
    plugin_register.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npi/packet_decoder.cpp)
add_thirddepen(${PROJECT_NAME} arrow rapidjson sqlite)
add_dependencies(${PROJECT_NAME} flowsql_common)
target_link_libraries(${PROJECT_NAME} flowsql_common)

set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
