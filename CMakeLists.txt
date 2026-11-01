cmake_minimum_required(VERSION 3.21)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(Uranium VERSION 0.1.0)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK. Set GEODE_SDK to your Geode SDK directory.")
else()
    message(STATUS "Found Geode SDK: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# gdsolver upstream currently supports the raw Geometry Dash offsets and packed DP
# state layout on Windows and Android64. Android32 has a different pointer/alignment
# layout and is intentionally excluded from this in-process solver integration.
set(URANIUM_GDSOLVER_SUPPORTED TRUE)
if (ANDROID AND ANDROID_ABI STREQUAL "armeabi-v7a")
    set(URANIUM_GDSOLVER_SUPPORTED FALSE)
    message(STATUS "Building Android32 without gdsolver: upstream does not support its offsets/state layout")
endif()

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
if (NOT URANIUM_GDSOLVER_SUPPORTED)
    list(FILTER SOURCES EXCLUDE REGEX "src/gdsolver_bridge\\.cpp$")
endif()

add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (URANIUM_GDSOLVER_SUPPORTED)
    # The CLI/test targets stay disabled; Uranium links the in-process DP core.
    set(GDSOLVER_BUILD_DP OFF CACHE BOOL "" FORCE)
    if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gdsolver/dp/CMakeLists.txt")
        message(FATAL_ERROR "gdsolver submodule is missing. Run: git submodule update --init --recursive")
    endif()
    add_subdirectory(third_party/gdsolver/dp ${CMAKE_CURRENT_BINARY_DIR}/gdsolver-dp EXCLUDE_FROM_ALL)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gdsolver/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gdsolver/dp/src
    )
    # Geode's setup_geode_mod() uses the plain target_link_libraries signature.
    target_link_libraries(${PROJECT_NAME} gdsolver_dpcore)
endif()

setup_geode_mod(${PROJECT_NAME})
