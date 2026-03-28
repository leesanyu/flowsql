cmake_minimum_required(VERSION 3.12)
project(flowsql_cpp_operator_sample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT DEFINED FLOWSQL_INCLUDE_DIR)
    message(FATAL_ERROR "FLOWSQL_INCLUDE_DIR is required (e.g. -DFLOWSQL_INCLUDE_DIR=/path/to/flowsql/src)")
endif()

get_filename_component(FLOWSQL_INCLUDE_DIR "${FLOWSQL_INCLUDE_DIR}" ABSOLUTE)
set(FLOWSQL_DATAFRAME_CPP "${FLOWSQL_INCLUDE_DIR}/framework/core/dataframe.cpp")
if(NOT EXISTS "${FLOWSQL_DATAFRAME_CPP}")
    message(FATAL_ERROR "dataframe.cpp not found: ${FLOWSQL_DATAFRAME_CPP}")
endif()

if(NOT DEFINED FLOWSQL_ARROW_INCLUDE_DIR)
    if(EXISTS "/usr/local/lib/python3.12/dist-packages/pyarrow/include/arrow/api.h")
        set(FLOWSQL_ARROW_INCLUDE_DIR "/usr/local/lib/python3.12/dist-packages/pyarrow/include")
    elseif(EXISTS "${FLOWSQL_INCLUDE_DIR}/../.thirdparts_installed/arrow/include/arrow/api.h")
        set(FLOWSQL_ARROW_INCLUDE_DIR "${FLOWSQL_INCLUDE_DIR}/../.thirdparts_installed/arrow/include")
    endif()
endif()

if(NOT DEFINED FLOWSQL_ARROW_LIBRARY)
    if(EXISTS "/usr/local/lib/python3.12/dist-packages/pyarrow/libarrow.so.2200")
        set(FLOWSQL_ARROW_LIBRARY "/usr/local/lib/python3.12/dist-packages/pyarrow/libarrow.so.2200")
    elseif(EXISTS "${FLOWSQL_INCLUDE_DIR}/../.thirdparts_installed/arrow/lib/libarrow.so")
        set(FLOWSQL_ARROW_LIBRARY "${FLOWSQL_INCLUDE_DIR}/../.thirdparts_installed/arrow/lib/libarrow.so")
    endif()
endif()

if(NOT DEFINED FLOWSQL_ARROW_INCLUDE_DIR OR NOT EXISTS "${FLOWSQL_ARROW_INCLUDE_DIR}/arrow/api.h")
    message(FATAL_ERROR "Arrow headers not found. Please pass -DFLOWSQL_ARROW_INCLUDE_DIR=/path/to/arrow/include")
endif()
if(NOT DEFINED FLOWSQL_ARROW_LIBRARY OR NOT EXISTS "${FLOWSQL_ARROW_LIBRARY}")
    message(FATAL_ERROR "Arrow library not found. Please pass -DFLOWSQL_ARROW_LIBRARY=/path/to/libarrow.so")
endif()

add_library(sample_cpp_operator SHARED
    sample_operator.cpp
    plugin_exports.cpp
    ${FLOWSQL_DATAFRAME_CPP}
)

target_include_directories(sample_cpp_operator PRIVATE
    ${FLOWSQL_INCLUDE_DIR}
    ${FLOWSQL_ARROW_INCLUDE_DIR}
)
target_compile_options(sample_cpp_operator PRIVATE -fvisibility=hidden)
target_link_libraries(sample_cpp_operator PRIVATE ${FLOWSQL_ARROW_LIBRARY} dl pthread)

set_target_properties(sample_cpp_operator PROPERTIES
    OUTPUT_NAME sample_cpp_operator
)
