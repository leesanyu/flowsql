include(ExternalProject)

set(_glog_install ${THIRDPARTS_INSTALL_DIR}/glog)

if(EXISTS "${_glog_install}/lib/libglog.a" OR EXISTS "${_glog_install}/lib64/libglog.a")
    message(STATUS "glog: using cached install at ${_glog_install}")
    add_custom_target(glog)
else()
    ExternalProject_Add(glog
        PREFIX ${THIRDPARTS_PREFIX_DIR}/glog
        URL "https://github.com/google/glog/archive/v0.4.0.tar.gz"
        CMAKE_ARGS -DWITH_GFLAGS=0 -DCMAKE_INSTALL_PREFIX:PATH=${_glog_install}
    )
endif()

set(glog_LINK_INC ${_glog_install}/include)
set(glog_LINK_DIR ${_glog_install}/lib64 ${_glog_install}/lib)
set(glog_LINK_TAR -lglog)
