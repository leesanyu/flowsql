# Arrow C++ 依赖配置
# 优先使用 pyarrow 自带的 C++ 库，其次使用缓存，最后从源码编译

set(PYARROW_DIR "/usr/local/lib/python3.12/dist-packages/pyarrow")

if(EXISTS "${PYARROW_DIR}/include/arrow/api.h" AND EXISTS "${PYARROW_DIR}/libarrow.so.2200")
    message(STATUS "arrow: using pyarrow at ${PYARROW_DIR}")
    add_custom_target(arrow)
    set(arrow_LINK_INC ${PYARROW_DIR}/include)
    set(arrow_LINK_DIR ${PYARROW_DIR})
    set(arrow_LINK_TAR ${PYARROW_DIR}/libarrow.so.2200)
else()
    set(_arrow_install ${THIRDPARTS_INSTALL_DIR}/arrow)

    if(EXISTS "${_arrow_install}/lib/libarrow.a" OR EXISTS "${_arrow_install}/lib/libarrow.so")
        message(STATUS "arrow: using cached install at ${_arrow_install}")
        add_custom_target(arrow)
    else()
        include(ExternalProject)
        ExternalProject_Add(arrow
            PREFIX ${THIRDPARTS_PREFIX_DIR}/arrow
            URL "https://github.com/apache/arrow/archive/refs/tags/apache-arrow-18.1.0.tar.gz"
            SOURCE_SUBDIR cpp
            CMAKE_ARGS
                -DARROW_BUILD_STATIC=ON
                -DARROW_BUILD_SHARED=OFF
                -DARROW_COMPUTE=OFF
                -DARROW_JSON=ON
                -DARROW_IPC=ON
                -DARROW_WITH_UTF8PROC=OFF
                -DARROW_WITH_RE2=OFF
                -DARROW_DEPENDENCY_SOURCE=BUNDLED
                -DCMAKE_POSITION_INDEPENDENT_CODE=ON
                -DCMAKE_INSTALL_PREFIX:PATH=${_arrow_install}
                -DCMAKE_BUILD_TYPE=Release
        )
    endif()

    set(arrow_LINK_INC ${_arrow_install}/include)
    set(arrow_LINK_DIR ${_arrow_install}/lib)
    set(arrow_LINK_TAR -larrow)
endif()
