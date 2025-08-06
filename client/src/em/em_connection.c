// Copyright 2020-2023, Collabora, Ltd.
// Copyright 2022-2023, PlutoVR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  WebRTC handshake/connection for ElectricMaple XR streaming frameserver
 * @author Rylie Pavlik <rpavlik@collabora.com>
 * @author Moshi Turner <moses@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Christoph Haag <christoph.haag@collabora.com>
 * @author Pete Black <pblack@collabora.com>
 * @ingroup em_client
 */

#include "em_connection.h"

#include <gst/gstelement.h>
#include <gst/gstobject.h>
#include <stdbool.h>
#include <string.h>

#include "em_app_log.h"
#include "em_status.h"

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#undef GST_USE_UNSTABLE_API

#include <json-glib/json-glib.h>
#include <libsoup/soup-message.h>
#include <libsoup/soup-session.h>

#define DEFAULT_WEBSOCKET_URI "ws://192.168.49.1:52356/ws" // Android P2P group owner address

/*!
 * Data required for the handshake to complete and to maintain the connection.
 */
struct _EmConnection {
    GObject parent;
    SoupSession *soup_session;
    gchar *websocket_uri;
    /// Cancellable for websocket connection process
    GCancellable *ws_cancel;
    SoupWebsocketConnection *ws;

    GstPipeline *pipeline;
    GstElement *webrtcbin;
    GstWebRTCDataChannel *datachannel;

    enum em_status status;
};

G_DEFINE_TYPE(EmConnection, em_connection, G_TYPE_OBJECT)

enum {
    // action signals
    SIGNAL_CONNECT,
    SIGNAL_DISCONNECT,
    SIGNAL_SET_PIPELINE,
    // signals
    SIGNAL_WEBSOCKET_CONNECTED,
    SIGNAL_WEBSOCKET_FAILED,
    SIGNAL_CONNECTED,
    SIGNAL_STATUS_CHANGE,
    SIGNAL_ON_NEED_PIPELINE,
    SIGNAL_ON_DROP_PIPELINE,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef enum {
    PROP_WEBSOCKET_URI = 1,
    // PROP_STATUS,
    N_PROPERTIES
} EmConnectionProperty;

static GParamSpec *properties[N_PROPERTIES] = {
    NULL,
};

/* GObject method implementations */

static void em_connection_set_property(GObject *object, guint property_id, const GValue *value, GParamSpec *pspec) {
    EmConnection *self = EM_CONNECTION(object);

    switch ((EmConnectionProperty)property_id) {
        case PROP_WEBSOCKET_URI:
            g_free(self->websocket_uri);
            self->websocket_uri = g_value_dup_string(value);
            ALOGI("websocket URI assigned; %s", self->websocket_uri);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
            break;
    }
}

static void em_connection_get_property(GObject *object, guint property_id, GValue *value, GParamSpec *pspec) {
    EmConnection *self = EM_CONNECTION(object);

    switch ((EmConnectionProperty)property_id) {
        case PROP_WEBSOCKET_URI:
            g_value_set_string(value, self->websocket_uri);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
            break;
    }
}

static void em_connection_init(EmConnection *em_conn) {
    em_conn->ws_cancel = g_cancellable_new();
    em_conn->soup_session = soup_session_new();
    em_conn->websocket_uri = g_strdup(DEFAULT_WEBSOCKET_URI);
}

static void em_connection_dispose(GObject *object) {
    EmConnection *self = EM_CONNECTION(object);

    em_connection_disconnect(self);

    g_clear_object(&self->soup_session);
    g_clear_object(&self->ws_cancel);
}

static void em_connection_finalize(GObject *object) {
    EmConnection *self = EM_CONNECTION(object);

    g_free(self->websocket_uri);
}

static void em_connection_class_init(EmConnectionClass *klass) {
    ALOGI("%s: Begin", __FUNCTION__);
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

    gobject_class->dispose = em_connection_dispose;
    gobject_class->finalize = em_connection_finalize;

    gobject_class->set_property = em_connection_set_property;
    gobject_class->get_property = em_connection_get_property;

    /**
     * EmConnection:websocket-uri:
     *
     * The websocket URI for the signaling server
     */
    g_object_class_install_property(
        gobject_class,
        PROP_WEBSOCKET_URI,
        g_param_spec_string("websocket-uri",
                            "WebSocket URI",
                            "WebSocket URI for signaling server.",
                            DEFAULT_WEBSOCKET_URI /* default value */,
                            G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
     * EmConnection::connect
     * @object: the #EmConnection
     *
     * Start the connection process
     */
    signals[SIGNAL_CONNECT] = g_signal_new_class_handler("connect",
                                                         G_OBJECT_CLASS_TYPE(klass),
                                                         G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                                                         G_CALLBACK(em_connection_connect),
                                                         NULL,
                                                         NULL,
                                                         NULL,
                                                         G_TYPE_NONE,
                                                         0);

    /**
     * EmConnection::disconnect
     * @object: the #EmConnection
     *
     * Stop the connection process or shutdown the connection
     */
    signals[SIGNAL_DISCONNECT] = g_signal_new_class_handler("disconnect",
                                                            G_OBJECT_CLASS_TYPE(klass),
                                                            G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                                                            G_CALLBACK(em_connection_disconnect),
                                                            NULL,
                                                            NULL,
                                                            NULL,
                                                            G_TYPE_NONE,
                                                            0);

    /**
     * EmConnection::set-pipeline
     * @object: the #EmConnection
     * @pipeline: A #GstPipeline
     *
     * Sets the #GstPipeline containing a #GstWebRTCBin element and begins the WebRTC connection negotiation.
     * Should be signalled in response to @on-need-pipeline
     */
    signals[SIGNAL_SET_PIPELINE] = g_signal_new_class_handler("set-pipeline",
                                                              G_OBJECT_CLASS_TYPE(klass),
                                                              G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                                                              G_CALLBACK(em_connection_set_pipeline),
                                                              NULL,
                                                              NULL,
                                                              NULL,
                                                              G_TYPE_NONE,
                                                              1,
                                                              G_TYPE_POINTER);

    /**
     * EmConnection::websocket-connected
     * @object: the #EmConnection
     */
    signals[SIGNAL_WEBSOCKET_CONNECTED] = g_signal_new("websocket-connected",
                                                       G_OBJECT_CLASS_TYPE(klass),
                                                       G_SIGNAL_RUN_LAST,
                                                       0,
                                                       NULL,
                                                       NULL,
                                                       NULL,
                                                       G_TYPE_NONE,
                                                       0);

    /**
     * EmConnection::websocket-failed
     * @object: the #EmConnection
     */
    signals[SIGNAL_WEBSOCKET_FAILED] = g_signal_new("websocket-failed",
                                                    G_OBJECT_CLASS_TYPE(klass),
                                                    G_SIGNAL_RUN_LAST,
                                                    0,
                                                    NULL,
                                                    NULL,
                                                    NULL,
                                                    G_TYPE_NONE,
                                                    0);
    /**
     * EmConnection::connected
     * @object: the #EmConnection
     */
    signals[SIGNAL_CONNECTED] =
        g_signal_new("connected", G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);

    /**
     * EmConnection::on-need-pipeline
     * @object: the #EmConnection
     *
     * Your handler for this must emit @set-pipeline
     */
    signals[SIGNAL_ON_NEED_PIPELINE] = g_signal_new("on-need-pipeline",
                                                    G_OBJECT_CLASS_TYPE(klass),
                                                    G_SIGNAL_RUN_LAST,
                                                    0,
                                                    NULL,
                                                    NULL,
                                                    NULL,
                                                    G_TYPE_NONE,
                                                    0);

    /**
     * EmConnection::on-drop-pipeline
     * @object: the #EmConnection
     *
     * If you store any references in your handler for @on-need-pipeline you must make a handler for this signal to
     * drop them.
     */
    signals[SIGNAL_ON_DROP_PIPELINE] = g_signal_new("on-drop-pipeline",
                                                    G_OBJECT_CLASS_TYPE(klass),
                                                    G_SIGNAL_RUN_LAST,
                                                    0,
                                                    NULL,
                                                    NULL,
                                                    NULL,
                                                    G_TYPE_NONE,
                                                    0);
    ALOGI("%s: End", __FUNCTION__);
}

#define MAKE_CASE(E) \
    case E:          \
        return #E

static const char *peer_connection_state_to_string(GstWebRTCPeerConnectionState state) {
    switch (state) {
        MAKE_CASE(GST_WEBRTC_PEER_CONNECTION_STATE_NEW);
        MAKE_CASE(GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTING);
        MAKE_CASE(GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED);
        MAKE_CASE(GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED);
        MAKE_CASE(GST_WEBRTC_PEER_CONNECTION_STATE_FAILED);
        MAKE_CASE(GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED);
        default:
            return "!Unknown!";
    }
}
#undef MAKE_CASE

static void em_conn_update_status(EmConnection *em_conn, enum em_status status) {
    if (status == em_conn->status) {
        ALOGI("em_conn: state update: already in %s", em_status_to_string(em_conn->status));
        return;
    }
    ALOGI("em_conn: state update: %s -> %s", em_status_to_string(em_conn->status), em_status_to_string(status));
    em_conn->status = status;
}

static void em_conn_update_status_from_peer_connection_state(EmConnection *em_conn,
                                                             GstWebRTCPeerConnectionState state) {
    switch (state) {
        case GST_WEBRTC_PEER_CONNECTION_STATE_NEW:
            break;
        case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTING:
            em_conn_update_status(em_conn, EM_STATUS_NEGOTIATING);
            break;
        case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED:
            em_conn_update_status(em_conn, EM_STATUS_CONNECTED_NO_DATA);
            break;

        case GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED:
        case GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED:
            em_conn_update_status(em_conn, EM_STATUS_IDLE_NOT_CONNECTED);
            break;

        case GST_WEBRTC_PEER_CONNECTION_STATE_FAILED:
            em_conn_update_status(em_conn, EM_STATUS_DISCONNECTED_ERROR);
            break;
    }
}

static void em_conn_disconnect_internal(EmConnection *em_conn, enum em_status status) {
    if (!em_conn) {
        return;
    }

    if (em_conn->ws_cancel != NULL) {
        g_cancellable_cancel(em_conn->ws_cancel);
        gst_clear_object(&em_conn->ws_cancel);
    }

    // Stop the pipeline, if it exists
    if (em_conn->pipeline != NULL) {
        gst_element_set_state(GST_ELEMENT(em_conn->pipeline), GST_STATE_NULL);
        g_signal_emit(em_conn, signals[SIGNAL_ON_DROP_PIPELINE], 0);
    }

    if (em_conn->ws) {
        soup_websocket_connection_close(em_conn->ws, 0, "");
    }
    g_clear_object(&em_conn->ws);

    gst_clear_object(&em_conn->webrtcbin);
    gst_clear_object(&em_conn->datachannel);
    gst_clear_object(&em_conn->pipeline);
    em_conn_update_status(em_conn, status);
}

static void em_conn_data_channel_error_cb(GstWebRTCDataChannel *datachannel, EmConnection *em_conn) {
    ALOGE("%s: error", __FUNCTION__);
    em_conn_disconnect_internal(em_conn, EM_STATUS_DISCONNECTED_ERROR);
    // abort();
}

static void em_conn_data_channel_close_cb(GstWebRTCDataChannel *datachannel, EmConnection *em_conn) {
    ALOGI("%s: Data channel closed", __FUNCTION__);
    em_conn_disconnect_internal(em_conn, EM_STATUS_DISCONNECTED_REMOTE_CLOSE);
}

static void em_conn_data_channel_message_string_cb(GstWebRTCDataChannel *datachannel,
                                                   gchar *str,
                                                   EmConnection *em_conn) {
    ALOGI("%s: Received data channel message: %s", __FUNCTION__, str);
}

static void em_conn_connect_internal(EmConnection *em_conn, enum em_status status);

static void em_conn_webrtc_deep_notify_callback(GstObject *self,
                                                GstObject *prop_object,
                                                GParamSpec *prop,
                                                EmConnection *em_conn) {
    GstWebRTCPeerConnectionState state;
    g_object_get(prop_object, "connection-state", &state, NULL);
    ALOGV("deep-notify callback says peer connection state is %s - but it lies sometimes",
          peer_connection_state_to_string(state));
    //	em_conn_update_status_from_peer_connection_state(em_conn, state);
}

static void em_conn_webrtc_prepare_data_channel_cb(GstElement *webrtc,
                                                   GObject *data_channel,
                                                   gboolean is_local,
                                                   EmConnection *em_conn) {
    ALOGI("preparing data channel");

    g_signal_connect(data_channel, "on-close", G_CALLBACK(em_conn_data_channel_close_cb), em_conn);
    g_signal_connect(data_channel, "on-error", G_CALLBACK(em_conn_data_channel_error_cb), em_conn);
    g_signal_connect(data_channel, "on-message-string", G_CALLBACK(em_conn_data_channel_message_string_cb), em_conn);
}

static void em_conn_webrtc_on_incoming_stream(GstElement *webrtc, GstPad *pad, gpointer user_data) {
    ALOGI("Got incoming stream");

    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) return;

    GstCaps *caps = gst_pad_get_current_caps(pad);
    gchar *str = gst_caps_serialize(caps, 0);
    ALOGI("Pad caps: %s", str);
}

static void em_conn_webrtc_on_data_channel_cb(GstElement *webrtcbin,
                                              GstWebRTCDataChannel *data_channel,
                                              EmConnection *em_conn) {
    ALOGI("Successfully created datachannel");

    g_assert_null(em_conn->datachannel);

    em_conn->datachannel = GST_WEBRTC_DATA_CHANNEL(data_channel);

    em_conn_update_status(em_conn, EM_STATUS_CONNECTED);
    g_signal_emit(em_conn, signals[SIGNAL_CONNECTED], 0);
}

static void em_conn_webrtc_on_prepare_data_channel(GstElement *webrtcbin,
                                                   GstWebRTCDataChannel *channel,
                                                   gboolean is_local,
                                                   gpointer udata) {
    // Adjust receive buffer size (IMPORTANT)
    {
        GstWebRTCSCTPTransport *sctp_transport = NULL;
        g_object_get(webrtcbin, "sctp-transport", &sctp_transport, NULL);
        if (!sctp_transport) {
            g_error("Failed to get sctp_transport!");
        }

        GstWebRTCDTLSTransport *dtls_transport = NULL;
        g_object_get(sctp_transport, "transport", &dtls_transport, NULL);
        if (!dtls_transport) {
            g_error("Failed to get dtls_transport!");
        }

        GstWebRTCICETransport *ice_transport = NULL;
        g_object_get(dtls_transport, "transport", &ice_transport, NULL);

        if (ice_transport) {
            g_object_set(ice_transport, "receive-buffer-size", 8 * 1024 * 1024, NULL);
        } else {
            g_error("Failed to get ice_transport!");
        }

        g_object_unref(ice_transport);
        g_object_unref(dtls_transport);
        g_object_unref(sctp_transport);
    }
}

void em_conn_send_sdp_answer(EmConnection *em_conn, const gchar *sdp) {
    JsonBuilder *builder;
    JsonNode *root;
    gchar *msg_str;

    ALOGI("Send answer: %s", sdp);

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "msg");
    json_builder_add_string_value(builder, "answer");

    json_builder_set_member_name(builder, "sdp");
    json_builder_add_string_value(builder, sdp);
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);

    msg_str = json_to_string(root, TRUE);
    soup_websocket_connection_send_text(em_conn->ws, msg_str);
    g_clear_pointer(&msg_str, g_free);

    json_node_unref(root);
    g_object_unref(builder);
}

static void em_conn_webrtc_on_ice_candidate_cb(GstElement *webrtcbin,
                                               guint mlineindex,
                                               gchar *candidate,
                                               EmConnection *em_conn) {
    JsonBuilder *builder;
    JsonNode *root;
    gchar *msg_str;

    ALOGI("Send candidate: line %u: %s", mlineindex, candidate);

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "msg");
    json_builder_add_string_value(builder, "candidate");

    json_builder_set_member_name(builder, "candidate");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "candidate");
    json_builder_add_string_value(builder, candidate);
    json_builder_set_member_name(builder, "sdpMLineIndex");
    json_builder_add_int_value(builder, mlineindex);
    json_builder_end_object(builder);
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);

    msg_str = json_to_string(root, TRUE);
    ALOGD("%s: candidate message: %s", __FUNCTION__, msg_str);
    soup_websocket_connection_send_text(em_conn->ws, msg_str);
    g_clear_pointer(&msg_str, g_free);

    json_node_unref(root);
    g_object_unref(builder);
}

static void em_conn_webrtc_on_answer_created(GstPromise *promise, EmConnection *em_conn) {
    GstWebRTCSessionDescription *answer = NULL;
    gchar *sdp;

    ALOGD("%s", __FUNCTION__);
    gst_structure_get(gst_promise_get_reply(promise), "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
    gst_promise_unref(promise);

    if (NULL == answer) {
        ALOGE("%s : ERROR !  get_promise answer = null !", __FUNCTION__);
    }

    g_signal_emit_by_name(em_conn->webrtcbin, "set-local-description", answer, NULL);

    sdp = gst_sdp_message_as_text(answer->sdp);
    if (NULL == sdp) {
        ALOGE("%s : ERROR !  sdp = null !", __FUNCTION__);
    }
    em_conn_send_sdp_answer(em_conn, sdp);
    g_free(sdp);

    gst_webrtc_session_description_free(answer);
}

static void em_conn_webrtc_process_sdp_offer(EmConnection *em_conn, const gchar *sdp) {
    GstSDPMessage *sdp_msg = NULL;
    GstWebRTCSessionDescription *desc = NULL;

    ALOGI("Received offer: %s\n", sdp);

    if (gst_sdp_message_new_from_text(sdp, &sdp_msg) != GST_SDP_OK) {
        g_debug("Error parsing SDP description");
        goto out;
    }

    desc = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp_msg);
    if (desc) {
        GstPromise *promise;

        promise = gst_promise_new();

        g_signal_emit_by_name(em_conn->webrtcbin, "set-remote-description", desc, promise);

        gst_promise_wait(promise);
        gst_promise_unref(promise);

        g_signal_emit_by_name(
            em_conn->webrtcbin,
            "create-answer",
            NULL,
            gst_promise_new_with_change_func((GstPromiseChangeFunc)em_conn_webrtc_on_answer_created, em_conn, NULL));
    } else {
        gst_sdp_message_free(sdp_msg);
    }

out:
    g_clear_pointer(&desc, gst_webrtc_session_description_free);
}

static void em_conn_webrtc_process_candidate(EmConnection *em_conn, guint mlineindex, const gchar *candidate) {
    ALOGI("process_candidate: %d %s", mlineindex, candidate);

    g_signal_emit_by_name(em_conn->webrtcbin, "add-ice-candidate", mlineindex, candidate);
}

static void em_conn_on_ws_message_cb(SoupWebsocketConnection *connection,
                                     gint type,
                                     GBytes *message,
                                     EmConnection *em_conn) {
    ALOGD("%s", __FUNCTION__);
    gsize length = 0;
    const gchar *msg_data = g_bytes_get_data(message, &length);
    JsonParser *parser = json_parser_new();
    GError *error = NULL;

    // TODO convert gsize to gssize after range check

    if (json_parser_load_from_data(parser, msg_data, length, &error)) {
        JsonObject *msg = json_node_get_object(json_parser_get_root(parser));
        const gchar *msg_type;

        if (!json_object_has_member(msg, "msg")) {
            // Invalid message
            goto out;
        }

        msg_type = json_object_get_string_member(msg, "msg");
        ALOGI("Websocket message received: %s", msg_type);

        if (g_str_equal(msg_type, "offer")) {
            const gchar *offer_sdp = json_object_get_string_member(msg, "sdp");
            em_conn_webrtc_process_sdp_offer(em_conn, offer_sdp);
        } else if (g_str_equal(msg_type, "candidate")) {
            JsonObject *candidate;

            candidate = json_object_get_object_member(msg, "candidate");

            em_conn_webrtc_process_candidate(em_conn,
                                             json_object_get_int_member(candidate, "sdpMLineIndex"),
                                             json_object_get_string_member(candidate, "candidate"));
        }
    } else {
        g_debug("Error parsing message: %s", error->message);
        g_clear_error(&error);
    }

out:
    g_object_unref(parser);
}

static void em_conn_websocket_connected_cb(GObject *session, GAsyncResult *res, EmConnection *em_conn) {
    g_assert(!em_conn->ws);

    GError *error = NULL;

    SoupWebsocketConnection *conn = soup_session_websocket_connect_finish(SOUP_SESSION(session), res, &error);
    if (conn) {
        em_conn->ws = g_object_ref_sink(conn);
    }

    if (error) {
        ALOGE("Websocket connection failed, error: '%s'", error->message);
        g_signal_emit(em_conn, signals[SIGNAL_WEBSOCKET_FAILED], 0);
        em_conn_update_status(em_conn, EM_STATUS_WEBSOCKET_FAILED);
        return;
    }
    g_assert_no_error(error);

#ifndef USE_WEBRTC
    em_conn->status = EM_STATUS_CONNECTED;
#endif

    ALOGI("WebSocket connected");
    g_signal_connect(em_conn->ws, "message", G_CALLBACK(em_conn_on_ws_message_cb), em_conn);
    g_signal_emit(em_conn, signals[SIGNAL_WEBSOCKET_CONNECTED], 0);

    ALOGI("Creating pipeline");
    g_assert_null(em_conn->pipeline);
    g_signal_emit(em_conn, signals[SIGNAL_ON_NEED_PIPELINE], 0);
    if (em_conn->pipeline == NULL) {
        ALOGE("on-need-pipeline signal did not return a pipeline!");
        em_connection_disconnect(em_conn);
        return;
    }
    // OK, if we get here, we have a websocket connection, and a pipeline fully configured
    // so we can start the pipeline playing

    ALOGI("Setting pipeline state to PLAYING");
    gst_element_set_state(GST_ELEMENT(em_conn->pipeline), GST_STATE_PLAYING);
}

void em_connection_set_pipeline(EmConnection *em_conn, GstPipeline *pipeline) {
    g_assert_nonnull(pipeline);
    if (em_conn->pipeline) {
        // Stop old pipeline if applicable
        gst_element_set_state(GST_ELEMENT(em_conn->pipeline), GST_STATE_NULL);
    }
    gst_clear_object(&em_conn->pipeline);
    em_conn->pipeline = gst_object_ref_sink(pipeline);

#ifdef USE_WEBRTC
    em_conn_update_status(em_conn, EM_STATUS_NEGOTIATING);

    em_conn->webrtcbin = gst_bin_get_by_name(GST_BIN(em_conn->pipeline), "webrtc");
    g_assert_nonnull(em_conn->webrtcbin);
    g_assert(G_IS_OBJECT(em_conn->webrtcbin));

    g_signal_connect(em_conn->webrtcbin, "on-ice-candidate", G_CALLBACK(em_conn_webrtc_on_ice_candidate_cb), em_conn);
    g_signal_connect(em_conn->webrtcbin,
                     "prepare-data-channel",
                     G_CALLBACK(em_conn_webrtc_prepare_data_channel_cb),
                     em_conn);
    // Incoming streams will be exposed via this signal
    g_signal_connect(em_conn->webrtcbin, "pad-added", G_CALLBACK(em_conn_webrtc_on_incoming_stream), NULL);
    g_signal_connect(em_conn->webrtcbin, "on-data-channel", G_CALLBACK(em_conn_webrtc_on_data_channel_cb), em_conn);
    g_signal_connect(em_conn->webrtcbin,
                     "deep-notify::connection-state",
                     G_CALLBACK(em_conn_webrtc_deep_notify_callback),
                     em_conn);
    g_signal_connect(em_conn->webrtcbin,
                     "prepare-data-channel",
                     G_CALLBACK(em_conn_webrtc_on_prepare_data_channel),
                     NULL);
#endif
}

enum em_status em_connection_get_status(EmConnection *em_conn) {
    return em_conn->status;
}

static void em_conn_connect_internal(EmConnection *em_conn, enum em_status status) {
    em_connection_disconnect(em_conn);
    if (!em_conn->ws_cancel) {
        em_conn->ws_cancel = g_cancellable_new();
    }
    g_cancellable_reset(em_conn->ws_cancel);

    // Set connection timeout
    soup_session_set_timeout(em_conn->soup_session, 5);

    ALOGI("calling soup_session_websocket_connect_async. websocket_uri = %s", em_conn->websocket_uri);
#if SOUP_MAJOR_VERSION == 2
    soup_session_websocket_connect_async(em_conn->soup_session,                                     // session
                                         soup_message_new(SOUP_METHOD_GET, em_conn->websocket_uri), // message
                                         NULL,                                                      // origin
                                         NULL,                                                      // protocols
                                         em_conn->ws_cancel,                                        // cancellable
                                         (GAsyncReadyCallback)em_conn_websocket_connected_cb,       // callback
                                         em_conn);                                                  // user_data
#else
    soup_session_websocket_connect_async(em_conn->soup_session,                                     // session
                                         soup_message_new(SOUP_METHOD_GET, em_conn->websocket_uri), // message
                                         NULL,                                                      // origin
                                         NULL,                                                      // protocols
                                         0,                                                         // io_priority
                                         em_conn->ws_cancel,                                        // cancellable
                                         (GAsyncReadyCallback)em_conn_websocket_connected_cb,       // callback
                                         em_conn);                                                  // user_data

#endif
    em_conn_update_status(em_conn, status);
}

/* public (non-GObject) methods */

EmConnection *em_connection_new(const gchar *websocket_uri) {
    return EM_CONNECTION(g_object_new(EM_TYPE_CONNECTION, "websocket-uri", websocket_uri, NULL));
}

EmConnection *em_connection_new_localhost() {
    return EM_CONNECTION(g_object_new(EM_TYPE_CONNECTION, NULL));
}

void em_connection_connect(EmConnection *em_conn) {
    em_conn_connect_internal(em_conn, EM_STATUS_CONNECTING);
}

void em_connection_disconnect(EmConnection *em_conn) {
    em_conn_disconnect_internal(em_conn, EM_STATUS_IDLE_NOT_CONNECTED);
}

bool em_connection_send_bytes(EmConnection *em_conn, GBytes *bytes) {
    if (em_conn->status != EM_STATUS_CONNECTED) {
        ALOGW("Cannot send bytes when status is %s", em_status_to_string(em_conn->status));
        return false;
    }

#ifdef USE_WEBRTC
    gboolean success = gst_webrtc_data_channel_send_data_full(em_conn->datachannel, bytes, NULL);
    return success == TRUE;
#else
    gsize length = 0;
    const gchar *msg_data = g_bytes_get_data(bytes, &length);
    soup_websocket_connection_send_binary(em_conn->ws, msg_data, length);
    return TRUE;
#endif
}
