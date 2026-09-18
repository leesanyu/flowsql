project(flowsql_config_channel)

add_library(${PROJECT_NAME} SHARED
    config_channel_provider.cpp
    config_channel_control.cpp
    config_content_validator.cpp
    plugin_register.cpp
)
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(${PROJECT_NAME} rapidjson sqlite yaml-cpp)
find_package(OpenSSL REQUIRED)
target_link_libraries(${PROJECT_NAME} OpenSSL::Crypto)
set_target_properties(${PROJECT_NAME} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
