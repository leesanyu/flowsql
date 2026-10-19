macro(add_subprojets parent)
	file(GLOB dirs LIST_DIRECTORIES true ${parent}/*)
	foreach(dir ${dirs})
		if(IS_DIRECTORY ${dir} AND NOT ${dir} STREQUAL ${CMAKE_BINARY_DIR})
			if(NOT EXISTS ${dir}/CMakeLists.txt)
				add_subprojets(${dir})
			else()
				add_subdirectory(${dir})
			endif()
		endif()
	endforeach()
endmacro()

# 第三方依赖缓存统一挂在 Git 主工作区根目录下。
# 这样即使从 .worktrees/ 中开发，也会复用主工作区已经完成的缓存，
# 避免每个 worktree 各自触发一套 ExternalProject 下载与编译。
get_filename_component(FLOWSQL_PROJECT_ROOT "${CMAKE_SOURCE_DIR}/.." REALPATH)
set(FLOWSQL_THIRDPARTS_ROOT "${FLOWSQL_PROJECT_ROOT}")

find_program(FLOWSQL_GIT_EXECUTABLE git)
if(FLOWSQL_GIT_EXECUTABLE)
	execute_process(
		COMMAND ${FLOWSQL_GIT_EXECUTABLE} rev-parse --git-common-dir
		WORKING_DIRECTORY "${FLOWSQL_PROJECT_ROOT}"
		RESULT_VARIABLE FLOWSQL_GIT_COMMON_DIR_RET
		OUTPUT_VARIABLE FLOWSQL_GIT_COMMON_DIR_RAW
		ERROR_QUIET
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	if(FLOWSQL_GIT_COMMON_DIR_RET EQUAL 0 AND NOT "${FLOWSQL_GIT_COMMON_DIR_RAW}" STREQUAL "")
		get_filename_component(FLOWSQL_GIT_COMMON_DIR "${FLOWSQL_GIT_COMMON_DIR_RAW}" REALPATH BASE_DIR "${FLOWSQL_PROJECT_ROOT}")
		get_filename_component(FLOWSQL_THIRDPARTS_ROOT "${FLOWSQL_GIT_COMMON_DIR}" DIRECTORY)
	endif()
endif()

message(STATUS "thirdparts: shared cache root at ${FLOWSQL_THIRDPARTS_ROOT}")
set(THIRDPARTS_INSTALL_DIR ${FLOWSQL_THIRDPARTS_ROOT}/.thirdparts_installed)
set(THIRDPARTS_PREFIX_DIR  ${FLOWSQL_THIRDPARTS_ROOT}/.thirdparts_prefix)
set(FLOWSQL_LEGACY_CMAKE_ARGS "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
	set(FLOWSQL_LEGACY_CMAKE_ARGS -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
endif()

macro(add_thirdparts)
	set(THIRDPARTS_DIR ${CMAKE_SOURCE_DIR}/../thirdparts)
	file(GLOB_RECURSE PROJECTS ${THIRDPARTS_DIR}/*.cmake)
	foreach(PROJECT ${PROJECTS})
		include(${PROJECT})
	endforeach()
endmacro()

macro(add_thirddepen TARGET)
	set(LIBRARIES ${ARGV})
	list(REMOVE_AT LIBRARIES 0)
	foreach(LIBRARY ${LIBRARIES})
		if(TARGET ${LIBRARY})
			add_dependencies(${TARGET} ${LIBRARY})
		endif()
		target_include_directories(${TARGET} PUBLIC ${${LIBRARY}_LINK_INC})
		if(NOT "${${LIBRARY}_LINK_DIR}" STREQUAL "")
			target_link_directories(${TARGET} PUBLIC ${${LIBRARY}_LINK_DIR})
		endif()
		target_link_libraries(${TARGET} -ldl -lpthread ${${LIBRARY}_LINK_TAR})
	endforeach()
endmacro()
