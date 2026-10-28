project(test_config_channel)

add_executable(${PROJECT_NAME}
    test_config_channel.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_control.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_model.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/data_semantics_registry.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/config_channel_provider.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/config_channel_control.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/config_content_validator.cpp
)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/channels/config)
target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)
add_thirddepen(${PROJECT_NAME} rapidjson sqlite yaml-cpp)
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
