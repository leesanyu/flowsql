project(test_config_channel)

add_executable(${PROJECT_NAME}
    test_config_channel.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/operator_contract_service.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/operator_contract_derivation.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/channel_dictionary.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_control.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/operator_data_contract_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_registry.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/config_channel_provider.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/config_channel_control.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/config_content_validator.cpp
)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/channels/config)
target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)
add_thirddepen(${PROJECT_NAME} rapidjson sqlite yaml-cpp httplib)
find_package(OpenSSL REQUIRED)
target_link_libraries(${PROJECT_NAME} OpenSSL::Crypto ${CMAKE_DL_LIBS})
add_dependencies(${PROJECT_NAME} flowsql_config_channel)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    FLOWSQL_CONFIG_CHANNEL_PLUGIN_PATH="$<TARGET_FILE:flowsql_config_channel>"
)
set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_config_channel COMMAND ${CMAKE_BINARY_DIR}/output/test_config_channel)

add_executable(test_config_channel_e2e test_config_channel_e2e.cpp)
target_include_directories(test_config_channel_e2e PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_config_channel_e2e PRIVATE -UNDEBUG)
add_thirddepen(test_config_channel_e2e rapidjson httplib)
target_link_libraries(test_config_channel_e2e OpenSSL::Crypto ${CMAKE_DL_LIBS})
add_dependencies(test_config_channel_e2e flowsql_config_channel flowsql_web flowsql_gateway flowsql_router)
if(TARGET flowsql_flow_labeling)
    add_thirddepen(test_config_channel_e2e arrow)
    target_compile_definitions(test_config_channel_e2e PRIVATE
        FLOWSQL_CONFIG_LABELING_E2E=1
        FLOWSQL_NPM_BASIC_PLUGIN_PATH="$<TARGET_FILE:flowsql_npm_basic>"
        FLOWSQL_NPI_PROTOCOLS_PATH="${CMAKE_SOURCE_DIR}/plugins/npi/conf/protocols.yml"
    )
    add_dependencies(test_config_channel_e2e flowsql_common flowsql_npi flowsql_npm_basic flowsql_flow_labeling)
    target_link_libraries(test_config_channel_e2e flowsql_common)
endif()
set_target_properties(test_config_channel_e2e PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_config_channel_e2e COMMAND ${CMAKE_BINARY_DIR}/output/test_config_channel_e2e)
set_tests_properties(test_config_channel_e2e PROPERTIES TIMEOUT 60)

add_executable(test_data_semantics test_data_semantics.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/operator_data_contract_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_registry.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/config_content_validator.cpp)
target_include_directories(test_data_semantics PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/channels/config)
target_compile_options(test_data_semantics PRIVATE -UNDEBUG)
add_thirddepen(test_data_semantics rapidjson sqlite yaml-cpp)
target_link_libraries(test_data_semantics OpenSSL::Crypto ${CMAKE_DL_LIBS})
target_compile_definitions(test_data_semantics PRIVATE FLOWSQL_SOURCE_ROOT="${CMAKE_SOURCE_DIR}/..")
add_test(NAME test_data_semantics COMMAND $<TARGET_FILE:test_data_semantics>)

add_executable(test_data_semantics_host semantics_host.cpp)
target_include_directories(test_data_semantics_host PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_data_semantics_host PRIVATE -UNDEBUG)
add_thirddepen(test_data_semantics_host rapidjson)
target_link_libraries(test_data_semantics_host ${CMAKE_DL_LIBS})
add_dependencies(test_data_semantics_host flowsql_config_channel flowsql_web flowsql_gateway flowsql_router)
add_test(NAME test_data_semantics_api COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_data_semantics_api.py $<TARGET_FILE:test_data_semantics_host>)
set_tests_properties(test_data_semantics_api PROPERTIES TIMEOUT 180 RESOURCE_LOCK semantics_http)

find_program(FLOWSQL_SEMANTICS_NODE NAMES node nodejs REQUIRED)
add_test(NAME test_data_semantics_browser COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_data_semantics_browser.py $<TARGET_FILE:test_data_semantics_host> ${FLOWSQL_SEMANTICS_NODE})
set_tests_properties(test_data_semantics_browser PROPERTIES TIMEOUT 180 RESOURCE_LOCK "semantics_http;visualization_browser")

add_executable(test_channel_dictionary test_channel_dictionary.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/channel_dictionary.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_control.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/operator_data_contract_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_registry.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/config_content_validator.cpp)
target_include_directories(test_channel_dictionary PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/channels/config)
target_compile_options(test_channel_dictionary PRIVATE -UNDEBUG)
add_thirddepen(test_channel_dictionary rapidjson sqlite yaml-cpp)
target_link_libraries(test_channel_dictionary OpenSSL::Crypto)
add_test(NAME test_channel_dictionary COMMAND $<TARGET_FILE:test_channel_dictionary>)
add_test(NAME test_channel_dictionary_schema COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_channel_dictionary_schema.py ${CMAKE_SOURCE_DIR}/..)

add_executable(test_channel_dictionary_sources test_channel_dictionary_sources.cpp
    ${CMAKE_SOURCE_DIR}/services/catalog/lazy_dataframe_channel.cpp)
target_include_directories(test_channel_dictionary_sources PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_channel_dictionary_sources PRIVATE -UNDEBUG)
add_thirddepen(test_channel_dictionary_sources arrow rapidjson sqlite)
target_link_libraries(test_channel_dictionary_sources flowsql_catalog flowsql_kafka_runtime OpenSSL::Crypto)
add_test(NAME test_channel_dictionary_sources COMMAND $<TARGET_FILE:test_channel_dictionary_sources>)

add_executable(test_channel_dictionary_host channel_dictionary_host.cpp)
target_include_directories(test_channel_dictionary_host PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_channel_dictionary_host PRIVATE -UNDEBUG)
add_thirddepen(test_channel_dictionary_host rapidjson sqlite)
target_link_libraries(test_channel_dictionary_host ${CMAKE_DL_LIBS} OpenSSL::Crypto)
add_dependencies(test_channel_dictionary_host flowsql_config_channel flowsql_web flowsql_gateway flowsql_router
    flowsql_builtin flowsql_catalog flowsql_kafka flowsql_database)
add_test(NAME test_channel_dictionary_api COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_channel_dictionary_api.py $<TARGET_FILE:test_channel_dictionary_host>)
set_tests_properties(test_channel_dictionary_api PROPERTIES TIMEOUT 180 RESOURCE_LOCK semantics_http)
add_test(NAME test_channel_dictionary_iid COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_channel_dictionary_iid.py $<TARGET_FILE:test_channel_dictionary_host>)
set_tests_properties(test_channel_dictionary_iid PROPERTIES TIMEOUT 60)

add_executable(test_channel_dictionary_database test_channel_dictionary_database.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/channel_dictionary.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_control.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/operator_data_contract_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_registry.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/config_content_validator.cpp)
target_include_directories(test_channel_dictionary_database PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/channels/config)
target_compile_options(test_channel_dictionary_database PRIVATE -UNDEBUG)
add_thirddepen(test_channel_dictionary_database arrow rapidjson sqlite yaml-cpp)
target_link_libraries(test_channel_dictionary_database flowsql_database OpenSSL::Crypto)
add_test(NAME test_channel_dictionary_database COMMAND $<TARGET_FILE:test_channel_dictionary_database>)
add_test(NAME test_channel_dictionary_database_unit COMMAND $<TARGET_FILE:test_channel_dictionary_database> --sqlite)
set_tests_properties(test_channel_dictionary_database PROPERTIES TIMEOUT 180 RESOURCE_LOCK database_driver)

add_test(NAME test_channel_dictionary_browser COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_channel_dictionary_browser.py $<TARGET_FILE:test_channel_dictionary_host> ${FLOWSQL_SEMANTICS_NODE})
set_tests_properties(test_channel_dictionary_browser PROPERTIES TIMEOUT 200 RESOURCE_LOCK "semantics_http;visualization_browser")

add_executable(test_channel_dictionary_consistency test_channel_dictionary_consistency.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/channel_dictionary.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_control.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/operator_data_contract_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_registry.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/config_content_validator.cpp)
target_include_directories(test_channel_dictionary_consistency PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/channels/config)
target_compile_options(test_channel_dictionary_consistency PRIVATE -UNDEBUG)
add_thirddepen(test_channel_dictionary_consistency rapidjson sqlite yaml-cpp)
target_link_libraries(test_channel_dictionary_consistency OpenSSL::Crypto)
add_test(NAME test_channel_dictionary_consistency COMMAND $<TARGET_FILE:test_channel_dictionary_consistency>)
set_tests_properties(test_channel_dictionary_consistency PROPERTIES TIMEOUT 30)

add_executable(test_operator_contract_model test_operator_contract_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/operator_data_contract_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_registry.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_control.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/config_content_validator.cpp)
target_include_directories(test_operator_contract_model PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/channels/config)
target_compile_options(test_operator_contract_model PRIVATE -UNDEBUG)
add_thirddepen(test_operator_contract_model rapidjson sqlite yaml-cpp)
target_link_libraries(test_operator_contract_model OpenSSL::Crypto)
target_compile_definitions(test_operator_contract_model PRIVATE FLOWSQL_SOURCE_ROOT="${CMAKE_SOURCE_DIR}/..")
add_test(NAME test_operator_contract_model COMMAND $<TARGET_FILE:test_operator_contract_model>)
add_test(NAME test_operator_contract_schema COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_operator_contract_schema.py ${CMAKE_SOURCE_DIR}/..)

add_executable(test_baseliner_contract_schema test_baseliner_contract_schema.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/operator_data_contract_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/config_content_validator.cpp)
target_include_directories(test_baseliner_contract_schema PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/channels/config)
add_thirddepen(test_baseliner_contract_schema rapidjson yaml-cpp)
target_link_libraries(test_baseliner_contract_schema flowsql_baseliner_contract OpenSSL::Crypto)
target_compile_options(test_baseliner_contract_schema PRIVATE -UNDEBUG)
target_compile_definitions(test_baseliner_contract_schema PRIVATE FLOWSQL_SOURCE_ROOT="${CMAKE_SOURCE_DIR}/..")
add_test(NAME test_baseliner_contract_schema COMMAND $<TARGET_FILE:test_baseliner_contract_schema>)
set_tests_properties(test_baseliner_contract_schema PROPERTIES TIMEOUT 30)

add_executable(test_operator_contract_check test_operator_contract_check.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/operator_contract_service.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/operator_contract_derivation.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/channel_dictionary.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/operator_data_contract_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_registry.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/config_content_validator.cpp)
target_include_directories(test_operator_contract_check PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/channels/config)
target_compile_options(test_operator_contract_check PRIVATE -UNDEBUG)
add_thirddepen(test_operator_contract_check rapidjson sqlite yaml-cpp)
target_link_libraries(test_operator_contract_check OpenSSL::Crypto)
add_test(NAME test_operator_contract_check COMMAND $<TARGET_FILE:test_operator_contract_check>)

add_executable(test_baseliner_contract_invocation test_baseliner_contract_invocation.cpp)
target_include_directories(test_baseliner_contract_invocation PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(test_baseliner_contract_invocation arrow rapidjson)
target_link_libraries(test_baseliner_contract_invocation flowsql_common flowsql_baseliner_contract OpenSSL::Crypto)
target_compile_options(test_baseliner_contract_invocation PRIVATE -UNDEBUG)
target_compile_definitions(test_baseliner_contract_invocation PRIVATE FLOWSQL_SOURCE_ROOT="${CMAKE_SOURCE_DIR}/..")
foreach(component IN ITEMS config_channel builtin catalog database baseliner)
    string(TOUPPER ${component} upper)
    if(component STREQUAL "config_channel")
        set(upper CONFIG)
    endif()
    target_compile_definitions(test_baseliner_contract_invocation PRIVATE ${upper}_PLUGIN_PATH="$<TARGET_FILE:flowsql_${component}>")
    add_dependencies(test_baseliner_contract_invocation flowsql_${component})
endforeach()
add_test(NAME test_baseliner_contract_invocation COMMAND $<TARGET_FILE:test_baseliner_contract_invocation>)
set_tests_properties(test_baseliner_contract_invocation PROPERTIES TIMEOUT 60)

add_executable(test_operator_contract_output test_operator_contract_output.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/operator_contract_service.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/operator_contract_derivation.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/operator_data_contract_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_registry.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/config_content_validator.cpp)
target_include_directories(test_operator_contract_output PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/channels/config)
target_compile_options(test_operator_contract_output PRIVATE -UNDEBUG)
add_thirddepen(test_operator_contract_output rapidjson sqlite yaml-cpp)
target_link_libraries(test_operator_contract_output OpenSSL::Crypto)
add_test(NAME test_operator_contract_output COMMAND $<TARGET_FILE:test_operator_contract_output>)

add_executable(test_operator_contract_host operator_contract_host.cpp)
target_include_directories(test_operator_contract_host PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_operator_contract_host PRIVATE -UNDEBUG -Wno-type-limits)
add_thirddepen(test_operator_contract_host arrow rapidjson)
target_link_libraries(test_operator_contract_host flowsql_common OpenSSL::Crypto ${CMAKE_DL_LIBS})
add_dependencies(test_operator_contract_host flowsql_gateway flowsql_router flowsql_config_channel flowsql_builtin
    flowsql_catalog flowsql_binaddon flowsql_bridge flowsql_database flowsql_baseline flowsql_scheduler flowsql_web
    fixture_contract_output flowsql_baseliner flowsql_task)
target_compile_definitions(test_operator_contract_host PRIVATE FIXTURE_OUTPUT_PATH="$<TARGET_FILE:fixture_contract_output>" BASELINER_PATH="$<TARGET_FILE:flowsql_baseliner>")
set_target_properties(test_operator_contract_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_operator_contract_api COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_operator_contract_api.py $<TARGET_FILE:test_operator_contract_host>)
set_tests_properties(test_operator_contract_api PROPERTIES TIMEOUT 180 RESOURCE_LOCK semantics_http)

add_test(NAME test_operator_contract_preflight COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_operator_contract_api.py $<TARGET_FILE:test_operator_contract_host> --local)
set_tests_properties(test_operator_contract_preflight PROPERTIES TIMEOUT 90)

add_test(NAME test_operator_contract_browser COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_operator_contract_browser.py $<TARGET_FILE:test_operator_contract_host> ${FLOWSQL_SEMANTICS_NODE})
set_tests_properties(test_operator_contract_browser PROPERTIES TIMEOUT 220 RESOURCE_LOCK "semantics_http;visualization_browser")
