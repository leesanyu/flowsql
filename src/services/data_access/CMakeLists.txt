add_library(flowsql_data_access SHARED data_access_plugin.cpp sources.cpp access_store.cpp execution.cpp analysis.cpp
    ${CMAKE_SOURCE_DIR}/services/visualization/workflow.cpp
    ${CMAKE_SOURCE_DIR}/services/visualization/dataframe_workflow.cpp)
target_include_directories(flowsql_data_access PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(flowsql_data_access arrow rapidjson httplib sqlite)
find_package(OpenSSL REQUIRED)
target_link_libraries(flowsql_data_access flowsql_common OpenSSL::Crypto)
set_target_properties(flowsql_data_access PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
