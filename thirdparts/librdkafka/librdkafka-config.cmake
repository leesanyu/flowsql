option(FLOWSQL_KAFKA_OFFLINE "Never download Kafka source; require a matching cache or local archive" OFF)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(KAFKA_SYSTEM REQUIRED IMPORTED_TARGET libcurl libsasl2 krb5-gssapi zlib liblz4 libzstd)
find_program(KAFKA_PATCH_EXECUTABLE patch REQUIRED)

set(_kafka_version 2.16.0)
set(_kafka_sha256 e6b61de61d3282879a88e4ee3d9f634a8b05bf78a9198dfc25c4820c0c9ed231)
set(_kafka_recipe ${CMAKE_CURRENT_LIST_DIR})
file(SHA256 "${_kafka_recipe}/gssapi-session-cache.patch" _kafka_patch_sha)
file(SHA256 "${_kafka_recipe}/dependencies.json" _kafka_manifest_sha)
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _kafka_recipe_sha)
file(SHA256 "${_kafka_recipe}/build-client.cmake" _kafka_builder_sha)
string(SHA256 _kafka_fingerprint
    "${_kafka_sha256};${_kafka_patch_sha};${_kafka_manifest_sha};${_kafka_recipe_sha};${_kafka_builder_sha};${CMAKE_SYSTEM_NAME};${CMAKE_SYSTEM_PROCESSOR};${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${OPENSSL_VERSION};${KAFKA_SYSTEM_VERSION};${KAFKA_SYSTEM_libcurl_VERSION};${KAFKA_SYSTEM_libsasl2_VERSION};${KAFKA_SYSTEM_krb5-gssapi_VERSION};${KAFKA_SYSTEM_zlib_VERSION};${KAFKA_SYSTEM_liblz4_VERSION};${KAFKA_SYSTEM_libzstd_VERSION}")
set(_kafka_install "${THIRDPARTS_INSTALL_DIR}/librdkafka/${_kafka_fingerprint}")
set(_kafka_prefix "${THIRDPARTS_PREFIX_DIR}/librdkafka/${_kafka_fingerprint}")
set(_kafka_archive "${THIRDPARTS_PREFIX_DIR}/librdkafka/librdkafka-${_kafka_version}.tar.gz")

if(EXISTS "${_kafka_install}/lib/librdkafka.a" AND EXISTS "${_kafka_install}/flowsql-fingerprint")
    file(READ "${_kafka_install}/flowsql-fingerprint" _kafka_cached)
    if(NOT _kafka_cached STREQUAL _kafka_fingerprint)
        message(FATAL_ERROR "Kafka cache fingerprint mismatch; explicitly rebuild its installation")
    endif()
    add_custom_target(librdkafka)
    message(STATUS "Kafka: reusing verified cache ${_kafka_install}")
else()
    if(FLOWSQL_KAFKA_OFFLINE AND NOT EXISTS "${_kafka_archive}")
        message(FATAL_ERROR "Kafka offline build requires a matching installed cache or local source archive")
    endif()
    file(MAKE_DIRECTORY "${_kafka_install}/include")
    add_custom_command(OUTPUT "${_kafka_install}/flowsql-fingerprint" "${_kafka_install}/lib/librdkafka.a"
        COMMAND ${CMAKE_COMMAND}
            "-DPREFIX=${_kafka_prefix}" "-DINSTALL=${_kafka_install}" "-DFINGERPRINT=${_kafka_fingerprint}"
            "-DARCHIVE=${_kafka_archive}" "-DURL=https://codeload.github.com/confluentinc/librdkafka/tar.gz/refs/tags/v${_kafka_version}"
            "-DSHA256=${_kafka_sha256}" "-DVERSION=${_kafka_version}" "-DPATCH=${KAFKA_PATCH_EXECUTABLE}"
            "-DRECIPE=${_kafka_recipe}" "-DOFFLINE=${FLOWSQL_KAFKA_OFFLINE}"
            "-DC_COMPILER=${CMAKE_C_COMPILER}" "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
            -P "${_kafka_recipe}/build-client.cmake"
        DEPENDS "${_kafka_recipe}/build-client.cmake" "${_kafka_recipe}/gssapi-session-cache.patch"
        VERBATIM
    )
    add_custom_target(librdkafka DEPENDS "${_kafka_install}/flowsql-fingerprint" "${_kafka_install}/lib/librdkafka.a")
endif()

add_library(FlowSQL::KafkaClient STATIC IMPORTED GLOBAL)
set_target_properties(FlowSQL::KafkaClient PROPERTIES
    IMPORTED_LOCATION "${_kafka_install}/lib/librdkafka.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_kafka_install}/include"
    INTERFACE_LINK_LIBRARIES "OpenSSL::SSL;OpenSSL::Crypto;PkgConfig::KAFKA_SYSTEM;Threads::Threads;${CMAKE_DL_LIBS};m"
)
add_dependencies(FlowSQL::KafkaClient librdkafka)
set(FLOWSQL_KAFKA_CLIENT_VERSION "${_kafka_version}")
set(FLOWSQL_KAFKA_CLIENT_FINGERPRINT "${_kafka_fingerprint}")
