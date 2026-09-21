project(test_flow_labeling)

add_executable(${PROJECT_NAME} test_flow_labeling.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS})
target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)
add_dependencies(${PROJECT_NAME} flowsql_flow_labeling)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

add_test(NAME test_flow_labeling COMMAND
    ${CMAKE_BINARY_DIR}/output/test_flow_labeling
    $<TARGET_FILE:flowsql_flow_labeling>
)

# Machine-dependent build cost, RSS, and throughput are observations rather than CTest thresholds.
add_executable(benchmark_flow_labeling benchmark_flow_labeling.cpp)
target_include_directories(benchmark_flow_labeling PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(benchmark_flow_labeling PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(benchmark_flow_labeling flowsql_flow_labeling)
set_target_properties(benchmark_flow_labeling PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
