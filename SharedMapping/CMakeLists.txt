get_filename_component(SF SourceFiles REALPATH)

if (BUILD_KM)
    include(cmake/sm_kernel.cmake)
endif()

if (BUILD_UM)
    include(cmake/sm_user.cmake)
endif()

if (BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    add_subdirectory(ThirdParty/googletest EXCLUDE_FROM_ALL)
    include(cmake/sm_tests.cmake)
endif()
