project(test_builtin)

add_library(fixture_cpp_operator SHARED
    fixtures/fixture_cpp_operator.cpp
)
target_include_directories(fixture_cpp_operator PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(fixture_cpp_operator PRIVATE -fvisibility=hidden)
set_target_properties(fixture_cpp_operator PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

add_library(fixture_bad_abi SHARED
    fixtures/fixture_bad_abi.cpp
)
target_include_directories(fixture_bad_abi PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(fixture_bad_abi PRIVATE -fvisibility=hidden)
set_target_properties(fixture_bad_abi PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

add_library(fixture_cpp_operator_v2 SHARED
    fixtures/fixture_cpp_operator_v2.cpp
)
target_include_directories(fixture_cpp_operator_v2 PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(fixture_cpp_operator_v2 PRIVATE -fvisibility=hidden)
set_target_properties(fixture_cpp_operator_v2 PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

add_executable(${PROJECT_NAME}
    test_builtin.cpp
    ${CMAKE_SOURCE_DIR}/services/catalog/catalog_plugin.cpp
    ${CMAKE_SOURCE_DIR}/services/catalog/catalog_contract.cpp
    ${CMAKE_SOURCE_DIR}/services/catalog/catalog_dataset.cpp
    ${CMAKE_SOURCE_DIR}/services/catalog/lazy_dataframe_channel.cpp
    ${CMAKE_SOURCE_DIR}/services/catalog/dataframe_transfer.cpp
    ${CMAKE_SOURCE_DIR}/services/binaddon/binaddon_host_plugin.cpp
    ${CMAKE_SOURCE_DIR}/framework/builtin/dataframe/concat_operator.cpp
    ${CMAKE_SOURCE_DIR}/framework/builtin/dataframe/hstack_operator.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow rapidjson sqlite)
find_package(OpenSSL REQUIRED)
target_link_libraries(${PROJECT_NAME} OpenSSL::Crypto)

add_dependencies(${PROJECT_NAME} flowsql_common)
add_dependencies(${PROJECT_NAME} fixture_cpp_operator fixture_bad_abi fixture_cpp_operator_v2)
target_link_libraries(${PROJECT_NAME} flowsql_common)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    FIXTURE_CPP_SO_PATH="$<TARGET_FILE:fixture_cpp_operator>"
    FIXTURE_BAD_ABI_SO_PATH="$<TARGET_FILE:fixture_bad_abi>"
    FIXTURE_CPP_V2_SO_PATH="$<TARGET_FILE:fixture_cpp_operator_v2>"
)

target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

add_test(NAME test_builtin COMMAND ${CMAKE_BINARY_DIR}/output/test_builtin)

add_executable(test_operator_contract_registration test_operator_contract_registration.cpp)
target_include_directories(test_operator_contract_registration PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(test_operator_contract_registration rapidjson)
target_link_libraries(test_operator_contract_registration OpenSSL::Crypto ${CMAKE_DL_LIBS})
target_compile_options(test_operator_contract_registration PRIVATE -UNDEBUG)
target_compile_definitions(test_operator_contract_registration PRIVATE
    FLOWSQL_SOURCE_ROOT="${CMAKE_SOURCE_DIR}/.."
    BUILTIN_PLUGIN_PATH="$<TARGET_FILE:flowsql_builtin>"
    CONFIG_PLUGIN_PATH="$<TARGET_FILE:flowsql_config_channel>"
    CATALOG_PLUGIN_PATH="$<TARGET_FILE:flowsql_catalog>")
add_dependencies(test_operator_contract_registration flowsql_config_channel flowsql_catalog flowsql_builtin)
add_test(NAME test_operator_contract_registration COMMAND $<TARGET_FILE:test_operator_contract_registration>)
set_tests_properties(test_operator_contract_registration PROPERTIES TIMEOUT 60)

foreach(variant IN ITEMS v1 v2 reference invalid too_long throw)
    add_library(fixture_contract_${variant} SHARED fixtures/fixture_contract_operator.cpp)
    target_include_directories(fixture_contract_${variant} PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_options(fixture_contract_${variant} PRIVATE -fvisibility=hidden)
    set_target_properties(fixture_contract_${variant} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
    if(variant STREQUAL "v2")
        target_compile_definitions(fixture_contract_${variant} PRIVATE FIXTURE_ABI=2 FIXTURE_NAME="v2")
    elseif(variant STREQUAL "reference")
        target_compile_definitions(fixture_contract_${variant} PRIVATE FIXTURE_NAME="reference")
    else()
        target_compile_definitions(fixture_contract_${variant} PRIVATE FIXTURE_NAME="v1")
    endif()
    string(TOUPPER ${variant} upper)
    target_compile_definitions(fixture_contract_${variant} PRIVATE FIXTURE_${upper}=1)
endforeach()
add_executable(test_operator_contract_cpp test_operator_contract_cpp.cpp)
target_include_directories(test_operator_contract_cpp PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(test_operator_contract_cpp rapidjson)
target_compile_options(test_operator_contract_cpp PRIVATE -UNDEBUG)
target_link_libraries(test_operator_contract_cpp OpenSSL::Crypto ${CMAKE_DL_LIBS})
foreach(component IN ITEMS config_channel builtin catalog binaddon baseliner)
    string(TOUPPER ${component} upper)
    if(component STREQUAL "config_channel")
        set(upper CONFIG)
    endif()
    target_compile_definitions(test_operator_contract_cpp PRIVATE ${upper}_PLUGIN_PATH="$<TARGET_FILE:flowsql_${component}>")
    add_dependencies(test_operator_contract_cpp flowsql_${component})
endforeach()
foreach(variant IN ITEMS v1 v2 reference invalid too_long throw)
    string(TOUPPER ${variant} upper)
    target_compile_definitions(test_operator_contract_cpp PRIVATE FIXTURE_${upper}_PATH="$<TARGET_FILE:fixture_contract_${variant}>")
    add_dependencies(test_operator_contract_cpp fixture_contract_${variant})
endforeach()
add_test(NAME test_operator_contract_cpp COMMAND $<TARGET_FILE:test_operator_contract_cpp>)
set_tests_properties(test_operator_contract_cpp PROPERTIES TIMEOUT 90)

target_compile_definitions(test_operator_contract_cpp PRIVATE FIXTURE_PAIR_PATH="$<TARGET_FILE:fixture_cpp_operator_v2>")
add_dependencies(test_operator_contract_cpp fixture_cpp_operator_v2)

add_library(fixture_contract_execution SHARED fixtures/fixture_contract_operator.cpp)
target_include_directories(fixture_contract_execution PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(fixture_contract_execution PRIVATE FIXTURE_EXECUTION=1 FIXTURE_NAME="execution")
add_thirddepen(fixture_contract_execution arrow)
target_link_libraries(fixture_contract_execution flowsql_common)
set_target_properties(fixture_contract_execution PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)

add_library(fixture_contract_block SHARED fixtures/fixture_contract_block.cpp)
target_include_directories(fixture_contract_block PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(fixture_contract_block arrow rapidjson)
target_link_libraries(fixture_contract_block flowsql_common)
set_target_properties(fixture_contract_block PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)

add_library(fixture_contract_output SHARED fixtures/fixture_contract_output.cpp)
target_include_directories(fixture_contract_output PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(fixture_contract_output arrow rapidjson)
target_link_libraries(fixture_contract_output flowsql_common OpenSSL::Crypto)
set_target_properties(fixture_contract_output PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
