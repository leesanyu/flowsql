add_library(flowsql_visualization SHARED visualization_plugin.cpp workflow.cpp exploration_service.cpp dataframe_transfer.cpp dataframe_workflow.cpp)
target_include_directories(flowsql_visualization PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(flowsql_visualization arrow httplib rapidjson)
find_package(OpenSSL REQUIRED)
target_link_libraries(flowsql_visualization OpenSSL::Crypto)
set_target_properties(flowsql_visualization PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)

set(exploration ${CMAKE_SOURCE_DIR}/exploration)
file(GLOB_RECURSE exploration_sources CONFIGURE_DEPENDS ${exploration}/src/*)
set(npm_stamp ${CMAKE_CURRENT_BINARY_DIR}/npm-ci.stamp)
add_custom_command(OUTPUT ${npm_stamp}
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${FLOWSQL_NODE_BIN}:$ENV{PATH}"
        ${FLOWSQL_NPM_EXECUTABLE} ci --no-audit --no-fund --fetch-retries=1 --fetch-timeout=30000
        --cache ${THIRDPARTS_PREFIX_DIR}/npm-cache
    COMMAND ${CMAKE_COMMAND} -E touch ${npm_stamp}
    DEPENDS ${exploration}/package.json ${exploration}/package-lock.json
    WORKING_DIRECTORY ${exploration} VERBATIM)
set(frontend_stamp ${CMAKE_CURRENT_BINARY_DIR}/exploration.stamp)
add_custom_command(OUTPUT ${frontend_stamp} ${CMAKE_BINARY_DIR}/output/exploration/manifest.json
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${FLOWSQL_NODE_BIN}:$ENV{PATH}"
        "FLOWSQL_BUILD_INFO_FILE=${FLOWSQL_VERSION_DIR}/build-info.json"
        ${FLOWSQL_NPM_EXECUTABLE} run build -- --outDir ${CMAKE_BINARY_DIR}/output/exploration
    COMMAND ${CMAKE_COMMAND} -E touch ${frontend_stamp}
    DEPENDS ${npm_stamp} ${exploration_sources} ${exploration}/index.html ${exploration}/vite.config.js
        ${FLOWSQL_VERSION_DIR}/build-info.json
    WORKING_DIRECTORY ${exploration} VERBATIM)
add_custom_target(flowsql_exploration DEPENDS ${frontend_stamp} ${CMAKE_BINARY_DIR}/output/exploration/manifest.json)
add_dependencies(flowsql_exploration flowsql_version)
add_dependencies(flowsql_visualization flowsql_exploration)
