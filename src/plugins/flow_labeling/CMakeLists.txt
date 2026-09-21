project(flowsql_flow_labeling)

add_library(${PROJECT_NAME} SHARED
    flow_labeling_plugin.cpp
    plugin_register.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR})
if(TARGET yaml-cpp)
    add_dependencies(${PROJECT_NAME} yaml-cpp)
endif()
target_include_directories(${PROJECT_NAME} PRIVATE ${yaml-cpp_LINK_INC})
if(NOT "${yaml-cpp_LINK_DIR}" STREQUAL "")
    target_link_directories(${PROJECT_NAME} PRIVATE ${yaml-cpp_LINK_DIR})
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${CMAKE_DL_LIBS}
    pthread
    ${yaml-cpp_LINK_TAR}
    PkgConfig::DPDK
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
