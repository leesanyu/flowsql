project(flowsql_binaddon)

file(GLOB_RECURSE DIR_SRCS *.cpp)

add_library(${PROJECT_NAME} SHARED ${DIR_SRCS})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} rapidjson sqlite)

find_package(OpenSSL REQUIRED)
target_link_libraries(${PROJECT_NAME} OpenSSL::Crypto)

add_dependencies(${PROJECT_NAME} flowsql_common)
target_link_libraries(${PROJECT_NAME} flowsql_common)

set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
