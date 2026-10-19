include(ExternalProject)

set(_glog_install ${THIRDPARTS_INSTALL_DIR}/glog)

if(EXISTS "${_glog_install}/lib/libglog.a" OR EXISTS "${_glog_install}/lib64/libglog.a")
    message(STATUS "glog: using cached install at ${_glog_install}")
    add_custom_target(glog)
else()
    ExternalProject_Add(glog
        PREFIX ${THIRDPARTS_PREFIX_DIR}/glog
        URL "https://github.com/google/glog/archive/v0.4.0.tar.gz"
        URL_HASH SHA256=f28359aeba12f30d73d9e4711ef356dc842886968112162bc73002645139c39c
        CMAKE_ARGS ${FLOWSQL_LEGACY_CMAKE_ARGS} -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DWITH_GFLAGS=0 -DCMAKE_INSTALL_PREFIX:PATH=${_glog_install}
    )
endif()

set(glog_LINK_INC ${_glog_install}/include)
set(glog_LINK_DIR ${_glog_install}/lib64 ${_glog_install}/lib)
set(glog_LINK_TAR -lglog)
