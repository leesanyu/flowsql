# --- flowsql 可执行文件（通用入口） ---
# Guardian 模式需要解析 gateway.yaml，因此链接 gateway 的 config.cpp 和 yaml-cpp
add_executable(flowsql
    main.cpp
    ${CMAKE_SOURCE_DIR}/services/gateway/config.cpp
)
target_include_directories(flowsql PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(flowsql httplib yaml-cpp)

set_target_properties(flowsql PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

set(FLOWSQL_RUNTIME_CONFIG_SOURCE_DIR ${CMAKE_SOURCE_DIR}/../config)
set(FLOWSQL_RUNTIME_CONFIG_OUTPUT_DIR ${CMAKE_BINARY_DIR}/output/config)
set(FLOWSQL_NPI_PROTOCOLS_SOURCE ${CMAKE_SOURCE_DIR}/plugins/npi/conf/protocols.yml)
add_custom_target(flowsql_runtime_config
    COMMAND ${CMAKE_COMMAND} -E make_directory
        ${FLOWSQL_RUNTIME_CONFIG_OUTPUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FLOWSQL_RUNTIME_CONFIG_SOURCE_DIR}/flowsql.yml
        ${FLOWSQL_RUNTIME_CONFIG_OUTPUT_DIR}/flowsql.yml
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FLOWSQL_RUNTIME_CONFIG_SOURCE_DIR}/deploy-single.yaml
        ${FLOWSQL_RUNTIME_CONFIG_OUTPUT_DIR}/deploy-single.yaml
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FLOWSQL_RUNTIME_CONFIG_SOURCE_DIR}/deploy-multi.yaml
        ${FLOWSQL_RUNTIME_CONFIG_OUTPUT_DIR}/deploy-multi.yaml
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FLOWSQL_NPI_PROTOCOLS_SOURCE}
        ${FLOWSQL_RUNTIME_CONFIG_OUTPUT_DIR}/protocols.yml
    DEPENDS
        ${FLOWSQL_RUNTIME_CONFIG_SOURCE_DIR}/flowsql.yml
        ${FLOWSQL_RUNTIME_CONFIG_SOURCE_DIR}/deploy-single.yaml
        ${FLOWSQL_RUNTIME_CONFIG_SOURCE_DIR}/deploy-multi.yaml
        ${FLOWSQL_NPI_PROTOCOLS_SOURCE}
    COMMENT "Syncing runtime config files to output/config"
    VERBATIM
)
add_dependencies(flowsql flowsql_runtime_config)

# 构建后将 web/static 复制到 output/static（前端静态文件）
# flowsql_web 目标已负责同步静态资源；这里避免并发 copy_directory 造成竞态失败。
if(NOT TARGET flowsql_web)
    add_custom_command(TARGET flowsql POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/services/web/static
            ${CMAKE_BINARY_DIR}/output/static
        COMMENT "Copying web static files to output/static"
    )
endif()
