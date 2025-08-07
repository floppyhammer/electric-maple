// Copyright 2019-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  An @ref xrt_frame_sink that does gst things.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Aaron Boxer <aaron.boxer@collabora.com>
 * @author Moses Turner <moses@collabora.com>
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * @ingroup aux_util
 */

#include "ems_gstreamer_src.h"

#include <assert.h>

#include "ems_gstreamer.h"
#include "gst/app/gstappsink.h"
#include "gst/app/gstappsrc.h"
#include "gst/video/gstvideometa.h"
#include "gst/video/video-format.h"
#include "util/u_debug.h"
#include "util/u_format.h"
#include "util/u_misc.h"
#include "util/u_trace_marker.h"

/*
 *
 * Internal sink functions.
 *
 */

static void wrapped_buffer_destroy(gpointer data) {
    struct xrt_frame *xf = (struct xrt_frame *)data;

    U_LOG_T("Called");

    xrt_frame_reference(&xf, NULL);
}

static GstVideoFormat gst_fmt_from_xf_format(enum xrt_format format_in) {
    switch (format_in) {
        case XRT_FORMAT_R8G8B8:
            return GST_VIDEO_FORMAT_RGB;
        case XRT_FORMAT_R8G8B8A8:
            return GST_VIDEO_FORMAT_RGBA;
        case XRT_FORMAT_R8G8B8X8:
            return GST_VIDEO_FORMAT_RGBx;
        case XRT_FORMAT_YUYV422:
            return GST_VIDEO_FORMAT_YUY2;
        case XRT_FORMAT_L8:
            return GST_VIDEO_FORMAT_GRAY8;
        default:
            assert(false);
            return GST_VIDEO_FORMAT_UNKNOWN;
    }
}

static void complain_if_wrong_image_size(struct xrt_frame *xf) {
    // libx264 is the actual source of this requirement; it refuses to handle odd widths/heights when encoding I420
    // subsampled content. OpenH264 should work, but it's easy enough to just force all users of this code to
    // provide normal-sized inputs.
    if (xf->width % 2 == 1) {
        U_LOG_W("Image width needs to be divisible by 2!");
    }
    if (xf->height % 2 == 1) {
        U_LOG_W("Image height needs to be divisible by 2!");
    }
}

void ems_gstreamer_src_push_frame(struct ems_gstreamer_src *gs, struct xrt_frame *xf, GBytes *downMsg_bytes) {
    SINK_TRACE_MARKER();

    complain_if_wrong_image_size(xf);

    GstBuffer *buffer;
    GstFlowReturn ret;

    U_LOG_T(
        "Called"
        "\n\tformat: %s"
        "\n\twidth: %u"
        "\n\theight: %u",
        u_format_str(xf->format),
        xf->width,
        xf->height);

    /* We need to take a reference on the frame to keep it alive. */
    struct xrt_frame *taken = NULL;
    xrt_frame_reference(&taken, xf);

    /* Wrap the frame that we now hold a reference to. */
    buffer = gst_buffer_new_wrapped_full( //
        0,                                // GstMemoryFlags flags
        (gpointer)xf->data,               // gpointer data
        taken->size,                      // gsize maxsize
        0,                                // gsize offset
        taken->size,                      // gsize size
        taken,                            // gpointer user_data
        wrapped_buffer_destroy);          // GDestroyNotify notify

    int stride = xf->stride;

    gsize offsets[4] = {0, 0, 0, 0};
    gint strides[4] = {stride, 0, 0, 0};
    gst_buffer_add_video_meta_full(buffer,
                                   GST_VIDEO_FRAME_FLAG_NONE,
                                   gst_fmt_from_xf_format(xf->format),
                                   xf->width,
                                   xf->height,
                                   1,
                                   offsets,
                                   strides);

    //! Get the timestampe from the frame.
    uint64_t xtimestamp_ns = xf->timestamp;

    // Use the first frame as offset.
    if (gs->offset_ns == 0) {
        gs->offset_ns = xtimestamp_ns;
    }

    // Need to be offset or gstreamer becomes sad.
    GST_BUFFER_PTS(buffer) = xtimestamp_ns - gs->offset_ns;

    // Duration is measured from last time stamp.
    GST_BUFFER_DURATION(buffer) = xtimestamp_ns - gs->timestamp_ns;
    gs->timestamp_ns = xtimestamp_ns;

    {
        size_t payload_size;
        gconstpointer payload_ptr = g_bytes_get_data(downMsg_bytes, &payload_size);

        // Repack the protobuf into a GstBuffer
        GstBuffer *struct_buf = gst_buffer_new_memdup(payload_ptr, payload_size);
        if (!struct_buf) {
            U_LOG_E("Failed to allocate GstBuffer with payload.");
            return;
        }

        // Add it to a custom meta
        GstCustomMeta *custom_meta = gst_buffer_add_custom_meta(buffer, "down-message");
        if (custom_meta == NULL) {
            U_LOG_E("Failed to add GstCustomMeta");
            gst_buffer_unref(struct_buf);
            return;
        }
        GstStructure *custom_structure = gst_custom_meta_get_structure(custom_meta);
        gst_structure_set(custom_structure, "protobuf", GST_TYPE_BUFFER, struct_buf, NULL);

        gst_buffer_unref(struct_buf);
    }

    // All done, send it to the gstreamer pipeline.
    ret = gst_app_src_push_buffer((GstAppSrc *)gs->appsrc, buffer);
    if (ret != GST_FLOW_OK) {
        U_LOG_E("Got GST error '%i'", ret);
    }
}

static void enough_data(GstElement *appsrc, gpointer udata) {
    // Debugging code.
    U_LOG_T("Called");
}

static void break_apart(struct xrt_frame_node *node) {
    struct ems_gstreamer_src *gs = container_of(node, struct ems_gstreamer_src, node);

    /*
     * This function is called when we are shutting down, after returning
     * from this function you are not allowed to call any other nodes in the
     * graph. But it must be safe for other nodes to call any normal
     * functions on us. Once the context is done calling break_aprt on all
     * objects it will call destroy on them.
     */

    (void)gs;
}

static void destroy(struct xrt_frame_node *node) {
    struct ems_gstreamer_src *gs = container_of(node, struct ems_gstreamer_src, node);

    /*
     * All of the nodes has been broken apart and none of our functions will
     * be called, it's now safe to destroy and free ourselves.
     */

    free(gs);
}

/*
 *
 * Exported functions.
 *
 */

void ems_gstreamer_src_create_with_pipeline(struct gstreamer_pipeline *gp,
                                            uint32_t width,
                                            uint32_t height,
                                            enum xrt_format format,
                                            const char *appsrc_name,
                                            struct ems_gstreamer_src **out_gs,
                                            struct xrt_frame_sink **out_xfs) {
    const char *format_str = NULL;
    switch (format) {
        case XRT_FORMAT_R8G8B8:
            format_str = "RGB";
            break;
        case XRT_FORMAT_R8G8B8A8:
            format_str = "RGBA";
            break;
        case XRT_FORMAT_R8G8B8X8:
            format_str = "RGBx";
            break;
        case XRT_FORMAT_YUYV422:
            format_str = "YUY2";
            break;
        case XRT_FORMAT_L8:
            format_str = "GRAY8";
            break;
        default:
            assert(false);
            break;
    }

    const gchar *tags[] = {NULL};
    const GstMetaInfo *info = gst_meta_register_custom("down-message", tags, NULL, NULL, NULL);
    if (info == NULL) {
        U_LOG_E("Failed to register custom meta 'down-message'.");
    }

    struct ems_gstreamer_src *gs = U_TYPED_CALLOC(struct ems_gstreamer_src);
    // gs->base.push_frame = push_frame;
    gs->node.break_apart = break_apart;
    gs->node.destroy = destroy;
    gs->gp = gp;
    gs->appsrc = gst_bin_get_by_name(GST_BIN(gp->pipeline), appsrc_name);

    GstCaps *caps = gst_caps_new_simple( //
        "video/x-raw",                   //
        "format",
        G_TYPE_STRING,
        format_str, //
        "width",
        G_TYPE_INT,
        width, //
        "height",
        G_TYPE_INT,
        height, //
        "framerate",
        GST_TYPE_FRACTION,
        0,
        1, //
        NULL);

    g_object_set(G_OBJECT(gs->appsrc), //
                 "caps",
                 caps, //
                 "stream-type",
                 GST_APP_STREAM_TYPE_STREAM, //
                 "format",
                 GST_FORMAT_TIME, //
                 "is-live",
                 TRUE, //
                 NULL);

    g_signal_connect(G_OBJECT(gs->appsrc), "enough-data", G_CALLBACK(enough_data), gs);

    /*
     * Add ourselves to the context so we are destroyed.
     * This is done once we know everything is completed.
     */
    xrt_frame_context_add(gp->xfctx, &gs->node);

    *out_gs = gs;
    *out_xfs = &gs->base;
}
