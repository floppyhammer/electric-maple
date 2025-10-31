# Copyright 2023, Pluto VR, Inc.
#
# SPDX-License-Identifier: BSL-1.0

add_library(
        electric_maple SHARED
        em_connection.c
        em_remote_experience.cpp
        em_stream_client.c
        render/GLDebug.cpp
        render/GLError.cpp
        render/GLSwapchain.cpp
        render/render.cpp
        render/em_egl_context.c
)
target_link_libraries(
        electric_maple
        PRIVATE em_proto aux_util ${ANDROID_LOG_LIBRARY}
        PUBLIC
        OpenXR::openxr_loader # actually only need headers but prefab doesn't expose that
        EGL::EGL
        OpenGLES::OpenGLESv3
        xrt-interfaces
        aux-includes
        gstreamer_android
        em_egl
)
target_compile_definitions(
        electric_maple PUBLIC XR_USE_PLATFORM_ANDROID XR_USE_GRAPHICS_API_OPENGL_ES
        XR_USE_TIMESPEC
)
target_include_directories(
        electric_maple
        PRIVATE ${LIBSOUP_INCLUDE_DIRS} ${JSONGLIB_INCLUDE_DIRS} ${GIO_INCLUDE_DIRS}
        PUBLIC .. ${GST_INCLUDE_DIRS} ${GLIB_INCLUDE_DIRS}
)
