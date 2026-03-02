project(flowsql_gateway)

file(GLOB_RECURSE DIR_SRCS *.cpp)
# service_client.cpp 由可执行文件编译，不包含在 gateway.so 中
list(FILTER DIR_SRCS EXCLUDE REGEX "service_client\\.cpp$")

add_library(${PROJECT_NAME} SHARED ${DIR_SRCS})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} httplib rapidjson yaml-cpp)

add_dependencies(${PROJECT_NAME} flowsql_framework)
target_link_libraries(${PROJECT_NAME} flowsql_framework)

set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
