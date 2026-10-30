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

# gdsolver's plain C++17 reachability-DP core. The upstream CLI/test targets
# stay disabled; Uranium links only the same static core used by leveldp.
set(GDSOLVER_BUILD_DP OFF CACHE BOOL "" FORCE)
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gdsolver/dp/CMakeLists.txt")
    message(FATAL_ERROR "gdsolver submodule is missing. Run: git submodule update --init --recursive")
endif()
add_subdirectory(third_party/gdsolver/dp ${CMAKE_CURRENT_BINARY_DIR}/gdsolver-dp EXCLUDE_FROM_ALL)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gdsolver/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gdsolver/dp/src
)
target_link_libraries(${PROJECT_NAME} PRIVATE gdsolver_dpcore)

setup_geode_mod(${PROJECT_NAME})
