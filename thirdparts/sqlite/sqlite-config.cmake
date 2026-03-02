# SQLite amalgamation 依赖配置
# 下载 amalgamation 源码，编译为静态库

set(_sqlite_install ${THIRDPARTS_INSTALL_DIR}/sqlite)

if(EXISTS "${_sqlite_install}/lib/libsqlite3.a")
    message(STATUS "sqlite: using cached install at ${_sqlite_install}")
    add_custom_target(sqlite)
else()
    include(ExternalProject)
    ExternalProject_Add(sqlite
        PREFIX ${THIRDPARTS_PREFIX_DIR}/sqlite
        URL "https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ${CMAKE_C_COMPILER} -c -O2 -fPIC -DSQLITE_THREADSAFE=1
            <SOURCE_DIR>/sqlite3.c -o <BINARY_DIR>/sqlite3.o
            COMMAND ${CMAKE_AR} rcs <BINARY_DIR>/libsqlite3.a <BINARY_DIR>/sqlite3.o
        INSTALL_COMMAND ${CMAKE_COMMAND} -E make_directory ${_sqlite_install}/include
                COMMAND ${CMAKE_COMMAND} -E make_directory ${_sqlite_install}/lib
                COMMAND ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/sqlite3.h ${_sqlite_install}/include/
                COMMAND ${CMAKE_COMMAND} -E copy <BINARY_DIR>/libsqlite3.a ${_sqlite_install}/lib/
    )
endif()

set(sqlite_LINK_INC ${_sqlite_install}/include)
set(sqlite_LINK_DIR ${_sqlite_install}/lib)
set(sqlite_LINK_TAR -lsqlite3)
