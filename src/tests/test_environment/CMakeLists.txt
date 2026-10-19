find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(test_environment_config test_config.cpp)
target_include_directories(test_environment_config PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(test_environment_config rapidjson)
target_compile_options(test_environment_config PRIVATE -UNDEBUG -Wall -Wextra -Werror)
set_target_properties(test_environment_config PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_environment_config COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_config.py $<TARGET_FILE:test_environment_config>)

add_executable(test_environment_probe probe.cpp)
target_include_directories(test_environment_probe PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(test_environment_probe arrow rapidjson)
target_link_libraries(test_environment_probe flowsql_database flowsql_common)
target_compile_options(test_environment_probe PRIVATE -UNDEBUG -Wall -Wextra -Werror -Wno-unused-parameter)
set_target_properties(test_environment_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)

add_executable(test_environment_kafka_probe kafka_probe.cpp)
target_include_directories(test_environment_kafka_probe PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_environment_kafka_probe PRIVATE flowsql_kafka_runtime)
target_compile_options(test_environment_kafka_probe PRIVATE -UNDEBUG)
set_target_properties(test_environment_kafka_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_environment_fixture COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_fixture.py)
add_test(NAME test_environment_runner COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_runner.py)
add_test(NAME test_environment_cleanup COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cleanup.py)

add_test(NAME test_environment_failures COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/../tools/test_environment/acceptance.py)
set_tests_properties(test_environment_failures PROPERTIES TIMEOUT 240 WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output)

add_test(NAME test_product_build COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_product_build.py)
set_tests_properties(test_product_build PROPERTIES TIMEOUT 120)
