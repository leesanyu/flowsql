# Eigen 3 依赖配置
# 优先使用系统安装，其次使用缓存，最后按需下载。
# 这是头文件库，只导出 include 路径，不导出 link 目标。

find_path(EIGEN_SYSTEM_INC
    NAMES Eigen/Core
    PATHS
        /usr/include/eigen3
        /usr/local/include/eigen3
)

if(EIGEN_SYSTEM_INC)
    message(STATUS "eigen: using system install at ${EIGEN_SYSTEM_INC}")
    add_custom_target(eigen)
    set(eigen_LINK_INC ${EIGEN_SYSTEM_INC})
    set(eigen_LINK_DIR "")
    set(eigen_LINK_TAR )
else()
    set(_eigen_install ${THIRDPARTS_INSTALL_DIR}/eigen)

    if(EXISTS "${_eigen_install}/include/eigen3/Eigen/Core")
        message(STATUS "eigen: using cached install at ${_eigen_install}")
        add_custom_target(eigen)
    else()
        include(ExternalProject)
        ExternalProject_Add(eigen
            PREFIX ${THIRDPARTS_PREFIX_DIR}/eigen
            URL "https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz"
            URL_HASH SHA256=8586084f71f9bde545ee7fa6d00288b264a2b7ac3607b974e54d13e7162c1c72
            CMAKE_ARGS
                -DCMAKE_INSTALL_PREFIX:PATH=${_eigen_install}
                -DBUILD_TESTING=OFF
                -DEIGEN_BUILD_DOC=OFF
                -DEIGEN_BUILD_PKGCONFIG=OFF
        )
    endif()

    set(eigen_LINK_INC ${_eigen_install}/include/eigen3)
    set(eigen_LINK_DIR "")
    set(eigen_LINK_TAR )
endif()
