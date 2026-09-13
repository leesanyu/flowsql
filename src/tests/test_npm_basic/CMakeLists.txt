project(test_npm_basic)

add_executable(${PROJECT_NAME} test_npm_basic.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npm_basic/npm_analysis_contract.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npm_basic/npm_session_key.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/npm_basic/npm_session_table.cpp)

add_dependencies(${PROJECT_NAME} flowsql_common)
target_link_libraries(${PROJECT_NAME} flowsql_common)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

add_test(NAME test_npm_basic COMMAND ${CMAKE_BINARY_DIR}/output/test_npm_basic)
