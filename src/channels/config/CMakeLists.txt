project(flowsql_config_channel)

add_library(${PROJECT_NAME} SHARED
    data_semantics_control.cpp
    data_semantics_model.cpp
    data_semantics_registry.cpp
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

file(READ "${CMAKE_SOURCE_DIR}/../config/semantics/documents.schema.json" FLOWSQL_SEMANTICS_SCHEMA)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/../config/semantics/documents.schema.json")
configure_file(data_semantics_schema.h.in ${CMAKE_CURRENT_BINARY_DIR}/data_semantics_schema.h @ONLY)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

file(READ "${CMAKE_SOURCE_DIR}/../config/semantics/api.schema.json" FLOWSQL_SEMANTICS_API_SCHEMA)
file(READ "${CMAKE_SOURCE_DIR}/../config/semantics/openapi.json" FLOWSQL_SEMANTICS_OPENAPI)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/../config/semantics/api.schema.json" "${CMAKE_SOURCE_DIR}/../config/semantics/openapi.json")
configure_file(data_semantics_api_schema.h.in ${CMAKE_CURRENT_BINARY_DIR}/data_semantics_api_schema.h @ONLY)
