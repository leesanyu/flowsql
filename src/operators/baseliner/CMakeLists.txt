project(flowsql_baseliner_contract)
find_package(OpenSSL REQUIRED)
find_package(ICU REQUIRED COMPONENTS uc i18n)
add_library(${PROJECT_NAME} STATIC baseliner_contract.cpp baseliner_effective_config.cpp snapshot_reader.cpp dataframe_reader.cpp bucket_aggregator.cpp snapshot_input.cpp poll_input.cpp evaluation.cpp result_codec.cpp writer_fence.cpp durable_store.cpp model_output.cpp)
set_target_properties(${PROJECT_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})
add_thirddepen(${PROJECT_NAME} arrow rapidjson)
target_link_libraries(${PROJECT_NAME} OpenSSL::Crypto ICU::uc ICU::i18n)

add_library(flowsql_baseliner SHARED baseliner_operator.cpp baseliner_data_contract.cpp baseliner_plugin_exports.cpp)
target_link_libraries(flowsql_baseliner PRIVATE flowsql_baseliner_contract flowsql_common)
set_target_properties(flowsql_baseliner PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)

file(READ "${CMAKE_SOURCE_DIR}/../config/semantics/examples/operator-contract-baseliner.json" FLOWSQL_OPERATOR_CONTRACT)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/../config/semantics/examples/operator-contract-baseliner.json")
configure_file(operator_contract_package.h.in ${CMAKE_CURRENT_BINARY_DIR}/operator_contract_package.h @ONLY)
target_include_directories(flowsql_baseliner PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
