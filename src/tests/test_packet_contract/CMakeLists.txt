project(test_packet_contract)

add_executable(test_wire_layout test_wire_layout.cpp)
target_include_directories(test_wire_layout PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_wire_layout PRIVATE -UNDEBUG)
set_target_properties(test_wire_layout PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_wire_layout COMMAND $<TARGET_FILE:test_wire_layout>)

add_executable(${PROJECT_NAME} test_packet_contract.cpp)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npi/packet_decoder.cpp)

add_dependencies(${PROJECT_NAME} flowsql_common)
target_link_libraries(${PROJECT_NAME} flowsql_common)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

add_test(NAME test_packet_contract COMMAND ${CMAKE_BINARY_DIR}/output/test_packet_contract)

add_executable(test_npi_alignment test_npi_alignment.cpp ${CMAKE_SOURCE_DIR}/plugins/npi/packet_decoder.cpp)
target_include_directories(test_npi_alignment PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_npi_alignment PRIVATE flowsql_common ${CMAKE_DL_LIBS})
target_compile_options(test_npi_alignment PRIVATE -UNDEBUG)
add_dependencies(test_npi_alignment flowsql_npi)
set_target_properties(test_npi_alignment PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_npi_alignment COMMAND $<TARGET_FILE:test_npi_alignment> $<TARGET_FILE:flowsql_npi>
    ${CMAKE_SOURCE_DIR}/plugins/npi/conf/protocols.yml)
set_tests_properties(test_npi_alignment PROPERTIES LABELS "npi;alignment;packet" TIMEOUT 90)
