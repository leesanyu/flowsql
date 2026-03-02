project(flowsql_web_all)

# --- libflowsql_web.so（Web 插件） ---
file(GLOB WEB_PLUGIN_SRCS
    web_server.cpp
    web_plugin.cpp
    web_plugin_register.cpp
    db/database.cpp
)

add_library(flowsql_web SHARED ${WEB_PLUGIN_SRCS})
target_include_directories(flowsql_web PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(flowsql_web arrow httplib rapidjson sqlite)
add_dependencies(flowsql_web flowsql_framework)
target_link_libraries(flowsql_web flowsql_framework)
set_target_properties(flowsql_web PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

# --- flowsql 可执行文件（通用入口） ---
add_executable(flowsql main.cpp ${CMAKE_SOURCE_DIR}/services/gateway/service_client.cpp)
target_include_directories(flowsql PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(flowsql httplib rapidjson)
add_dependencies(flowsql flowsql_framework)
target_link_libraries(flowsql flowsql_framework)
set_target_properties(flowsql PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

# 构建后将 web/static 复制到 output/static（前端静态文件）
add_custom_command(TARGET flowsql POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/services/web/static
        ${CMAKE_BINARY_DIR}/output/static
    COMMENT "Copying web static files to output/static"
)
