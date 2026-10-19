# Ubuntu packages are installed only when missing. No repository or service changes.
option(FLOWSQL_INSTALL_SYSTEM_DEPS "Install missing Ubuntu 24.04 build dependencies with apt" ON)

function(flowsql_require_system_packages)
    file(READ "/etc/os-release" release)
    if(NOT release MATCHES "\n?ID=ubuntu(\n|$)" OR NOT release MATCHES "VERSION_ID=\"24.04\"")
        if(FLOWSQL_INSTALL_SYSTEM_DEPS)
            message(FATAL_ERROR "Automatic dependencies support Ubuntu 24.04. Install dependencies manually and set FLOWSQL_INSTALL_SYSTEM_DEPS=OFF; see docs/building.md")
        endif()
        return()
    endif()

    set(packages ${ARGN})
    find_program(FLOWSQL_DPKG_QUERY_EXECUTABLE dpkg-query REQUIRED)
    set(missing "")
    foreach(package IN LISTS packages)
        execute_process(COMMAND "${FLOWSQL_DPKG_QUERY_EXECUTABLE}" -W "-f=\${Status}" "${package}"
            RESULT_VARIABLE result OUTPUT_VARIABLE status ERROR_QUIET)
        if(NOT result EQUAL 0 OR NOT status STREQUAL "install ok installed")
            list(APPEND missing "${package}")
        endif()
    endforeach()
    if(NOT missing)
        message(STATUS "System build dependencies: installed")
        return()
    endif()
    list(JOIN missing " " display)
    if(NOT FLOWSQL_INSTALL_SYSTEM_DEPS)
        message(FATAL_ERROR "Missing system build packages: ${display}. Install them or enable FLOWSQL_INSTALL_SYSTEM_DEPS.")
    endif()
    find_program(FLOWSQL_APT_EXECUTABLE apt-get REQUIRED)
    find_program(FLOWSQL_ID_EXECUTABLE id REQUIRED)
    execute_process(COMMAND "${FLOWSQL_ID_EXECUTABLE}" -u OUTPUT_VARIABLE uid OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    set(privilege "")
    if(NOT uid STREQUAL "0")
        find_program(FLOWSQL_SUDO_EXECUTABLE sudo REQUIRED)
        execute_process(COMMAND "${FLOWSQL_SUDO_EXECUTABLE}" -n true RESULT_VARIABLE result ERROR_QUIET)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Installing ${display} requires root or non-interactive sudo. Install the listed packages as administrator, then rerun CMake.")
        endif()
        set(privilege "${FLOWSQL_SUDO_EXECUTABLE}" -n)
    endif()
    message(STATUS "Installing missing build packages: ${display}")
    execute_process(COMMAND ${privilege} "${CMAKE_COMMAND}" -E env DEBIAN_FRONTEND=noninteractive
        "${FLOWSQL_APT_EXECUTABLE}" -o APT::Update::Error-Mode=any
        -o Acquire::Retries=1 -o Acquire::http::Timeout=30 update
        RESULT_VARIABLE result TIMEOUT 300)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "apt update failed (${result}); dependencies were not prepared")
    endif()
    execute_process(COMMAND ${privilege} "${CMAKE_COMMAND}" -E env DEBIAN_FRONTEND=noninteractive
        "${FLOWSQL_APT_EXECUTABLE}" -o Acquire::Retries=1 -o Acquire::http::Timeout=30
        install -y --no-install-recommends ${missing} RESULT_VARIABLE result TIMEOUT 900)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "apt install failed (${result}) for: ${display}")
    endif()
    foreach(package IN LISTS missing)
        execute_process(COMMAND "${FLOWSQL_DPKG_QUERY_EXECUTABLE}" -W "-f=\${Status}" "${package}"
            RESULT_VARIABLE result OUTPUT_VARIABLE status ERROR_QUIET)
        if(NOT result EQUAL 0 OR NOT status STREQUAL "install ok installed")
            message(FATAL_ERROR "Package ${package} is still missing after apt install")
        endif()
    endforeach()
endfunction()

function(flowsql_prepare_system_dependencies)
    set(packages build-essential pkg-config patch git curl ca-certificates
        python3-dev python3-pip
        libssl-dev libicu-dev libmysqlclient-dev libpq-dev
        libcurl4-openssl-dev libsasl2-dev libsasl2-modules-gssapi-mit
        libkrb5-dev krb5-user zlib1g-dev liblz4-dev libzstd-dev
        libboost-dev ragel libpcap-dev libxdp-dev libbpf-dev)
    if(FLOWSQL_FLOW_LABELING_MODE STREQUAL "ON" OR
       (FLOWSQL_FLOW_LABELING_MODE STREQUAL "AUTO" AND FLOWSQL_INSTALL_SYSTEM_DEPS))
        list(APPEND packages libdpdk-dev)
    endif()
    flowsql_require_system_packages(${packages})
endfunction()
