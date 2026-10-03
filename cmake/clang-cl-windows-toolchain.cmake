cmake_minimum_required(VERSION 3.16)

if (NOT CMAKE_C_COMPILER)
    find_program(CMAKE_C_COMPILER NAMES clang-cl.exe)
endif ()

if (NOT CMAKE_CXX_COMPILER)
    find_program(CMAKE_CXX_COMPILER NAMES clang-cl.exe)
endif ()

if (CMAKE_GENERATOR_PLATFORM)
    string(TOLOWER "${CMAKE_GENERATOR_PLATFORM}" _WIN_SDK_ARCH)
else ()
    set(_WIN_SDK_ARCH "x64")
endif ()

set(_WIN10_KITS_ROOT "")

# Prefer the environment variable if it is set.
if (DEFINED ENV{WindowsSdkDir} AND NOT "$ENV{WindowsSdkDir}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{WindowsSdkDir}" _WIN10_KITS_ROOT)
endif ()

# Fall back to the registry.
if (_WIN10_KITS_ROOT STREQUAL "")
    get_filename_component(_WIN10_KITS_ROOT
            "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]"
            ABSOLUTE
    )
endif ()
if (_WIN10_KITS_ROOT STREQUAL "" OR _WIN10_KITS_ROOT STREQUAL "/registry")
    get_filename_component(_WIN10_KITS_ROOT
            "[HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]"
            ABSOLUTE
    )
endif ()

if (_WIN10_KITS_ROOT STREQUAL "" OR _WIN10_KITS_ROOT STREQUAL "/registry")
    message(FATAL_ERROR
            "Unable to locate the Windows 10/11 SDK. "
            "Install the SDK, launch from a VS Developer Shell, or set the WindowsSdkDir environment variable."
    )
endif ()

file(GLOB _WIN10_SDK_VERSIONS
        LIST_DIRECTORIES TRUE
        RELATIVE "${_WIN10_KITS_ROOT}/bin"
        "${_WIN10_KITS_ROOT}/bin/10.*"
)

if (NOT _WIN10_SDK_VERSIONS)
    message(FATAL_ERROR
            "No Windows SDK versions found under ${_WIN10_KITS_ROOT}/bin"
    )
endif ()

list(SORT _WIN10_SDK_VERSIONS)
list(GET _WIN10_SDK_VERSIONS -1 _WIN10_SDK_VERSION)

set(_WIN10_SDK_BIN "${_WIN10_KITS_ROOT}/bin/${_WIN10_SDK_VERSION}/${_WIN_SDK_ARCH}")
if (NOT EXISTS "${_WIN10_SDK_BIN}")
    message(FATAL_ERROR
            "Windows SDK architecture folder does not exist: ${_WIN10_SDK_BIN}"
    )
endif ()

# Find mt.exe and rc.exe
find_program(CMAKE_MT NAMES mt.exe
        HINTS "${_WIN10_SDK_BIN}"
        NO_DEFAULT_PATH
)
if (NOT CMAKE_MT)
    message(FATAL_ERROR "mt.exe not found in ${_WIN10_SDK_BIN}")
endif ()
set(CMAKE_MT "${CMAKE_MT}" CACHE FILEPATH "Windows manifest tool" FORCE)

find_program(CMAKE_RC_COMPILER NAMES rc.exe
        HINTS "${_WIN10_SDK_BIN}"
        NO_DEFAULT_PATH
)
if (NOT CMAKE_RC_COMPILER)
    message(FATAL_ERROR "rc.exe not found in ${_WIN10_SDK_BIN}")
endif ()
set(CMAKE_RC_COMPILER "${CMAKE_RC_COMPILER}" CACHE FILEPATH "Windows resource compiler" FORCE)

message(STATUS "Windows SDK root:    ${_WIN10_KITS_ROOT}")
message(STATUS "Windows SDK version: ${_WIN10_SDK_VERSION}")
message(STATUS "Windows SDK arch:    ${_WIN_SDK_ARCH}")
message(STATUS "Manifest tool:       ${CMAKE_MT}")
message(STATUS "Resource compiler:   ${CMAKE_RC_COMPILER}")
