project(test_router)

add_executable(${PROJECT_NAME}
    test_router.cpp
    # 直接编译源文件，避免 .so 符号问题
    ${CMAKE_SOURCE_DIR}/services/gateway/route_table.cpp
    ${CMAKE_SOURCE_DIR}/services/router/router_agency_plugin.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} httplib rapidjson)

target_compile_options(${PROJECT_NAME} PRIVATE -UNDEBUG)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
