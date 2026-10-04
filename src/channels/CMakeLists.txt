add_subdirectory(pcapfile)
add_subdirectory(config)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(netadapter)
endif()
