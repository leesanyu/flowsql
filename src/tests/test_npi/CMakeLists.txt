project(test_npi)

cmake_minimum_required(VERSION 3.12)

file(GLOB_RECURSE DIR_SRCS *.cc *.cpp)

# 头文件目录（THIRDPARTS_DIR 由主 CMakeLists.txt 设置）
include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/common)
include_directories(${CMAKE_SOURCE_DIR}/plugins)
include_directories(${CMAKE_SOURCE_DIR}/plugins/npi)
include_directories(${THIRDPARTS_DIR})

# 库文件目录
link_directories(${CMAKE_BINARY_DIR}/output)

# 生成可执行文件
add_executable(${PROJECT_NAME} ${DIR_SRCS})

# 依赖库
add_thirddepen(${PROJECT_NAME} gflags glog yaml-cpp rapidjson)

# 链接 NPI 插件库
add_dependencies(${PROJECT_NAME} flowsql_npi)
target_link_libraries(${PROJECT_NAME} flowsql_npi)
