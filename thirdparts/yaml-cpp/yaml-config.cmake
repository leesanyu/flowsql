set(_yamlcpp_install ${THIRDPARTS_INSTALL_DIR}/yaml-cpp)

if(EXISTS "${_yamlcpp_install}/lib/libyaml-cpp.so" OR EXISTS "${_yamlcpp_install}/lib/libyaml-cpp.a")
    message(STATUS "yaml-cpp: using cached install at ${_yamlcpp_install}")
    add_custom_target(yaml-cpp)
else()
    include(ExternalProject)
    ExternalProject_Add(yaml-cpp
        PREFIX ${THIRDPARTS_PREFIX_DIR}/yaml-cpp
        URL "https://github.com/jbeder/yaml-cpp/archive/yaml-cpp-0.9.0.tar.gz"
        CMAKE_ARGS -DYAML_BUILD_SHARED_LIBS=1 -DCMAKE_INSTALL_PREFIX:PATH=${_yamlcpp_install}
    )
endif()

set(yaml-cpp_LINK_INC ${_yamlcpp_install}/include)
set(yaml-cpp_LINK_DIR ${_yamlcpp_install}/lib)
set(yaml-cpp_LINK_TAR -lyaml-cpp)
