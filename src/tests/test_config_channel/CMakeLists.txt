project(test_config_channel)

add_executable(${PROJECT_NAME}
    test_config_channel.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/config_channel_provider.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/config_channel_control.cpp
    ${CMAKE_SOURCE_DIR}/channels/config/config_content_validator.cpp
)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR})
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
set_target_properties(test_config_channel_e2e PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_config_channel_e2e COMMAND ${CMAKE_BINARY_DIR}/output/test_config_channel_e2e)
set_tests_properties(test_config_channel_e2e PROPERTIES TIMEOUT 60)
