add_library(flowsql_kafka_structured STATIC kafka_structured.cpp)
target_include_directories(flowsql_kafka_structured PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(flowsql_kafka_structured arrow rapidjson)
set_target_properties(flowsql_kafka_structured PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(flowsql_kafka_store STATIC kafka_store.cpp)
target_include_directories(flowsql_kafka_store PUBLIC ${CMAKE_SOURCE_DIR})
add_dependencies(flowsql_kafka_store rapidjson sqlite)
target_include_directories(flowsql_kafka_store PUBLIC ${rapidjson_LINK_INC} PRIVATE ${sqlite_LINK_INC})
target_link_libraries(flowsql_kafka_store PUBLIC flowsql_kafka_structured PRIVATE OpenSSL::Crypto ${sqlite_LINK_DIR}/libsqlite3.a)
set_target_properties(flowsql_kafka_store PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(flowsql_kafka SHARED kafka_client.cpp kafka_reader.cpp kafka_plugin.cpp kafka_control.cpp plugin_register.cpp)
target_include_directories(flowsql_kafka PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(flowsql_kafka PRIVATE flowsql_kafka_store FlowSQL::KafkaClient)
set_target_properties(flowsql_kafka PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)

file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/build-info.json CONTENT
    "{\"client_version\":\"${FLOWSQL_KAFKA_CLIENT_VERSION}\",\"cache_fingerprint\":\"${FLOWSQL_KAFKA_CLIENT_FINGERPRINT}\",\"compiler\":\"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\",\"platform\":\"${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}\"}\n")
add_custom_command(TARGET flowsql_kafka POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/output/share/kafka
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/../thirdparts/librdkafka/dependencies.json ${CMAKE_BINARY_DIR}/output/share/kafka/dependencies.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/../thirdparts/librdkafka/LICENSES.txt ${CMAKE_BINARY_DIR}/output/share/kafka/LICENSES.txt
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_BINARY_DIR}/build-info.json ${CMAKE_BINARY_DIR}/output/share/kafka/build-info.json
    VERBATIM)
