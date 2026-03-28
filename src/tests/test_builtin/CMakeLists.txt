project(test_builtin)

add_library(fixture_cpp_operator SHARED
    fixtures/fixture_cpp_operator.cpp
)
target_include_directories(fixture_cpp_operator PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(fixture_cpp_operator PRIVATE -fvisibility=hidden)
set_target_properties(fixture_cpp_operator PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

add_library(fixture_bad_abi SHARED
    fixtures/fixture_bad_abi.cpp
)
target_include_directories(fixture_bad_abi PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(fixture_bad_abi PRIVATE -fvisibility=hidden)
set_target_properties(fixture_bad_abi PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)

add_executable(${PROJECT_NAME}
    test_builtin.cpp
    ${CMAKE_SOURCE_DIR}/services/catalog/catalog_plugin.cpp
    ${CMAKE_SOURCE_DIR}/services/binaddon/binaddon_host_plugin.cpp
    ${CMAKE_SOURCE_DIR}/services/catalog/builtin/concat_operator.cpp
    ${CMAKE_SOURCE_DIR}/services/catalog/builtin/hstack_operator.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow rapidjson sqlite)
find_package(OpenSSL REQUIRED)
target_link_libraries(${PROJECT_NAME} OpenSSL::Crypto)

add_dependencies(${PROJECT_NAME} flowsql_common)
add_dependencies(${PROJECT_NAME} fixture_cpp_operator fixture_bad_abi)
target_link_libraries(${PROJECT_NAME} flowsql_common)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    FIXTURE_CPP_SO_PATH="$<TARGET_FILE:fixture_cpp_operator>"
    FIXTURE_BAD_ABI_SO_PATH="$<TARGET_FILE:fixture_bad_abi>"
)

target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
