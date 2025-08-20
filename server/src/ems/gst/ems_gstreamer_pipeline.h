// Copyright 2019-2023, Collabora, Ltd.
// Copyright 2023, Pluto VR, Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A GStreamer pipeline for WebRTC streaming
 * @author Moshi Turner <moses@collabora.com>
 * @ingroup aux_util
 */

#pragma once

#include <glib.h>

#include "ems_gstreamer.h"
#include "util/u_debug.h"
#include "util/u_misc.h"

#ifdef __cplusplus
extern "C" {
#endif
struct gstreamer_pipeline;

struct ems_callbacks;

typedef struct _em_proto_DownMessage em_proto_DownMessage;

GBytes *
ems_gstreamer_pipeline_encode_down_msg(em_proto_DownMessage *msg);

void
ems_gstreamer_pipeline_play(struct gstreamer_pipeline *gp);

void
ems_gstreamer_pipeline_stop(struct gstreamer_pipeline *gp);

void
ems_gstreamer_pipeline_create(struct xrt_frame_context *xfctx,
                              const char *appsrc_name,
                              struct ems_callbacks *callbacks_collection,
                              struct gstreamer_pipeline **out_gp);

uint64_t
ems_gstreamer_pipeline_get_current_time(struct gstreamer_pipeline *gp);

void
ems_gstreamer_pipeline_adjust_bitrate(struct gstreamer_pipeline *gp, int target_bitrate);

#ifdef __cplusplus
}
#endif
