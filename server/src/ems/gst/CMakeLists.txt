# Copyright 2023, Pluto VR, Inc.
#
# SPDX-License-Identifier: BSL-1.0

add_library(ems_gst STATIC ems_gstreamer_pipeline.c ems_signaling_server.c ems_gstreamer_src.c)

target_link_libraries(
        ems_gst
        PRIVATE
        ems_build_defines
        ems_callbacks
        em_proto
        aux_util
        ${GST_LIBRARIES}
        ${GST_SDP_LIBRARIES}
        ${GST_RTP_LIBRARIES}
        ${GST_WEBRTC_LIBRARIES}
        ${GST_VIDEO_LIBRARIES}
        ${GST_APP_LIBRARIES}
        ${GLIB_LIBRARIES}
        ${LIBSOUP_LIBRARIES}
        ${JSONGLIB_LIBRARIES}
        ${GIO_LIBRARIES}
)

target_include_directories(
        ems_gst
        PRIVATE
        ${GLIB_INCLUDE_DIRS}
        ${GST_INCLUDE_DIRS}
        ${LIBSOUP_INCLUDE_DIRS}
        ${JSONGLIB_INCLUDE_DIRS}
        ${GIO_INCLUDE_DIRS}
)
target_compile_definitions(ems_gst PUBLIC G_LOG_DOMAIN="ElectricMapleServer")
