project(flowsql_gateway)

file(GLOB_RECURSE DIR_SRCS *.cpp)
# 排除已删除的废弃文件（service_manager、service_client）
list(FILTER DIR_SRCS EXCLUDE REGEX "service_manager\\.cpp$")
list(FILTER DIR_SRCS EXCLUDE REGEX "service_client\\.cpp$")

add_library(${PROJECT_NAME} SHARED ${DIR_SRCS})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} httplib rapidjson yaml-cpp)

set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
