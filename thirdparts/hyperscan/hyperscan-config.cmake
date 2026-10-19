include(ExternalProject)

set(_hyperscan_install ${THIRDPARTS_INSTALL_DIR}/hyperscan)

if(EXISTS "${_hyperscan_install}/lib/libhs.a")
    message(STATUS "hyperscan: using cached install at ${_hyperscan_install}")
    add_custom_target(hyperscan)
else()
    ExternalProject_Add(hyperscan
        PREFIX ${THIRDPARTS_PREFIX_DIR}/hyperscan
        URL "https://github.com/intel/hyperscan/archive/refs/tags/v5.4.2.tar.gz"
        URL_HASH SHA256=32b0f24b3113bbc46b6bfaa05cf7cf45840b6b59333d078cc1f624e4c40b2b99
        CMAKE_ARGS ${FLOWSQL_LEGACY_CMAKE_ARGS} -DBUILD_EXAMPLES=OFF
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX:PATH=${_hyperscan_install}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target hs hs_runtime --parallel 4
        INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
    )
endif()

set(hyperscan_LINK_INC ${_hyperscan_install}/include/hs)
set(hyperscan_LINK_DIR ${_hyperscan_install}/lib)
set(hyperscan_LINK_TAR -lhs -lhs_runtime)
