project(flowsql_database)

file(GLOB DIR_SRCS *.cpp)
file(GLOB DRIVER_SRCS drivers/*.cpp)

add_library(${PROJECT_NAME} SHARED ${DIR_SRCS} ${DRIVER_SRCS})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_SOURCE_DIR})

add_thirddepen(${PROJECT_NAME} arrow rapidjson sqlite)

# MySQL 客户端库是必需的依赖（产品特性）
find_path(MYSQL_INCLUDE_DIR mysql/mysql.h
    PATHS /usr/include /usr/local/include
)
find_library(MYSQL_LIBRARY mysqlclient
    PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu
)

if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
    message(STATUS "MySQL client library found: ${MYSQL_LIBRARY}")
    target_include_directories(${PROJECT_NAME} PRIVATE ${MYSQL_INCLUDE_DIR})
    target_link_libraries(${PROJECT_NAME} ${MYSQL_LIBRARY})
else()
    message(FATAL_ERROR "MySQL client library not found. This is a required dependency.\n"
                        "Please install: sudo apt-get install libmysqlclient-dev")
endif()

add_dependencies(${PROJECT_NAME} flowsql_common)
target_link_libraries(${PROJECT_NAME} flowsql_common)

set_target_properties(${PROJECT_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output
)
