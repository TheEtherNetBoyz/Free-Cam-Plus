cmake_minimum_required(VERSION 3.25)
project(secondary_camera_mod CXX)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(DUSK_DIR "" CACHE PATH "Path to a Dusklight source checkout")
    if (DUSK_DIR STREQUAL "")
        message(FATAL_ERROR
            "DUSK_DIR is required for standalone builds. Set it to your Dusklight checkout.")
    endif ()
    option(DUSK_MOD_USE_FULL_TREE "Use full build instead of the minimal mod SDK" OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    if (DUSK_MOD_USE_FULL_TREE)
        add_subdirectory("${DUSK_DIR}" dusk EXCLUDE_FROM_ALL)
    else ()
        add_subdirectory("${DUSK_DIR}/sdk" dusk-sdk EXCLUDE_FROM_ALL)
    endif ()
endif ()

add_mod(secondary_camera
    FEATURES game webgpu
    SOURCES src/mod.cpp
    MOD_JSON mod.json
    BUNDLE
)

if(WIN32)
    target_link_libraries(secondary_camera PRIVATE user32)
endif()
