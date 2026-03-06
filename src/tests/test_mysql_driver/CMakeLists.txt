project(test_mysql_driver)

add_executable(${PROJECT_NAME} test_mysql_driver.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow)

# 需要链接 database 库才能使用驱动实现
target_link_directories(${PROJECT_NAME} PRIVATE ${CMAKE_BINARY_DIR}/output)
add_dependencies(${PROJECT_NAME} flowsql_common flowsql_database)
target_link_libraries(${PROJECT_NAME} flowsql_common flowsql_database)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
