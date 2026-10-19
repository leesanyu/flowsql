# PyArrow supplies the exact C++ headers/libraries used by the managed Worker.
# product_runtime.cmake prepares this interpreter on clean hosts as well.
set(_arrow_dir "${FLOWSQL_PYARROW_DIR}")
foreach(required include/arrow/api.h libarrow.so.2200 libarrow_compute.so.2200)
    if(NOT EXISTS "${_arrow_dir}/${required}")
        message(FATAL_ERROR "PyArrow 22.0.0 C++ dependency missing: ${_arrow_dir}/${required}")
    endif()
endforeach()
add_custom_target(arrow)
set(arrow_LINK_INC "${_arrow_dir}/include")
set(arrow_LINK_DIR "${_arrow_dir}")
set(arrow_LINK_TAR "${_arrow_dir}/libarrow_compute.so.2200" "${_arrow_dir}/libarrow.so.2200")
