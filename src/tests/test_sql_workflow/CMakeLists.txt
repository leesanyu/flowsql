add_custom_target(sql_workflow_runtime DEPENDS flowsql flowsql_builtin flowsql_catalog flowsql_database
    flowsql_stream flowsql_scheduler flowsql_task flowsql_web flowsql_router flowsql_binaddon
    flowsql_npi flowsql_pcapfile flowsql_netadapter flowsql_capture_af_packet flowsql_config_channel flowsql_npm_basic
    flowsql_flow_labeling flowsql_baseline flowsql_baseliner)
add_test(NAME test_sql_workflow_docs COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_sql_workflow.py ${CMAKE_BINARY_DIR}/output docs)
set_tests_properties(test_sql_workflow_docs PROPERTIES TIMEOUT 30 WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_sql_workflow_discovery COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_sql_workflow.py ${CMAKE_BINARY_DIR}/output discovery)
set_tests_properties(test_sql_workflow_discovery PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_sql_workflow_finite COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_sql_workflow.py ${CMAKE_BINARY_DIR}/output finite)
set_tests_properties(test_sql_workflow_finite PROPERTIES TIMEOUT 180 WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output)
add_test(NAME test_sql_workflow_stream COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_sql_workflow.py ${CMAKE_BINARY_DIR}/output stream)
set_tests_properties(test_sql_workflow_stream PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output)
