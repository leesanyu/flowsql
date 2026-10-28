project(flowsql_web_all)

# --- libflowsql_web.so（Web 插件） ---
file(GLOB WEB_PLUGIN_SRCS
    web_server.cpp
    web_plugin.cpp
    web_plugin_register.cpp
    db/database.cpp
)

add_library(flowsql_web SHARED ${WEB_PLUGIN_SRCS})
add_dependencies(flowsql_web flowsql_version)
target_include_directories(flowsql_web PUBLIC ${CMAKE_SOURCE_DIR} PRIVATE ${CMAKE_BINARY_DIR}/channels/config)
add_thirddepen(flowsql_web httplib rapidjson sqlite)
# Web 不需要 flowsql_common（不使用 DataFrame 等公共基础设施）
set_target_properties(flowsql_web PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

# Build the frontend from the lockfile, including frontend-only incremental edits.
set(frontend ${CMAKE_SOURCE_DIR}/frontend)
file(GLOB_RECURSE frontend_sources CONFIGURE_DEPENDS
    ${frontend}/src/* ${frontend}/public/*)
set(npm_stamp ${CMAKE_CURRENT_BINARY_DIR}/npm-ci.stamp)
add_custom_command(OUTPUT ${npm_stamp}
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${FLOWSQL_NODE_BIN}:$ENV{PATH}"
        ${FLOWSQL_NPM_EXECUTABLE} ci --no-audit --no-fund --fetch-retries=1 --fetch-timeout=30000
        --cache ${THIRDPARTS_PREFIX_DIR}/npm-cache
    COMMAND ${CMAKE_COMMAND} -E touch ${npm_stamp}
    DEPENDS ${frontend}/package.json ${frontend}/package-lock.json
    WORKING_DIRECTORY ${frontend}
    COMMENT "Installing locked frontend dependencies"
    VERBATIM)
set(frontend_stamp ${CMAKE_CURRENT_BINARY_DIR}/frontend.stamp)
add_custom_command(OUTPUT ${frontend_stamp} ${CMAKE_BINARY_DIR}/output/static/index.html
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${FLOWSQL_NODE_BIN}:$ENV{PATH}"
        "FLOWSQL_BUILD_INFO_FILE=${FLOWSQL_VERSION_DIR}/build-info.json"
        ${FLOWSQL_NPM_EXECUTABLE} run build -- --outDir ${CMAKE_BINARY_DIR}/output/static --emptyOutDir
    COMMAND ${CMAKE_COMMAND} -E touch ${frontend_stamp}
    DEPENDS ${npm_stamp} ${frontend_sources} ${frontend}/index.html ${frontend}/vite.config.js
        ${FLOWSQL_VERSION_DIR}/build-info.json
    WORKING_DIRECTORY ${frontend}
    COMMENT "Building frontend into output/static"
    VERBATIM)
add_custom_target(flowsql_frontend DEPENDS ${frontend_stamp} ${CMAKE_BINARY_DIR}/output/static/index.html)
add_dependencies(flowsql_frontend flowsql_version)
add_dependencies(flowsql_web flowsql_frontend)
