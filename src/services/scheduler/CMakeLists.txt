project(flowsql_scheduler)

file(GLOB DIR_SRCS CONFIGURE_DEPENDS *.cpp)

add_library(${PROJECT_NAME} SHARED
    ${DIR_SRCS}
    # Pipeline 和 ChannelAdapter 只有 Scheduler 使用，编译进 scheduler.so
    ${CMAKE_SOURCE_DIR}/framework/core/pipeline.cpp
    ${CMAKE_SOURCE_DIR}/framework/core/channel_adapter.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow httplib rapidjson)

add_dependencies(${PROJECT_NAME} flowsql_common)
target_link_libraries(${PROJECT_NAME} flowsql_common)
find_package(OpenSSL REQUIRED)
target_link_libraries(${PROJECT_NAME} OpenSSL::Crypto)

set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
