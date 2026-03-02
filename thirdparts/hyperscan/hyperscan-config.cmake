include(ExternalProject)

set(_hyperscan_install ${THIRDPARTS_INSTALL_DIR}/hyperscan)

if(EXISTS "${_hyperscan_install}/lib/libhs.a")
    message(STATUS "hyperscan: using cached install at ${_hyperscan_install}")
    add_custom_target(hyperscan)
else()
    ExternalProject_Add(hyperscan
        PREFIX ${THIRDPARTS_PREFIX_DIR}/hyperscan
        URL "https://github.com/intel/hyperscan/archive/refs/tags/v5.4.2.tar.gz"
        CMAKE_ARGS -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX:PATH=${_hyperscan_install}
    )
endif()

set(hyperscan_LINK_INC ${_hyperscan_install}/include/hs)
set(hyperscan_LINK_DIR ${_hyperscan_install}/lib)
set(hyperscan_LINK_TAR -lhs -lhs_runtime)
