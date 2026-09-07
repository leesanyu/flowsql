project(test_framework)

file(GLOB_RECURSE DIR_SRCS *.cpp)
list(FILTER DIR_SRCS EXCLUDE REGEX "test_docker_deploy_config\\.cpp$")
list(FILTER DIR_SRCS EXCLUDE REGEX "test_native_deploy_config\\.cpp$")
list(FILTER DIR_SRCS EXCLUDE REGEX "test_pcap_upload_contract\\.cpp$")
list(FILTER DIR_SRCS EXCLUDE REGEX "test_plugin_loader\\.cpp$")

add_executable(${PROJECT_NAME} ${DIR_SRCS})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow rapidjson)

add_dependencies(${PROJECT_NAME} flowsql_common)
target_link_libraries(${PROJECT_NAME} flowsql_common)

# Pipeline 和 ChannelAdapter 源码直接编译进测试（测试不依赖 scheduler.so）
target_sources(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/framework/core/pipeline.cpp
    ${CMAKE_SOURCE_DIR}/framework/core/channel_adapter.cpp
)

# 测试二进制必须启用 assert，取消顶层 -DNDEBUG
target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

add_test(NAME test_framework COMMAND ${CMAKE_BINARY_DIR}/output/test_framework)

add_executable(test_pcap_upload_contract
    test_pcap_upload_contract.cpp
    ${CMAKE_SOURCE_DIR}/services/web/db/database.cpp
    ${CMAKE_SOURCE_DIR}/services/web/web_plugin.cpp
    ${CMAKE_SOURCE_DIR}/services/web/web_server.cpp
)
target_include_directories(test_pcap_upload_contract PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(test_pcap_upload_contract httplib rapidjson sqlite)
target_compile_options(test_pcap_upload_contract PRIVATE -UNDEBUG)
set_target_properties(test_pcap_upload_contract PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
add_test(NAME test_pcap_upload_contract COMMAND ${CMAKE_BINARY_DIR}/output/test_pcap_upload_contract)

function(add_plugin_loader_fixture target fixture_id external_entry)
    add_library(${target} MODULE test_plugin_loader.cpp)
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_definitions(${target} PRIVATE PLUGIN_LOADER_FIXTURE_ID=\"${fixture_id}\")
    if(external_entry)
        target_compile_definitions(${target} PRIVATE PLUGIN_LOADER_FIXTURE_EXTERNAL_ENTRY=1)
    endif()
    set_target_properties(${target} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
    )
endfunction()

add_plugin_loader_fixture(plugin_loader_fixture_a "a" TRUE)
add_plugin_loader_fixture(plugin_loader_fixture_b "b" FALSE)
add_plugin_loader_fixture(plugin_loader_fixture_c "c" FALSE)

add_executable(test_plugin_loader test_plugin_loader.cpp)
target_include_directories(test_plugin_loader PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_plugin_loader PRIVATE ${CMAKE_DL_LIBS})
target_compile_options(test_plugin_loader PRIVATE -UNDEBUG)
add_dependencies(test_plugin_loader
    plugin_loader_fixture_a
    plugin_loader_fixture_b
    plugin_loader_fixture_c
    flowsql_gateway
    flowsql_router
    flowsql_web
)
set_target_properties(test_plugin_loader PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
add_test(NAME test_plugin_loader COMMAND
    ${CMAKE_BINARY_DIR}/output/test_plugin_loader
    $<TARGET_FILE:plugin_loader_fixture_a>
    $<TARGET_FILE:plugin_loader_fixture_b>
    $<TARGET_FILE:plugin_loader_fixture_c>
    $<TARGET_FILE:flowsql_gateway>
    $<TARGET_FILE:flowsql_router>
    $<TARGET_FILE:flowsql_web>
)

add_executable(test_native_deploy_config
    test_native_deploy_config.cpp
    ${CMAKE_SOURCE_DIR}/services/gateway/config.cpp
)
target_include_directories(test_native_deploy_config PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(test_native_deploy_config yaml-cpp)
target_compile_definitions(test_native_deploy_config PRIVATE
    FLOWSQL_DEPLOY_SINGLE_PATH="${CMAKE_SOURCE_DIR}/../config/deploy-single.yaml"
    FLOWSQL_DEPLOY_MULTI_PATH="${CMAKE_SOURCE_DIR}/../config/deploy-multi.yaml"
    FLOWSQL_DEPLOY_SINGLE_OUTPUT_PATH="${CMAKE_BINARY_DIR}/output/config/deploy-single.yaml"
    FLOWSQL_DEPLOY_MULTI_OUTPUT_PATH="${CMAKE_BINARY_DIR}/output/config/deploy-multi.yaml"
    FLOWSQL_NPI_PROTOCOLS_SOURCE_PATH="${CMAKE_SOURCE_DIR}/plugins/npi/conf/protocols.yml"
    FLOWSQL_NPI_PROTOCOLS_OUTPUT_PATH="${CMAKE_BINARY_DIR}/output/config/protocols.yml"
)
target_compile_options(test_native_deploy_config PRIVATE -UNDEBUG)
add_dependencies(test_native_deploy_config flowsql)
set_target_properties(test_native_deploy_config PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
add_test(NAME test_native_deploy_config COMMAND ${CMAKE_BINARY_DIR}/output/test_native_deploy_config)

add_executable(test_docker_deploy_config test_docker_deploy_config.cpp)
target_include_directories(test_docker_deploy_config PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(test_docker_deploy_config yaml-cpp)
target_compile_definitions(test_docker_deploy_config PRIVATE
    FLOWSQL_DOCKERFILE_PATH="${CMAKE_SOURCE_DIR}/../Dockerfile"
    FLOWSQL_DOCKER_COMPOSE_PATH="${CMAKE_SOURCE_DIR}/../docker-compose.yml"
    FLOWSQL_BUILD_OUTPUT_PATH="${CMAKE_BINARY_DIR}/output"
)
target_compile_options(test_docker_deploy_config PRIVATE -UNDEBUG)
add_dependencies(test_docker_deploy_config
    flowsql_gateway
    flowsql_web
    flowsql_router
    flowsql_scheduler
    flowsql_bridge
    flowsql_database
    flowsql_npi
    flowsql_pcapfile
)
set_target_properties(test_docker_deploy_config PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
add_test(NAME test_docker_deploy_config COMMAND ${CMAKE_BINARY_DIR}/output/test_docker_deploy_config)
