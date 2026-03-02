cmake_minimum_required(VERSION 3.12)

project(flowsql)

# 设置编译参数
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fno-strict-aliasing -Wno-invalid-offsetof -std=c++17 -Wno-deprecated -Wno-unused-value")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-variable -Wno-unused-but-set-variable -Wno-array-bounds")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ldl -lm")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden -Wno-parentheses -Wno-sign-compare -Wno-unknown-pragmas -Wno-format")

# TODO: 代码开发阶段采用调试模式
SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS} -fPIC -O0 -Wall -g -ggdb ")
SET(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS} -fPIC -O3 -Wall -g ")

SET(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS} -fPIC -O0 -Wall -g -ggdb")
SET(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS} -fPIC -O3 -Wall -g")

IF(DEBUG_mode)
    add_definitions(-DDEBUG -D_DEBUG)
else()
    add_definitions(-DNDEBUG)
endif()

if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 4.8.1)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-literal-suffix -Wno-unused-local-typedefs")
endif()

# 设置库输出目录
set(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR}/output)
set(EXECUTABLE_OUTPUT_PATH  ${CMAKE_BINARY_DIR}/output)

# 设置 thirdparts 路径（已提到项目根）
set(THIRDPARTS_DIR ${CMAKE_SOURCE_DIR}/../thirdparts)

# 子项目
include(${CMAKE_SOURCE_DIR}/subjects.cmake)
add_thirdparts()

# 添加外部子项目
add_subdirectory(${CMAKE_SOURCE_DIR}/plugins/npi ${CMAKE_BINARY_DIR}/npi_plugin)
add_subdirectory(${CMAKE_SOURCE_DIR}/tests/test_npi ${CMAKE_BINARY_DIR}/test_npi)

# Stage 1: 框架核心
add_subdirectory(${CMAKE_SOURCE_DIR}/framework ${CMAKE_BINARY_DIR}/framework)
add_subdirectory(${CMAKE_SOURCE_DIR}/plugins/example ${CMAKE_BINARY_DIR}/example_plugin)
add_subdirectory(${CMAKE_SOURCE_DIR}/tests/test_framework ${CMAKE_BINARY_DIR}/test_framework)

# Stage 2: C++ ↔ Python 桥接
add_subdirectory(${CMAKE_SOURCE_DIR}/services/bridge ${CMAKE_BINARY_DIR}/bridge)
add_subdirectory(${CMAKE_SOURCE_DIR}/tests/test_bridge ${CMAKE_BINARY_DIR}/test_bridge)

# Stage 2: Web 管理系统
add_subdirectory(${CMAKE_SOURCE_DIR}/services/web ${CMAKE_BINARY_DIR}/web)

# Stage 2: Scheduler 调度服务
add_subdirectory(${CMAKE_SOURCE_DIR}/services/scheduler ${CMAKE_BINARY_DIR}/scheduler)

# Stage 3: Gateway 服务
add_subdirectory(${CMAKE_SOURCE_DIR}/services/gateway ${CMAKE_BINARY_DIR}/gateway)
