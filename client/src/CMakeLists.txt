# Copyright 2023, Pluto VR, Inc.
#
# SPDX-License-Identifier: BSL-1.0

# build native_app_glue as a static lib
add_library(
        native_app_glue STATIC
        ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

# now build app's shared lib
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")

add_library(
        electric_maple_client SHARED
        main.cpp
        em/em_connection.c
        em/em_remote_experience.cpp
        em/em_stream_client.c
        em/render/gl_debug.cpp
        em/render/gl_error.cpp
        em/render/gl_swapchain.cpp
        em/render/render.cpp
        em/render/em_egl_context.c
)

target_include_directories(
        electric_maple_client
        PRIVATE
        .
        ../../common/
        ${GLIB_INCLUDE_DIRS}
        ${GST_INCLUDE_DIRS}
        ${GIO_INCLUDE_DIRS}
        ${LIBSOUP_INCLUDE_DIRS}
        ${JSONGLIB_INCLUDE_DIRS}
)

target_compile_definitions(
        electric_maple_client PUBLIC XR_USE_PLATFORM_ANDROID XR_USE_GRAPHICS_API_OPENGL_ES
        XR_USE_TIMESPEC
)

# add lib dependencies
target_link_libraries(
        electric_maple_client
        PRIVATE
        em_proto
        aux_util
        gstreamer_android
        native_app_glue
        OpenXR::openxr_loader
        EGL::EGL
        OpenGLES::OpenGLESv3
        ${ANDROID_LIBRARY}
        ${ANDROID_LOG_LIBRARY}
)

# Export ANativeActivity_onCreate()
set_property(
        TARGET electric_maple_client
        APPEND_STRING
        PROPERTY LINK_FLAGS " -u ANativeActivity_onCreate"
)
