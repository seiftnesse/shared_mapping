cmake_minimum_required(VERSION 3.16)
project(shared_mapping C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/SharedMapping/ThirdParty/FindWDK/cmake")

include(cmake/options.cmake)

if(BUILD_KM)
    find_package(Wdk REQUIRED)
endif()

include(cmake/nice_target_sources.cmake)

add_subdirectory(SharedMapping)
