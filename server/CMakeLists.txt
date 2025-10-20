# Copyright 2023, Pluto VR, Inc.
#
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.10.2)

if (WIN32)
    set(CMAKE_TOOLCHAIN_FILE "C:/Users/tannh/Env/vcpkg/scripts/buildsystems/vcpkg.cmake")
endif ()

project(electric-maple VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_BUILD_TYPE RelWithDebInfo)

if (WIN32)
    # Fix macro conflict in nanopb.
    add_compile_definitions(PB_NO_STATIC_ASSERT)
endif ()

# Make sure we have pretty colours
option(DISABLE_COLORED_OUTPUT "Always produce ANSI-colored output (GNU/Clang only)." OFF)

if (NOT DISABLE_COLORED_OUTPUT)
    if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        add_compile_options(-fdiagnostics-color=always)
    elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
        add_compile_options(-fcolor-diagnostics)
    endif ()
endif ()

# Get Monado's CMake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/monado/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/monado/cmake/sanitizers")

if (NOT MSVC)
    set(CMAKE_C_FLAGS
            "${CMAKE_C_FLAGS} -pedantic -Wall -Wextra -Wno-unused-parameter -Werror=incompatible-pointer-types"
    )
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-parameter")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--no-undefined")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,--no-undefined")
endif ()

option(EMS_LIBSOUP2 "Use libsoup2.4 instead of libsoup3.0" OFF)

if (UNIX AND NOT ANDROID)
    find_package(PkgConfig REQUIRED)

    pkg_check_modules(GLIB REQUIRED glib-2.0)

    # We use EM_GST instead of GST here, in case it gets overwritten in monado/CMakeLists.txt.
    pkg_check_modules(EM_GST REQUIRED
            gstreamer-1.0
            gstreamer-plugins-base-1.0
            gstreamer-plugins-bad-1.0
            gstreamer-net-1.0
            gstreamer-sdp-1.0
            gstreamer-rtp-1.0
            gstreamer-webrtc-1.0
            gstreamer-app-1.0
            gstreamer-video-1.0
    )

    if (USE_LIBSOUP2)
        pkg_check_modules(LIBSOUP REQUIRED libsoup-2.4)
    else ()
        pkg_check_modules(LIBSOUP REQUIRED libsoup-3.0)
    endif ()

    pkg_check_modules(JSONGLIB REQUIRED json-glib-1.0)
    pkg_check_modules(GIO REQUIRED gio-2.0)
elseif (WIN32)
    # To ensure dll linking, add "C:\Program Files\gstreamer\1.0\msvc_x86_64\bin" to your PATH
    set(GST_ROOT "C:\\Program Files\\gstreamer\\1.0\\msvc_x86_64")
    # For older versions of gstreamer
    #    set(GST_ROOT "C:\\gstreamer\\1.0\\msvc_x86_64")

    set(GST_LIB_ROOT "${GST_ROOT}\\lib")

    set(GST_INCLUDE_DIRS "${GST_ROOT}\\include\\gstreamer-1.0")
    set(EM_GST_LIBRARIES
            "${GST_LIB_ROOT}\\gstreamer-1.0.lib"
            "${GST_LIB_ROOT}\\gstnet-1.0.lib"
            "${GST_LIB_ROOT}\\gstrtp-1.0.lib"
            "${GST_LIB_ROOT}\\gstvideo-1.0.lib"
            "${GST_LIB_ROOT}\\gstsdp-1.0.lib"
            "${GST_LIB_ROOT}\\gstwebrtc-1.0.lib"
            "${GST_LIB_ROOT}\\gstapp-1.0.lib"
    )

    set(GLIB_INCLUDE_DIRS "${GST_ROOT}\\include\\glib-2.0" "${GST_LIB_ROOT}\\glib-2.0\\include")
    set(GLIB_LIBRARIES "${GST_LIB_ROOT}\\gobject-2.0.lib" "${GST_LIB_ROOT}\\glib-2.0.lib")

    set(JSONGLIB_INCLUDE_DIRS "${GST_ROOT}\\include\\json-glib-1.0")
    set(JSONGLIB_LIBRARIES "${GST_LIB_ROOT}\\json-glib-1.0.lib")

    if (USE_LIBSOUP2)
        set(LIBSOUP_INCLUDE_DIRS "${GST_ROOT}\\include\\libsoup-2.4")
        set(LIBSOUP_LIBRARIES "${GST_LIB_ROOT}\\soup-2.4.lib")
    else ()
        set(LIBSOUP_INCLUDE_DIRS "${GST_ROOT}\\include\\libsoup-3.0")
        set(LIBSOUP_LIBRARIES "${GST_LIB_ROOT}\\soup-3.0.lib")
    endif ()

    set(GIO_INCLUDE_DIRS "${GST_ROOT}\\include\\gio-win32-2.0")
    set(GIO_LIBRARIES "${GST_LIB_ROOT}\\gio-2.0.lib")
endif ()

# Default to PIC code
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(MONADO_SRC_XRT ${CMAKE_CURRENT_SOURCE_DIR}/monado/src/xrt)

### Set configuration options for Monado

# Helper to obey CMP0077 and work around warnings.
function(setForce NAME VALUE)
    set(${NAME}
            ${VALUE}
            CACHE INTERNAL "" FORCE
    )
endfunction()

# Let's use this for now: Monado's main compositor is really good and is an
#easy way to see that things are working.
setForce(XRT_FEATURE_COMPOSITOR_MAIN ON)

# No reason to disable this
setForce(XRT_FEATURE_COMPOSITOR_NULL ON)

# It looks like we can use Monado's normal out-of-proc OpenXR instance as-is. We'll see.
setForce(XRT_FEATURE_OPENXR ON)

setForce(XRT_FEATURE_RENDERDOC OFF) # we are not using this
setForce(XRT_FEATURE_STEAMVR_PLUGIN OFF) # we are not using this
setForce(XRT_FEATURE_TRACING OFF) # we are not using this yet
setForce(XRT_FEATURE_SLAM OFF) # we are not using this

setForce(XRT_FEATURE_CLIENT_DEBUG_GUI OFF) # we are not using this

setForce(XRT_MODULE_MONADO_CLI OFF) # we are not using this
setForce(XRT_MODULE_MONADO_GUI OFF) # we are not using this
setForce(XRT_MODULE_MERCURY_HANDTRACKING OFF)

setForce(XRT_HAVE_D3D11 OFF)
setForce(XRT_HAVE_D3D12 OFF)
setForce(XRT_HAVE_OPENCV OFF)

# No drivers!
setForce(XRT_BUILD_DRIVER_ANDROID OFF)
setForce(XRT_BUILD_DRIVER_ARDUINO OFF)
setForce(XRT_BUILD_DRIVER_DAYDREAM OFF)
setForce(XRT_BUILD_DRIVER_DEPTHAI OFF)
setForce(XRT_BUILD_DRIVER_EUROC OFF)
setForce(XRT_BUILD_DRIVER_HANDTRACKING OFF)
setForce(XRT_BUILD_DRIVER_TWRAP OFF)
setForce(XRT_BUILD_DRIVER_HDK OFF)
setForce(XRT_BUILD_DRIVER_HYDRA OFF)
setForce(XRT_BUILD_DRIVER_ILLIXR OFF)
setForce(XRT_BUILD_DRIVER_NS OFF)
setForce(XRT_BUILD_DRIVER_OHMD OFF)
setForce(XRT_BUILD_DRIVER_OPENGLOVES OFF)
setForce(XRT_BUILD_DRIVER_PSMV OFF)
setForce(XRT_BUILD_DRIVER_PSVR OFF)
setForce(XRT_BUILD_DRIVER_QWERTY OFF)
setForce(XRT_BUILD_DRIVER_REALSENSE OFF)
setForce(XRT_BUILD_DRIVER_REMOTE OFF)
setForce(XRT_BUILD_DRIVER_RIFT_S OFF)
setForce(XRT_BUILD_DRIVER_SURVIVE OFF)
setForce(XRT_BUILD_DRIVER_ULV2 OFF)
setForce(XRT_BUILD_DRIVER_VF OFF)
setForce(XRT_BUILD_DRIVER_VIVE OFF)
setForce(XRT_BUILD_DRIVER_WMR OFF)
setForce(XRT_BUILD_DRIVER_SIMULAVR OFF)
setForce(XRT_BUILD_DRIVER_STEAMVR_LIGHTHOUSE OFF)
setForce(XRT_BUILD_DRIVER_XREAL_AIR OFF)
setForce(XRT_BUILD_DRIVER_PSSENSE OFF)
setForce(XRT_BUILD_DRIVER_ROKID OFF)

# There is no clean way to disable the prober/builders (as far as I can see?)
# So we enable one driver so that the legacy builder does not die
setForce(XRT_BUILD_DRIVER_SIMULATED ON)

setForce(XRT_BUILD_SAMPLES OFF)

setForce(BUILD_TESTING OFF)
setForce(BUILD_DOC OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")

set(XRT_OXR_RUNTIME_SUFFIX electricmaple CACHE STRING "OpenXR client library suffix" FORCE)

if (WIN32)
    set(GST_FOUND OFF)
endif ()
add_subdirectory(../monado ${CMAKE_CURRENT_BINARY_DIR}/monado)

add_subdirectory(../proto ${CMAKE_CURRENT_BINARY_DIR}/proto)

add_subdirectory(src)
