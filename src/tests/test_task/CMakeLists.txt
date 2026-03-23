project(test_task)

add_executable(${PROJECT_NAME} test_task.cpp)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(${PROJECT_NAME} rapidjson sqlite)

add_dependencies(${PROJECT_NAME} flowsql_task flowsql_common)
target_link_libraries(${PROJECT_NAME} flowsql_task flowsql_common)

target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
