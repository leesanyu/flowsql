project(test_bridge)

set(DIR_SRCS main.cpp)

add_executable(${PROJECT_NAME} ${DIR_SRCS})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow rapidjson httplib)

add_dependencies(${PROJECT_NAME} flowsql_common flowsql_bridge)
target_link_libraries(${PROJECT_NAME} flowsql_common)

# 直接链接 bridge 的 .o 文件（测试不需要加载 .so）
target_sources(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/services/bridge/arrow_ipc_serializer.cpp
    ${CMAKE_SOURCE_DIR}/services/bridge/bridge_plugin.cpp
    ${CMAKE_SOURCE_DIR}/services/bridge/python_operator_bridge.cpp
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

find_package(OpenSSL REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
target_link_libraries(test_bridge OpenSSL::Crypto)
add_test(NAME test_bridge COMMAND $<TARGET_FILE:test_bridge>)
set_tests_properties(test_bridge PROPERTIES TIMEOUT 60)
add_executable(test_operator_contract_python test_operator_contract_python.cpp)
target_include_directories(test_operator_contract_python PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(test_operator_contract_python arrow rapidjson sqlite)
target_link_libraries(test_operator_contract_python flowsql_common OpenSSL::Crypto)
target_compile_options(test_operator_contract_python PRIVATE -UNDEBUG)
foreach(component IN ITEMS config_channel builtin catalog bridge scheduler)
    string(TOUPPER ${component} upper)
    if(component STREQUAL "config_channel")
        set(upper CONFIG)
    endif()
    target_compile_definitions(test_operator_contract_python PRIVATE ${upper}_PLUGIN_PATH="$<TARGET_FILE:flowsql_${component}>")
    add_dependencies(test_operator_contract_python flowsql_${component})
endforeach()
add_test(NAME test_operator_contract_python COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_operator_registry.py $<TARGET_FILE:test_operator_contract_python>)
add_test(NAME test_operator_registry COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_operator_registry.py)
set_tests_properties(test_operator_contract_python PROPERTIES TIMEOUT 90)
set_tests_properties(test_operator_registry PROPERTIES TIMEOUT 30)

add_test(NAME test_operator_python_compatibility COMMAND ${CMAKE_COMMAND} -E env
    PYTHONPATH=${CMAKE_SOURCE_DIR}/python ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_SOURCE_DIR}/python/tests -p test_link_timestamp.py)
set_tests_properties(test_operator_python_compatibility PROPERTIES TIMEOUT 30)
