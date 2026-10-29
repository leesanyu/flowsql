project(flowsql_builtin)

set(DIR_SRCS
    builtin_registry.cpp
    builtin_registry_plugin.cpp
    plugin_register.cpp
    ${CMAKE_SOURCE_DIR}/framework/core/stream_hub_channel.cpp
)

add_library(${PROJECT_NAME} SHARED ${DIR_SRCS})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow rapidjson)

add_dependencies(${PROJECT_NAME} flowsql_common)
find_package(OpenSSL REQUIRED)
target_link_libraries(${PROJECT_NAME} flowsql_common OpenSSL::Crypto ${CMAKE_DL_LIBS})

set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

file(READ "${CMAKE_SOURCE_DIR}/../config/semantics/examples/operator-contract-passthrough.json" FLOWSQL_OPERATOR_CONTRACT)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/../config/semantics/examples/operator-contract-passthrough.json")
configure_file(operator_contract_package.h.in ${CMAKE_CURRENT_BINARY_DIR}/operator_contract_package.h @ONLY)
target_include_directories(flowsql_builtin PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
