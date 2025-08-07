// Copyright 2019-2023, Collabora, Ltd.
// Copyright 2023, Pluto VR, Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A GStreamer pipeline for WebRTC streaming
 * @author Moshi Turner <moses@collabora.com>
 * @author Jakub Adam <jakub.adam@collabora.com
 * @author Nicolas Dufresne <nicolas.dufresne@collabora.com>
 * @author Olivier Crête <olivier.crete@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Aaron Boxer <aaron.boxer@collabora.com>
 * @author Rylie Pavlik <rpavlik@collabora.com>
 * @ingroup aux_util
 */

#include "ems_gstreamer_pipeline.h"

#include "electricmaple.pb.h"
#include "ems_callbacks.h"
#include "include/ems_common.h"
#include "os/os_threading.h"
#include "pb_decode.h"
#include "util/u_debug.h"
#include "util/u_misc.h"

// Monado includes
#include <glib-unix.h>
#include <gst/gst.h>
#include <gst/gststructure.h>
#include <gst/rtp/gstrtpbuffer.h>

#include "ems_signaling_server.h"
#include "gstreamer/gst_internal.h"
#include "gstreamer/gst_pipeline.h"

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/datachannel.h>
#include <gst/webrtc/rtcsessiondescription.h>
#undef GST_USE_UNSTABLE_API

#include <assert.h>
#include <stdio.h>

#include "pb_encode.h"

#define WEBRTC_TEE_NAME "webrtctee"

#ifdef ANDROID
    #define DEFAULT_BITRATE "40960000"
#else
    #define DEFAULT_BITRATE "8000"
#endif

// TODO: Can we define the below at a higher level so it can also be
//       picked-up by em_stream_client ?
#define RTP_TWOBYTES_HDR_EXT_ID 1 // Must be in the [1,15] range
#define RTP_TWOBYTES_HDR_EXT_MAX_SIZE 255

EmsSignalingServer *signaling_server = NULL;

struct ems_gstreamer_pipeline {
    struct gstreamer_pipeline base;

    GstElement *webrtc;

    GObject *data_channel;
    guint timeout_src_id;
    guint timeout_src_id_dot_data;

    GBytes *downMsg_bytes;

    struct ems_callbacks *callbacks;
};

static gboolean sigint_handler(gpointer user_data) {
    g_main_loop_quit(user_data);
    return G_SOURCE_REMOVE;
}

static gboolean gst_bus_cb(GstBus *bus, GstMessage *message, gpointer user_data) {
    struct ems_gstreamer_pipeline *egp = (struct ems_gstreamer_pipeline *)user_data;
    GstBin *pipeline = GST_BIN(egp->base.pipeline);

    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError *gerr;
            gchar *debug_msg;
            gst_message_parse_error(message, &gerr, &debug_msg);
            GST_DEBUG_BIN_TO_DOT_FILE(pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "mss-pipeline-ERROR");
            g_error("Error: %s (%s)", gerr->message, debug_msg);
            g_error_free(gerr);
            g_free(debug_msg);
        } break;
        case GST_MESSAGE_WARNING: {
            GError *gerr;
            gchar *debug_msg;
            gst_message_parse_warning(message, &gerr, &debug_msg);
            GST_DEBUG_BIN_TO_DOT_FILE(pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "mss-pipeline-WARNING");
            g_warning("Warning: %s (%s)", gerr->message, debug_msg);
            g_error_free(gerr);
            g_free(debug_msg);
        } break;
        case GST_MESSAGE_EOS: {
            g_error("Got EOS!!");
        } break;
        case GST_MESSAGE_LATENCY: {
            g_warning("Handling latency");
            gst_bin_recalculate_latency(pipeline);
        } break;
        default:
            break;
    }
    return TRUE;
}

static void on_webrtcbin_get_stats(GstPromise *promise, GstElement *webrtcbin) {
    g_return_if_fail(gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED);

    const GstStructure *stats = gst_promise_get_reply(promise);

    gchar *stats_str = gst_structure_to_string(stats);
    U_LOG_I("webrtcbin stats: %s", stats_str);

    g_free(stats_str);
}

static gboolean webrtcbin_get_stats(GstElement *webrtcbin) {
    GstPromise *promise =
        gst_promise_new_with_change_func((GstPromiseChangeFunc)on_webrtcbin_get_stats, webrtcbin, NULL);

    g_signal_emit_by_name(webrtcbin, "get-stats", NULL, promise);
    gst_promise_unref(promise);

    return G_SOURCE_REMOVE;
}

static GstElement *get_webrtcbin_for_client(GstBin *pipeline, EmsClientId client_id) {
    gchar *name = g_strdup_printf("webrtcbin_%p", client_id);
    GstElement *webrtcbin = gst_bin_get_by_name(pipeline, name);
    g_free(name);

    return webrtcbin;
}

static void connect_webrtc_to_tee(GstElement *webrtcbin) {
    GstElement *pipeline = GST_ELEMENT(gst_element_get_parent(webrtcbin));
    if (pipeline == NULL) {
        return;
    }

    GstElement *tee = gst_bin_get_by_name(GST_BIN(pipeline), WEBRTC_TEE_NAME);
    GstPad *src_pad = gst_element_request_pad_simple(tee, "src_%u");

    GstPadTemplate *pad_template = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(webrtcbin), "sink_%u");

    // GstCaps *caps = gst_caps_from_string("application/x-rtp,encoding-name=VP8,media=video,payload=96");
    GstCaps *caps = gst_caps_from_string(
        "application/x-rtp, "
        "payload=96,encoding-name=H264,clock-rate=90000,media=video,packetization-mode=(string)1");

    GstPad *sink_pad = gst_element_request_pad(webrtcbin, pad_template, "sink_0", caps);

    GstPadLinkReturn ret = gst_pad_link(src_pad, sink_pad);
    g_assert(ret == GST_PAD_LINK_OK);

    {
        GArray *transceivers;
        g_signal_emit_by_name(webrtcbin, "get-transceivers", &transceivers);
        g_assert(transceivers != NULL && transceivers->len == 1);
        GstWebRTCRTPTransceiver *trans = g_array_index(transceivers, GstWebRTCRTPTransceiver *, 0);
        g_object_set(trans, "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, NULL);
        g_object_set(trans, "fec-type", GST_WEBRTC_FEC_TYPE_ULP_RED, "fec-percentage", 5, NULL);
        g_array_unref(transceivers);
    }

    gst_caps_unref(caps);
    gst_object_unref(src_pad);
    gst_object_unref(sink_pad);
    gst_object_unref(tee);
    gst_object_unref(pipeline);
}

static void on_offer_created(GstPromise *promise, GstElement *webrtcbin) {
    GstWebRTCSessionDescription *offer = NULL;

    gst_structure_get(gst_promise_get_reply(promise), "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);

    g_signal_emit_by_name(webrtcbin, "set-local-description", offer, NULL);

    gchar *sdp = gst_sdp_message_as_text(offer->sdp);
    ems_signaling_server_send_sdp_offer(signaling_server, g_object_get_data(G_OBJECT(webrtcbin), "client_id"), sdp);
    g_free(sdp);

    gst_webrtc_session_description_free(offer);
}

static void webrtc_on_data_channel_cb(GstElement *webrtcbin,
                                      GObject *data_channel,
                                      struct ems_gstreamer_pipeline *egp) {
    U_LOG_I("webrtc_on_data_channel_cb called");
}

static void webrtc_on_ice_candidate_cb(GstElement *webrtcbin, guint mlineindex, gchar *candidate) {
    ems_signaling_server_send_candidate(signaling_server,
                                        g_object_get_data(G_OBJECT(webrtcbin), "client_id"),
                                        mlineindex,
                                        candidate);
}

static void data_channel_error_cb(GstWebRTCDataChannel *datachannel, struct ems_gstreamer_pipeline *egp) {
    U_LOG_E("error");
}

gboolean datachannel_send_message(GstWebRTCDataChannel *datachannel) {
    g_signal_emit_by_name(datachannel, "send-string", "Hi! from Electric Maple Server");

    char buf[] = "Electric Maple Server";
    GBytes *b = g_bytes_new_static(buf, ARRAY_SIZE(buf));
    gst_webrtc_data_channel_send_data(datachannel, b);

    return G_SOURCE_CONTINUE;
}

static void data_channel_open_cb(GstWebRTCDataChannel *datachannel, struct ems_gstreamer_pipeline *egp) {
    U_LOG_I("data channel opened");

    // egp->timeout_src_id = g_timeout_add_seconds(3, G_SOURCE_FUNC(datachannel_send_message), datachannel);
}

static void data_channel_close_cb(GstWebRTCDataChannel *datachannel, struct ems_gstreamer_pipeline *egp) {
    U_LOG_I("data channel closed");

    g_clear_handle_id(&egp->timeout_src_id, g_source_remove);
    g_clear_object(&egp->data_channel);
}

bool ProtoMessage_decode_hand_joint_locations(pb_istream_t *istream, const pb_field_t *field, void **arg) {
    em_proto_HandJointLocation *dest = *arg;

    em_proto_HandJointLocation location;

    if (!pb_decode(istream, (pb_msgdesc_t *)em_proto_HandJointLocation_fields, &location)) {
        const char *error = PB_GET_ERROR(istream);
        U_LOG_E("decode error: %s\n", error);
        return false;
    }

    dest[(int)location.index] = location;

    // U_LOG_E("Down %d %d %f %f %f", (int)location.index, location.has_pose, location.pose.position.x,
    // location.pose.position.y,
    //         location.pose.position.z);

    return true;
}

/// Used by both WebRTC & WebSocket
static void handle_up_message(GBytes *data, const struct ems_gstreamer_pipeline *egp) {
    em_UpMessageSuper message_super = {};
    em_proto_UpMessage message = em_proto_UpMessage_init_default;

    size_t n = 0;
    const unsigned char *buf = g_bytes_get_data(data, &n);

    pb_istream_t our_istream = pb_istream_from_buffer(buf, n);

    message.tracking.hand_joint_locations_left.funcs.decode = ProtoMessage_decode_hand_joint_locations;
    message.tracking.hand_joint_locations_left.arg = message_super.hand_joint_locations_left;

    message.tracking.hand_joint_locations_right.funcs.decode = ProtoMessage_decode_hand_joint_locations;
    message.tracking.hand_joint_locations_right.arg = message_super.hand_joint_locations_right;

    bool result = pb_decode_ex(&our_istream, &em_proto_UpMessage_msg, &message, PB_DECODE_NULLTERMINATED);
    if (!result) {
        U_LOG_E("Error! %s", PB_GET_ERROR(&our_istream));
        return;
    }

    message_super.protoMessage = message;

    if (message.has_tracking) {
        ems_callbacks_call(egp->callbacks, EMS_CALLBACKS_EVENT_TRACKING, &message_super);
        ems_callbacks_call(egp->callbacks, EMS_CALLBACKS_EVENT_CONTROLLER, &message_super);
    }

    if (message.has_frame) {
        U_LOG_D(
            "Client frame message: frame_sequence_id %ld decode_complete_time %ld begin_frame_time %ld "
            "display_time %ld",
            message.frame.frame_sequence_id,
            message.frame.decode_complete_time,
            message.frame.begin_frame_time,
            message.frame.display_time);
    }
}

static void data_channel_message_data_cb(GstWebRTCDataChannel *data_channel,
                                         GBytes *data,
                                         struct ems_gstreamer_pipeline *egp) {
    handle_up_message(data, egp);
}

static void ws_up_message_cb(EmsSignalingServer *server, GBytes *data, struct ems_gstreamer_pipeline *egp) {
    handle_up_message(data, egp);
}

static void data_channel_message_string_cb(GstWebRTCDataChannel *data_channel,
                                           gchar *str,
                                           struct ems_gstreamer_pipeline *egp) {
    U_LOG_I("Received data channel message: %s\n", str);
}

GstPadProbeReturn webrtcbin_srcpad_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    (void)pad;

    GstRTPBuffer rtp_buffer = GST_RTP_BUFFER_INIT;
    size_t extension_size;

    const struct ems_gstreamer_pipeline *egp = (struct ems_gstreamer_pipeline *)user_data;

    GstBuffer *buffer = gst_pad_probe_info_get_buffer(info);

    buffer = gst_buffer_make_writable(buffer);

    if (!gst_rtp_buffer_map(buffer, GST_MAP_WRITE, &rtp_buffer)) {
        U_LOG_E("Failed to map GstBuffer");
        return GST_PAD_PROBE_REMOVE;
    }

    // Add extension data only on last Access Unit, indicated by the marker bit.
    if (!gst_rtp_buffer_get_marker(&rtp_buffer)) {
        gst_rtp_buffer_unmap(&rtp_buffer);
        return GST_PAD_PROBE_OK; // Nothing's wrong, we keep the pad active.
    }

    // Inject extension data
    const gconstpointer extension_data = g_bytes_get_data(egp->downMsg_bytes, &extension_size);

    if (extension_size > RTP_TWOBYTES_HDR_EXT_MAX_SIZE) {
        U_LOG_E("size of data in rtp header is too large! Implement multi-extension-element support!");
        gst_rtp_buffer_unmap(&rtp_buffer);
        return GST_PAD_PROBE_REMOVE;
    }

    if (!gst_rtp_buffer_add_extension_twobytes_header(&rtp_buffer,
                                                      0 /* appbits */,
                                                      RTP_TWOBYTES_HDR_EXT_ID,
                                                      extension_data,
                                                      (guint)extension_size)) {
        U_LOG_E("Failed to add extension data!");
        return GST_PAD_PROBE_REMOVE;
    }

    gst_rtp_buffer_unmap(&rtp_buffer);

    return GST_PAD_PROBE_OK;
}

static gboolean check_pipeline_dot_data(struct ems_gstreamer_pipeline *egp) {
    if (!egp || !egp->base.pipeline) {
        return G_SOURCE_CONTINUE;
    }

    gchar *dot_data = gst_debug_bin_to_dot_data(GST_BIN(egp->base.pipeline), GST_DEBUG_GRAPH_SHOW_ALL);
    g_free(dot_data);

    return G_SOURCE_CONTINUE;
}

/// When a WebSocket connection is established, we start creating a WebRTC connection.
static void webrtc_client_connected_cb(EmsSignalingServer *server,
                                       const EmsClientId client_id,
                                       const gchar *client_address,
                                       struct ems_gstreamer_pipeline *egp) {
    U_LOG_I("WebRTC client connected: %p", client_id);

#ifdef USE_WEBRTC
    GstBin *pipeline = GST_BIN(egp->base.pipeline);

    gchar *name = g_strdup_printf("webrtcbin_%p", client_id);
    GstElement *webrtcbin = gst_element_factory_make("webrtcbin", name);
    g_free(name);

    g_object_set(webrtcbin, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, NULL);
    g_object_set_data(G_OBJECT(webrtcbin), "client_id", client_id);
    gst_bin_add(pipeline, webrtcbin);

    GstStateChangeReturn ret = gst_element_set_state(webrtcbin, GST_STATE_READY);
    g_assert(ret != GST_STATE_CHANGE_FAILURE);

    g_signal_connect(webrtcbin, "on-data-channel", G_CALLBACK(webrtc_on_data_channel_cb), NULL);

    // I also think this would work if the pipeline state is READY but /shrug

    // TODO add priority
    GstStructure *data_channel_options = gst_structure_new_from_string("data-channel-options, ordered=false");
    g_signal_emit_by_name(webrtcbin, "create-data-channel", "channel", data_channel_options, &egp->data_channel);
    gst_clear_structure(&data_channel_options);

    if (!egp->data_channel) {
        U_LOG_E("Couldn't make datachannel!");
        assert(false);
    } else {
        U_LOG_I("Successfully created datachannel!");

        g_signal_connect(egp->data_channel, "on-open", G_CALLBACK(data_channel_open_cb), egp);
        g_signal_connect(egp->data_channel, "on-close", G_CALLBACK(data_channel_close_cb), egp);
        g_signal_connect(egp->data_channel, "on-error", G_CALLBACK(data_channel_error_cb), egp);
        g_signal_connect(egp->data_channel, "on-message-data", G_CALLBACK(data_channel_message_data_cb), egp);
        g_signal_connect(egp->data_channel, "on-message-string", G_CALLBACK(data_channel_message_string_cb), egp);
    }

    // Get the srcpad associated with our webrtcbin element
    GstPad *webrtcbin_srcpad = gst_element_get_static_pad(webrtcbin, "src");

    if (webrtcbin_srcpad != NULL) {
        // Add a probe to call our callback when buffers get to the src pad
        gst_pad_add_probe(webrtcbin_srcpad,
                          GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST,
                          webrtcbin_srcpad_probe,
                          egp,
                          NULL /*destroy_data*/);

        gst_object_unref(webrtcbin_srcpad);
    } else {
        U_LOG_E("Could not retrieve webrtcbin srcpad!");
    }

    g_signal_connect(webrtcbin, "on-ice-candidate", G_CALLBACK(webrtc_on_ice_candidate_cb), NULL);

    connect_webrtc_to_tee(webrtcbin);

    GstPromise *promise = gst_promise_new_with_change_func((GstPromiseChangeFunc)on_offer_created, webrtcbin, NULL);
    g_signal_emit_by_name(webrtcbin, "create-offer", NULL, promise);

    GST_DEBUG_BIN_TO_DOT_FILE(pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "rtcbin");

    ret = gst_element_set_state(webrtcbin, GST_STATE_PLAYING);
    g_assert(ret != GST_STATE_CHANGE_FAILURE);
#else
    GstElement *udpsink = gst_bin_get_by_name(GST_BIN(egp->base.pipeline), "udpsink");
    g_assert(udpsink);
    g_object_set(udpsink, "host", client_address, NULL);
    gst_object_unref(udpsink);
#endif

    egp->timeout_src_id_dot_data = g_timeout_add_seconds(3, G_SOURCE_FUNC(check_pipeline_dot_data), egp);
}

static void webrtc_sdp_answer_cb(EmsSignalingServer *server,
                                 EmsClientId client_id,
                                 const gchar *sdp,
                                 struct ems_gstreamer_pipeline *egp) {
    GstBin *pipeline = GST_BIN(egp->base.pipeline);
    GstSDPMessage *sdp_msg = NULL;
    GstWebRTCSessionDescription *desc = NULL;

    if (gst_sdp_message_new_from_text(sdp, &sdp_msg) != GST_SDP_OK) {
        U_LOG_I("Error parsing SDP description");
        goto out;
    }

    desc = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp_msg);
    if (desc) {
        GstPromise *promise;

        GstElement *webrtcbin = get_webrtcbin_for_client(pipeline, client_id);
        if (!webrtcbin) {
            goto out;
        }
        promise = gst_promise_new();

        g_signal_emit_by_name(webrtcbin, "set-remote-description", desc, promise);

        gst_promise_wait(promise);
        gst_promise_unref(promise);

        gst_object_unref(webrtcbin);
    } else {
        gst_sdp_message_free(sdp_msg);
    }

out:
    g_clear_pointer(&desc, gst_webrtc_session_description_free);
}

static void webrtc_candidate_cb(EmsSignalingServer *server,
                                EmsClientId client_id,
                                guint mlineindex,
                                const gchar *candidate,
                                struct ems_gstreamer_pipeline *egp) {
    GstBin *pipeline = GST_BIN(egp->base.pipeline);

    if (strlen(candidate)) {
        GstElement *webrtcbin = get_webrtcbin_for_client(pipeline, client_id);
        if (webrtcbin) {
            g_signal_emit_by_name(webrtcbin, "add-ice-candidate", mlineindex, candidate);
            gst_object_unref(webrtcbin);
        }
    }

    U_LOG_I("Remote candidate: %s", candidate);
}

static GstPadProbeReturn remove_webrtcbin_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    GstElement *webrtcbin = GST_ELEMENT(user_data);

    gst_element_set_state(webrtcbin, GST_STATE_NULL);
    gst_bin_remove(GST_BIN(GST_ELEMENT_PARENT(webrtcbin)), webrtcbin);

    return GST_PAD_PROBE_REMOVE;
}

static void webrtc_client_disconnected_cb(EmsSignalingServer *server,
                                          EmsClientId client_id,
                                          struct ems_gstreamer_pipeline *egp) {
    U_LOG_I("WebRTC client disconnected: %p", client_id);

    GstBin *pipeline = GST_BIN(egp->base.pipeline);
    GstElement *webrtcbin = get_webrtcbin_for_client(pipeline, client_id);

    if (webrtcbin) {
        webrtcbin_get_stats(webrtcbin);

        // Firstly, we block the dataflow into the webrtcbin
        GstPad *sinkpad = gst_element_get_static_pad(webrtcbin, "sink_0");
        if (sinkpad) {
            gst_pad_add_probe(GST_PAD_PEER(sinkpad),
                              GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                              remove_webrtcbin_probe_cb,
                              webrtcbin,
                              gst_object_unref);

            gst_clear_object(&sinkpad);
        }
    }
}

struct RestartData {
    GstElement *src;
    GstElement *pipeline;
};

static void free_restart_data(gpointer user_data) {
    struct RestartData *rd = user_data;

    gst_object_unref(rd->src);
    g_free(rd);
}

static gboolean restart_source(gpointer user_data) {
    struct RestartData *rd = user_data;

    gst_element_set_state(rd->src, GST_STATE_NULL);
    gst_element_set_locked_state(rd->src, TRUE);
    GstElement *e = gst_bin_get_by_name(GST_BIN(rd->pipeline), "srtqueue");
    gst_bin_add(GST_BIN(rd->pipeline), rd->src);
    if (!gst_element_link(rd->src, e)) g_assert_not_reached();
    gst_element_set_locked_state(rd->src, FALSE);
    GstStateChangeReturn ret = gst_element_set_state(rd->src, GST_STATE_PLAYING);
    g_assert(ret != GST_STATE_CHANGE_FAILURE);
    gst_object_unref(e);

    U_LOG_I("Restarted source after EOS");

    return G_SOURCE_REMOVE;
}

static GstPadProbeReturn src_event_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    GstElement *pipeline = user_data;

    if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) != GST_EVENT_EOS) return GST_PAD_PROBE_PASS;

    GstElement *src = gst_pad_get_parent_element(pad);

    gst_bin_remove(GST_BIN(pipeline), src);

    struct RestartData *rd = g_new(struct RestartData, 1);
    rd->src = src;
    rd->pipeline = pipeline;
    g_idle_add_full(G_PRIORITY_HIGH_IDLE, restart_source, rd, free_restart_data);

    return GST_PAD_PROBE_DROP;
}

static gboolean print_stats(gpointer user_data) {
    GstElement *src = user_data;
    GstStructure *s;

    g_object_get(src, "stats", &s, NULL);
    char *str = gst_structure_to_string(s);
    U_LOG_I("%s", str);
    g_free(str);
    gst_structure_free(s);

    return G_SOURCE_CONTINUE;
}

/*
 *
 * Internal pipeline functions.
 *
 */

static void break_apart(struct xrt_frame_node *node) {
    struct gstreamer_pipeline *gp = container_of(node, struct gstreamer_pipeline, node);

    /*
     * This function is called when we are shutting down, after returning
     * from this function you are not allowed to call any other nodes in the
     * graph. But it must be safe for other nodes to call any normal
     * functions on us. Once the context is done calling break_aprt on all
     * objects, it will call destroy() on them.
     */

    (void)gp;
}

static void destroy(struct xrt_frame_node *node) {
    struct gstreamer_pipeline *gp = container_of(node, struct gstreamer_pipeline, node);

    /*
     * All the nodes have been broken apart, and none of our functions will
     * be called, it's now safe to destroy and free ourselves.
     */

    free(gp);
}

GMainLoop *main_loop;

void *loop_thread(void *data) {
    g_main_loop_run(main_loop);
    return NULL;
}

/*
 *
 * Exported functions.
 *
 */

void ems_gstreamer_pipeline_set_down_msg(struct gstreamer_pipeline *gp, em_proto_DownMessage *msg) {
    struct ems_gstreamer_pipeline *egp = (struct ems_gstreamer_pipeline *)gp;

    uint8_t buf[em_proto_DownMessage_size];
    size_t n = 0;
    pb_get_encoded_size(&n, em_proto_DownMessage_fields, &msg);

    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));

    pb_encode(&os, em_proto_DownMessage_fields, &msg);

    g_bytes_unref(egp->downMsg_bytes);
    egp->downMsg_bytes = NULL;
    egp->downMsg_bytes = g_bytes_new(buf, n);
}

void ems_gstreamer_pipeline_play(struct gstreamer_pipeline *gp) {
    U_LOG_I("Starting pipeline");
    struct ems_gstreamer_pipeline *egp = (struct ems_gstreamer_pipeline *)gp;

    main_loop = g_main_loop_new(NULL, FALSE);

    const GstStateChangeReturn ret = gst_element_set_state(egp->base.pipeline, GST_STATE_PLAYING);
    g_assert(ret != GST_STATE_CHANGE_FAILURE);

    g_signal_connect(signaling_server, "ws-client-connected", G_CALLBACK(webrtc_client_connected_cb), egp);

    pthread_t thread;
    pthread_create(&thread, NULL, loop_thread, NULL);
}

void ems_gstreamer_pipeline_stop(struct gstreamer_pipeline *gp) {
    struct ems_gstreamer_pipeline *egp = (struct ems_gstreamer_pipeline *)gp;
    U_LOG_I("Stopping pipeline");

    // Settle the pipeline.
    U_LOG_T("Sending EOS");
    gst_element_send_event(egp->base.pipeline, gst_event_new_eos());

    // Wait for an EOS message on the pipeline bus.
    U_LOG_T("Waiting for EOS");
    GstMessage *msg = NULL;
    msg = gst_bus_timed_pop_filtered(GST_ELEMENT_BUS(egp->base.pipeline),
                                     GST_CLOCK_TIME_NONE,
                                     GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    //! @todo Should check if we got an error message here or an eos.
    (void)msg;

    // Completely stop the pipeline.
    U_LOG_T("Setting to NULL");
    gst_element_set_state(egp->base.pipeline, GST_STATE_NULL);
}

void gstAndroidLog(GstDebugCategory *category,
                   GstDebugLevel level,
                   const gchar *file,
                   const gchar *function,
                   gint line,
                   GObject *object,
                   GstDebugMessage *message,
                   gpointer data) {
    if (level <= gst_debug_category_get_threshold(category)) {
        if (level == GST_LEVEL_ERROR) {
            U_LOG_E("%s, %s: %s", file, function, gst_debug_message_get(message));
        } else {
            U_LOG_W("%s, %s: %s", file, function, gst_debug_message_get(message));
        }
    }
}

void ems_gstreamer_pipeline_create(struct xrt_frame_context *xfctx,
                                   const char *appsrc_name,
                                   struct ems_callbacks *callbacks_collection,
                                   struct gstreamer_pipeline **out_gp) {
    GError *error = NULL;

    // In case this function is called many times
    if (signaling_server) {
        g_object_unref(signaling_server);
    }

    signaling_server = ems_signaling_server_new();

    gchar *pipeline_str = g_strdup_printf(
        "appsrc name=%s ! "
        "videoconvert ! "
        "videorate ! "
        "video/x-raw,format=NV12,framerate=60/1 ! "
        "encodebin2 "
        "profile=\"video/x-h264|element-properties,tune=4,speed-preset=1,bframes=0,bitrate=%s,key-int-max=120\" ! "
        "rtph264pay config-interval=-1 aggregate-mode=zero-latency ! "
        "application/x-rtp,payload=96,ssrc=(uint)3484078952 ! "
#ifdef USE_WEBRTC
        "tee name=%s allow-not-linked=true",
        appsrc_name,
        DEFAULT_BITRATE,
        WEBRTC_TEE_NAME);
#else
        "udpsink name=udpsink port=5600", // host will be assigned later
        appsrc_name,
        DEFAULT_BITRATE);
#endif

    // No webrtc bin yet until later!

    U_LOG_I("EMS gstreamer pipeline: %s", pipeline_str);

    struct ems_gstreamer_pipeline *egp = U_TYPED_CALLOC(struct ems_gstreamer_pipeline);
    egp->base.node.break_apart = break_apart;
    egp->base.node.destroy = destroy;
    egp->base.xfctx = xfctx;
    egp->callbacks = callbacks_collection;

    // setenv("GST_TRACERS", "latency(flags=pipeline+element+reported)", 1);
    // setenv("GST_DEBUG", "GST_TRACER:7", 1);
    // setenv("GST_DEBUG_FILE", "./latency.log", 1);

    gst_init(NULL, NULL);

#ifdef __ANDROID__
    gst_debug_add_log_function(&gstAndroidLog, NULL, NULL);
#endif
    gst_debug_set_default_threshold(GST_LEVEL_WARNING);
    gst_debug_set_threshold_for_name("decodebin2", GST_LEVEL_INFO);
    gst_debug_set_threshold_for_name("webrtcbin", GST_LEVEL_INFO);
    gst_debug_set_threshold_for_name("webrtcbindatachannel", GST_LEVEL_INFO);

    GstElement *pipeline = gst_parse_launch(pipeline_str, &error);
    g_assert_no_error(error);
    g_free(pipeline_str);

    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, gst_bus_cb, egp);
    gst_object_unref(bus);

#ifdef USE_WEBRTC
    g_signal_connect(signaling_server, "ws-client-disconnected", G_CALLBACK(webrtc_client_disconnected_cb), egp);
    g_signal_connect(signaling_server, "sdp-answer", G_CALLBACK(webrtc_sdp_answer_cb), egp);
    g_signal_connect(signaling_server, "candidate", G_CALLBACK(webrtc_candidate_cb), egp);
#else
    g_signal_connect(signaling_server, "up_message", G_CALLBACK(ws_up_message_cb), egp);
#endif

    // Setup pipeline.
    egp->base.pipeline = pipeline;

    /*
     * Add ourselves to the context so we are destroyed.
     * This is done once we know everything is completed.
     */
    xrt_frame_context_add(xfctx, &egp->base.node);

    *out_gp = &egp->base;
}
