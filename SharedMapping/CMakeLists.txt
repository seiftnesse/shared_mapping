get_filename_component(SF SourceFiles REALPATH)

if (BUILD_KM)
    include(cmake/sm_kernel.cmake)
endif()

if (BUILD_UM)
    include(cmake/sm_user.cmake)
endif()
