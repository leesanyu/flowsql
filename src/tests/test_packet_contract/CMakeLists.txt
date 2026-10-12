project(test_packet_contract)

add_executable(test_packet_family_contract test_packet_family_contract.cpp
    ${CMAKE_SOURCE_DIR}/framework/core/packet_family_contract.cpp)
target_include_directories(test_packet_family_contract PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_packet_family_contract PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wno-pessimizing-move
    -Wno-unused-parameter)
add_thirddepen(test_packet_family_contract arrow)
set_target_properties(test_packet_family_contract PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_packet_family_contract COMMAND $<TARGET_FILE:test_packet_family_contract>)

add_executable(test_packet_family_runtime test_packet_family_runtime.cpp
    ${CMAKE_SOURCE_DIR}/plugins/npi/packet_decoder.cpp
    ${CMAKE_SOURCE_DIR}/operators/npm_basic/core/npm_packet_batch_view.cpp
    ${CMAKE_SOURCE_DIR}/framework/core/packet_family_codec.cpp
    ${CMAKE_SOURCE_DIR}/framework/core/packet_family_contract.cpp)
target_include_directories(test_packet_family_runtime PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_packet_family_runtime PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wno-unused-parameter
    -Wno-pessimizing-move)
add_thirddepen(test_packet_family_runtime arrow)
set_target_properties(test_packet_family_runtime PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
target_compile_definitions(test_packet_family_runtime PRIVATE FLOWSQL_NPI_PLUGIN_PATH="$<TARGET_FILE:flowsql_npi>")
add_dependencies(test_packet_family_runtime flowsql_npi)
target_link_libraries(test_packet_family_runtime flowsql_common)
target_sources(test_packet_family_runtime PRIVATE ${CMAKE_SOURCE_DIR}/channels/pcapfile/packet_filter_domain.cpp)
add_thirddepen(test_packet_family_runtime rapidjson)
add_test(NAME test_packet_family_runtime COMMAND $<TARGET_FILE:test_packet_family_runtime>)

option(FLOWSQL_IP_FAMILY_CONTRACT_SANITIZERS "Sanitize packet and NPM IP-family contract tests" OFF)
if(FLOWSQL_IP_FAMILY_CONTRACT_SANITIZERS)
    foreach(family_packet_target IN ITEMS test_packet_family_contract test_packet_family_runtime)
        target_compile_options(${family_packet_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${family_packet_target} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()

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
