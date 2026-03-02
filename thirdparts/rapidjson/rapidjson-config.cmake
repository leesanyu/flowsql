# RapidJSON 依赖配置
# 优先使用系统安装的 rapidjson-dev，其次使用缓存，最后从源码编译

find_path(RAPIDJSON_SYSTEM_INC NAMES rapidjson/document.h PATHS /usr/include)

if(RAPIDJSON_SYSTEM_INC)
    message(STATUS "rapidjson: using system install at ${RAPIDJSON_SYSTEM_INC}")
    add_custom_target(rapidjson)
    set(rapidjson_LINK_INC ${RAPIDJSON_SYSTEM_INC})
    set(rapidjson_LINK_DIR "")
    set(rapidjson_LINK_TAR )
else()
    set(_rapidjson_install ${THIRDPARTS_INSTALL_DIR}/rapidjson)

    if(EXISTS "${_rapidjson_install}/include/rapidjson/document.h")
        message(STATUS "rapidjson: using cached install at ${_rapidjson_install}")
        add_custom_target(rapidjson)
    else()
        include(ExternalProject)
        ExternalProject_Add(rapidjson
            PREFIX ${THIRDPARTS_PREFIX_DIR}/rapidjson
            URL "https://github.com/Tencent/rapidjson/archive/refs/tags/v1.1.0.tar.gz"
            CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=${_rapidjson_install}
                       -DRAPIDJSON_BUILD_THIRDPARTY_GTEST=OFF
                       -DRAPIDJSON_BUILD_EXAMPLES=OFF
                       -DCMAKE_CXX_FLAGS="-Wno-class-memaccess"
        )
    endif()

    set(rapidjson_LINK_INC ${_rapidjson_install}/include)
    set(rapidjson_LINK_DIR "")
    set(rapidjson_LINK_TAR )
endif()
