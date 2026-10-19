include(ExternalProject)

set(_gflags_install ${THIRDPARTS_INSTALL_DIR}/gflags)

if(EXISTS "${_gflags_install}/lib/libgflags.a")
    message(STATUS "gflags: using cached install at ${_gflags_install}")
    add_custom_target(gflags)
else()
    ExternalProject_Add(gflags
        PREFIX ${THIRDPARTS_PREFIX_DIR}/gflags
        URL "https://github.com/gflags/gflags/archive/v2.3.0.tar.gz"
        URL_HASH SHA256=f619a51371f41c0ad6837b2a98af9d4643b3371015d873887f7e8d3237320b2f
        CMAKE_ARGS
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            ${FLOWSQL_LEGACY_CMAKE_ARGS}
            -DREGISTER_INSTALL_PREFIX=OFF
            -DCMAKE_INSTALL_PREFIX:PATH=${_gflags_install}
    )
endif()

set(gflags_LINK_INC ${_gflags_install}/include)
set(gflags_LINK_DIR ${_gflags_install}/lib)
set(gflags_LINK_TAR -lgflags)
