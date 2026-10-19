find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
get_filename_component(FLOWSQL_VERSION_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(FLOWSQL_VERSION_TOOL "${FLOWSQL_VERSION_ROOT}/tools/product_version.py")
set(FLOWSQL_VERSION_DIR "${CMAKE_BINARY_DIR}/generated/version")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FLOWSQL_VERSION_ROOT}/VERSION")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${FLOWSQL_VERSION_TOOL}"
    --root "${FLOWSQL_VERSION_ROOT}" --numeric OUTPUT_VARIABLE FLOWSQL_NUMERIC_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${FLOWSQL_VERSION_TOOL}"
    --root "${FLOWSQL_VERSION_ROOT}" --output "${FLOWSQL_VERSION_DIR}"
    OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
include_directories("${FLOWSQL_VERSION_DIR}")

add_custom_target(flowsql_version
    COMMAND "${Python3_EXECUTABLE}" "${FLOWSQL_VERSION_TOOL}"
        --root "${FLOWSQL_VERSION_ROOT}" --output "${FLOWSQL_VERSION_DIR}"
    BYPRODUCTS "${FLOWSQL_VERSION_DIR}/flowsql_build_info.h" "${FLOWSQL_VERSION_DIR}/build-info.json"
    COMMENT "Refreshing product build identity"
    VERBATIM)

# Stamp every product shared library without exporting a symbol or changing its ABI.
file(GENERATE OUTPUT "${FLOWSQL_VERSION_DIR}/build_record.cpp" CONTENT
    "#include \"flowsql_build_info.h\"\n[[gnu::used]] static const char record[] = FLOWSQL_PRODUCT_RECORD;\n")
function(flowsql_stamp_product_libraries directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(kind ${target} TYPE)
        if(kind STREQUAL "SHARED_LIBRARY" AND target MATCHES "^flowsql")
            target_sources(${target} PRIVATE "${FLOWSQL_VERSION_DIR}/build_record.cpp")
            add_dependencies(${target} flowsql_version)
        endif()
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        flowsql_stamp_product_libraries("${child}")
    endforeach()
endfunction()
