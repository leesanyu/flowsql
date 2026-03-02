# cpp-httplib 依赖配置 (header-only)
# 优先使用缓存，其次从 GitHub 下载

set(_httplib_install ${THIRDPARTS_INSTALL_DIR}/httplib)

if(EXISTS "${_httplib_install}/include/httplib.h")
    message(STATUS "httplib: using cached install at ${_httplib_install}")
    add_custom_target(httplib)
else()
    include(ExternalProject)
    ExternalProject_Add(httplib
        PREFIX ${THIRDPARTS_PREFIX_DIR}/httplib
        URL "https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ${CMAKE_COMMAND} -E make_directory ${_httplib_install}/include
                COMMAND ${CMAKE_COMMAND} -E copy
                    <SOURCE_DIR>/httplib.h
                    ${_httplib_install}/include/httplib.h
    )
endif()

set(httplib_LINK_INC ${_httplib_install}/include)
set(httplib_LINK_DIR "")
set(httplib_LINK_TAR "")
