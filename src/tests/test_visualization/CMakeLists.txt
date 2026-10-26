find_program(FLOWSQL_TEST_NODE NAMES node nodejs REQUIRED)
add_executable(test_catalog_lazy_loading test_catalog_lazy_loading.cpp
    ${CMAKE_SOURCE_DIR}/services/catalog/lazy_dataframe_channel.cpp)
target_include_directories(test_catalog_lazy_loading PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(test_catalog_lazy_loading arrow rapidjson sqlite)
target_link_libraries(test_catalog_lazy_loading flowsql_catalog flowsql_common)
target_compile_options(test_catalog_lazy_loading PRIVATE -UNDEBUG)
set_target_properties(test_catalog_lazy_loading PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_catalog_lazy_loading COMMAND test_catalog_lazy_loading)
set_tests_properties(test_catalog_lazy_loading PROPERTIES TIMEOUT 30)
add_executable(test_visualization_workflow test_workflow.cpp ${CMAKE_SOURCE_DIR}/services/visualization/workflow.cpp)
target_include_directories(test_visualization_workflow PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(test_visualization_workflow arrow rapidjson sqlite)
target_link_libraries(test_visualization_workflow flowsql_database)
target_compile_options(test_visualization_workflow PRIVATE -UNDEBUG)
set_target_properties(test_visualization_workflow PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_visualization_workflow COMMAND test_visualization_workflow)
add_test(NAME test_visualization_entry COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_entry.py ${CMAKE_BINARY_DIR}/output ${FLOWSQL_TEST_NODE})
set_tests_properties(test_visualization_entry PROPERTIES TIMEOUT 180 WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output
    RESOURCE_LOCK visualization_browser)
add_test(NAME test_visualization_compute COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_compute.py ${CMAKE_BINARY_DIR}/output ${FLOWSQL_TEST_NODE})
set_tests_properties(test_visualization_compute PROPERTIES TIMEOUT 180 WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output
    RESOURCE_LOCK visualization_browser)
add_test(NAME test_visualization_limits COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_limits.py ${CMAKE_BINARY_DIR}/output)
set_tests_properties(test_visualization_limits PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_visualization_client COMMAND ${FLOWSQL_TEST_NODE} --test --test-isolation=none ${CMAKE_CURRENT_SOURCE_DIR}/compute.test.mjs)
add_executable(test_visualization_dataframe_store
    test_dataframe_transfer.cpp
    ${CMAKE_SOURCE_DIR}/services/catalog/dataframe_transfer.cpp
    ${CMAKE_SOURCE_DIR}/services/visualization/dataframe_transfer.cpp)
target_include_directories(test_visualization_dataframe_store PRIVATE ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/services/catalog ${CMAKE_SOURCE_DIR}/services/visualization)
add_thirddepen(test_visualization_dataframe_store arrow httplib rapidjson)
target_link_libraries(test_visualization_dataframe_store OpenSSL::Crypto)
target_compile_options(test_visualization_dataframe_store PRIVATE -UNDEBUG)
set_target_properties(test_visualization_dataframe_store PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_visualization_dataframe_store COMMAND test_visualization_dataframe_store)
add_executable(test_visualization_dataframe_workflow test_dataframe_workflow.cpp
    ${CMAKE_SOURCE_DIR}/services/visualization/dataframe_workflow.cpp
    ${CMAKE_SOURCE_DIR}/services/visualization/workflow.cpp)
target_include_directories(test_visualization_dataframe_workflow PRIVATE ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/services/visualization)
add_thirddepen(test_visualization_dataframe_workflow arrow rapidjson)
target_link_libraries(test_visualization_dataframe_workflow OpenSSL::Crypto)
target_compile_options(test_visualization_dataframe_workflow PRIVATE -UNDEBUG)
set_target_properties(test_visualization_dataframe_workflow PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_visualization_dataframe_workflow COMMAND test_visualization_dataframe_workflow)
add_test(NAME test_visualization_dataframe_transfer COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dataframe_transfer.py ${CMAKE_BINARY_DIR}/output)
set_tests_properties(test_visualization_dataframe_transfer PROPERTIES TIMEOUT 180
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output RESOURCE_LOCK visualization_dataframe_transfer)
add_test(NAME test_visualization_delivery COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_delivery.py ${CMAKE_BINARY_DIR}/output ${FLOWSQL_TEST_NODE})
set_tests_properties(test_visualization_delivery PROPERTIES TIMEOUT 240 WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output
    RESOURCE_LOCK visualization_browser)

foreach(backend mysql postgres clickhouse)
add_executable(test_visualization_${backend} test_connectors.cpp ${CMAKE_SOURCE_DIR}/services/visualization/workflow.cpp)
target_include_directories(test_visualization_${backend} PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(test_visualization_${backend} arrow rapidjson sqlite)
target_link_libraries(test_visualization_${backend} flowsql_database)
target_compile_options(test_visualization_${backend} PRIVATE -UNDEBUG)
set_target_properties(test_visualization_${backend} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_visualization_${backend} COMMAND test_visualization_${backend})
set_tests_properties(test_visualization_${backend} PROPERTIES TIMEOUT 60)

if(backend STREQUAL "postgres")
    target_compile_definitions(test_visualization_${backend} PRIVATE FLOWSQL_VISUALIZATION_POSTGRES)
elseif(backend STREQUAL "clickhouse")
    target_compile_definitions(test_visualization_${backend} PRIVATE FLOWSQL_VISUALIZATION_CLICKHOUSE)
endif()
endforeach()

add_executable(test_visualization_connectors_fixture connectors_fixture.cpp ${CMAKE_SOURCE_DIR}/services/visualization/workflow.cpp)
target_include_directories(test_visualization_connectors_fixture PRIVATE ${CMAKE_SOURCE_DIR})
add_thirddepen(test_visualization_connectors_fixture arrow rapidjson sqlite)
target_link_libraries(test_visualization_connectors_fixture flowsql_database)
target_compile_options(test_visualization_connectors_fixture PRIVATE -UNDEBUG)
set_target_properties(test_visualization_connectors_fixture PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_visualization_connectors_e2e COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_connectors_e2e.py ${CMAKE_BINARY_DIR}/output ${FLOWSQL_TEST_NODE})
set_tests_properties(test_visualization_connectors_e2e PROPERTIES TIMEOUT 420 WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output
    RESOURCE_LOCK visualization_browser)

add_test(NAME test_visualization_dataframe_native COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dataframe_native.py ${CMAKE_BINARY_DIR}/output)
set_tests_properties(test_visualization_dataframe_native PROPERTIES TIMEOUT 240 WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output
    RESOURCE_LOCK visualization_dataframe_transfer)

add_test(NAME test_visualization_dataframe_browser COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dataframe_browser.py ${CMAKE_BINARY_DIR}/output ${FLOWSQL_TEST_NODE})
set_tests_properties(test_visualization_dataframe_browser PROPERTIES TIMEOUT 300 WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output
    RESOURCE_LOCK visualization_browser)

add_test(NAME test_visualization_chart COMMAND ${FLOWSQL_TEST_NODE} --test ${CMAKE_CURRENT_SOURCE_DIR}/chart.test.mjs)
add_test(NAME test_visualization_experience COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_experience.py ${CMAKE_BINARY_DIR}/output ${FLOWSQL_TEST_NODE})
set_tests_properties(test_visualization_experience PROPERTIES TIMEOUT 240 WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output
    RESOURCE_LOCK visualization_browser)

add_test(NAME test_visualization_statistics COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_statistics.py ${CMAKE_BINARY_DIR}/output ${FLOWSQL_TEST_NODE})
set_tests_properties(test_visualization_statistics PROPERTIES TIMEOUT 240 WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output
    RESOURCE_LOCK visualization_browser)
