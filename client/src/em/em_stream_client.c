// Copyright 2020-2023, Collabora, Ltd.
// Copyright 2022-2023, PlutoVR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pipeline module ElectricMaple XR streaming solution
 * @author Rylie Pavlik <rpavlik@collabora.com>
 * @ingroup em_client
 */

#include "em_stream_client.h"

// clang-format off
#include "render/xr_platform_deps.h"
#include "em/em_egl.h"
#include "em_app_log.h"
#include "em_connection.h"
#include "gst_common.h"
#include "os/os_threading.h"
// clang-format on

#include <electricmaple.pb.h>
#include <gst/app/gstappsink.h>
#include <gst/gl/gl.h>
#include <gst/gl/gstglsyncmeta.h>
#include <gst/gst.h>
#include <gst/gstbus.h>
#include <gst/gstelement.h>
#include <gst/gstinfo.h>
#include <gst/gstmessage.h>
#include <gst/gstsample.h>
#include <gst/gstutils.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/video/video-frame.h>
#include <gst/webrtc/webrtc.h>
#include <gst/net/gstnet.h>
#include <linux/time.h>
#include <openxr/openxr.h>
#include <pb_decode.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Thresholds for reusing last DownMsg when we received too many frames without DownMsgs in a row.
#define EM_NO_DOWN_MSG_FALLBACK_TIMEOUT_SECS 1
#define EM_NO_DOWN_MSG_FALLBACK_SKIPPED_FRAME_THRESHOLD 10

struct em_sc_sample
{
	struct em_sample base;
	GstSample *sample;
};

struct _EmStreamClient
{
	GMainLoop *loop;
	EmConnection *connection;
	GstElement *pipeline;
	GstGLDisplay *gst_gl_display;
	GstGLContext *gst_gl_context;
	GstGLContext *gst_gl_other_context;

	GstGLDisplay *display;

	/// Wrapped version of the android_main/render context
	GstGLContext *android_main_context;

	/// GStreamer-created EGL context for its own use
	GstGLContext *context;

	GstElement *appsink;

	GLenum frame_texture_target;
	GLenum texture_target;
	GLuint texture_id;

	int width;
	int height;

	struct
	{
		EGLDisplay display;
		EGLContext android_main_context;
		// 16x16 pbuffer surface
		EGLSurface surface;
	} egl;

	bool own_egl_mutex;
	EmEglMutexIface *egl_mutex;

	struct os_thread_helper play_thread;

	bool pipeline_is_running;
	bool received_first_frame;

	GMutex sample_mutex;
	GstSample *sample;
	int64_t sample_decode_end_time;

	GMutex skipped_frames_mutex;
	uint32_t skipped_frames;
	em_proto_DownMessage last_down_msg;

	// todo: this may not be an ideal way to handle data racing
	GMutex metadata_preservation_mutex;
	GstBuffer *preserved_metadata_struct_buf;

	GArray *latency_collection;
	int64_t latency_calculation_window; // Nanoseconds
	int64_t latency_last_time_query;

	int64_t average_latency; // Nanoseconds
	int max_jitter_latency;
};

#if 0
G_DEFINE_TYPE(EmStreamClient, em_stream_client, G_TYPE_OBJECT);

enum
{
    // action signals
    // SIGNAL_CONNECT,
    // SIGNAL_DISCONNECT,
    // SIGNAL_SET_PIPELINE,
    // signals
    // SIGNAL_WEBSOCKET_CONNECTED,
    // SIGNAL_WEBSOCKET_FAILED,
    // SIGNAL_CONNECTED,
    // SIGNAL_STATUS_CHANGE,
    // SIGNAL_ON_NEED_PIPELINE,
    // SIGNAL_ON_DROP_PIPELINE,
    // SIGNAL_DISCONNECTED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef enum
{
    PROP_CONNECTION = 1,
    // PROP_STATUS,
    N_PROPERTIES
} EmStreamClientProperty;
#endif

#define RTP_TWOBYTES_HDR_EXT_ID 1 // Must be in the [1,15] range

// clang-format off
#define SINK_CAPS \
    "video/x-raw(" GST_CAPS_FEATURE_MEMORY_GL_MEMORY "), "              \
    "format = (string) RGBA, "                                          \
    "width = " GST_VIDEO_SIZE_RANGE ", "                                \
    "height = " GST_VIDEO_SIZE_RANGE ", "                               \
    "framerate = " GST_VIDEO_FPS_RANGE ", "                             \
    "texture-target = (string) { 2D, external-oes } "

// clang-format on

/*
 * callbacks
 */

static void
on_need_pipeline_cb(EmConnection *em_conn, EmStreamClient *sc);

static void
on_drop_pipeline_cb(EmConnection *em_conn, EmStreamClient *sc);

static void *
em_stream_client_thread_func(void *ptr);

/*
 * Helper functions
 */

static void
em_stream_client_set_connection(EmStreamClient *sc, EmConnection *connection);

static void
em_stream_client_free_egl_mutex(EmStreamClient *sc);

/* GObject method implementations */

#if 0

static void
em_stream_client_set_property(GObject *object, guint property_id, const GValue *value, GParamSpec *pspec)
{
    switch ((EmStreamClientProperty)property_id) {

    case PROP_CONNECTION:
        em_stream_client_set_connection(EM_STREAM_CLIENT(object), EM_CONNECTION(g_value_get_object(value)));
        break;

    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec); break;
    }
}

static void
em_stream_client_get_property(GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{

    switch ((EmStreamClientProperty)property_id) {
    case PROP_CONNECTION: g_value_set_object(value, EM_STREAM_CLIENT(object)->connection); break;

    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec); break;
    }
}

#endif

static void
em_stream_client_init(EmStreamClient *sc)
{
	ALOGI("%s: creating stuff", __FUNCTION__);

	guint major, minor, micro, nano;
	gst_version(&major, &minor, &micro, &nano);
	ALOGI("GStreamer version %d %d %d %d", major, minor, micro, nano);

	memset(sc, 0, sizeof(EmStreamClient));
	sc->loop = g_main_loop_new(NULL, FALSE);
	g_assert(os_thread_helper_init(&sc->play_thread) >= 0);
	g_mutex_init(&sc->sample_mutex);

	const gchar *tags[] = {NULL};
	const GstMetaInfo *info = gst_meta_register_custom("down-message", tags, NULL, NULL, NULL);
	if (info == NULL) {
		ALOGE("Failed to register custom meta 'down-message'.");
	}

	g_mutex_init(&sc->skipped_frames_mutex);
	sc->skipped_frames = 0;

	g_mutex_init(&sc->metadata_preservation_mutex);
	sc->preserved_metadata_struct_buf = NULL;

	sc->latency_collection = g_array_new(FALSE, FALSE, sizeof(gint64));
	sc->latency_calculation_window = time_s_to_ns(3);
	sc->latency_last_time_query = os_monotonic_get_ns();

	ALOGI("%s: done creating stuff", __FUNCTION__);
}

static void
em_stream_client_dispose(EmStreamClient *self)
{
	// May be called multiple times during destruction.
	// Stop things and clear ref counted things here.
	// EmStreamClient *self = EM_STREAM_CLIENT(object);
	em_stream_client_stop(self);
	g_clear_object(&self->loop);
	g_clear_object(&self->connection);
	gst_clear_object(&self->sample);
	gst_clear_object(&self->pipeline);
	gst_clear_object(&self->gst_gl_display);
	gst_clear_object(&self->gst_gl_context);
	gst_clear_object(&self->gst_gl_other_context);
	gst_clear_object(&self->display);
	gst_clear_object(&self->context);
	gst_clear_object(&self->appsink);
}

static void
em_stream_client_finalize(EmStreamClient *self)
{
	// only called once, after dispose
	// EmStreamClient *self = EM_STREAM_CLIENT(object);
	os_thread_helper_destroy(&self->play_thread);
	em_stream_client_free_egl_mutex(self);
}

#if 0
static void
em_stream_client_class_init(EmStreamClientClass *klass)
{
    ALOGE("%s: Begin", __FUNCTION__);

    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

    gobject_class->dispose = em_stream_client_dispose;
    gobject_class->finalize = em_stream_client_finalize;

    // gobject_class->set_property = em_stream_client_set_property;
    // gobject_class->get_property = em_stream_client_get_property;

    /**
     * EmStreamClient:connection:
     *
     * The websocket URI for the signaling server
     */
    // g_object_class_install_property(
    //     gobject_class, PROP_CONNECTION,
    //     g_param_spec_object("connection", "Connection", "EmConnection object for XR streaming",
    //     EM_TYPE_CONNECTION,
    //                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    ALOGE("%s: End", __FUNCTION__);
}

#endif

static inline XrQuaternionf
quat_to_openxr(const em_proto_Quaternion *q)
{
	return (XrQuaternionf){q->x, q->y, q->z, q->w};
}
static inline XrVector3f
vec3_to_openxr(const em_proto_Vec3 *v)
{
	return (XrVector3f){v->x, v->y, v->z};
}

static inline XrPosef
pose_to_openxr(const em_proto_Pose *p)
{
	return (XrPosef){
	    p->has_orientation ? quat_to_openxr(&p->orientation) : (XrQuaternionf){0, 0, 0, 1},
	    p->has_position ? vec3_to_openxr(&p->position) : (XrVector3f){0, 0, 0},
	};
}

/*
 * callbacks
 */

static GstBusSyncReply
bus_sync_handler_cb(GstBus *bus, GstMessage *msg, EmStreamClient *sc)
{
	// LOG_MSG(msg);

	/* Do not let GstGL retrieve the display handle on its own
	 * because then it believes it owns it and calls eglTerminate()
	 * when disposed */
	if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_NEED_CONTEXT) {
		const gchar *type;
		gst_message_parse_context_type(msg, &type);
		if (g_str_equal(type, GST_GL_DISPLAY_CONTEXT_TYPE)) {
			ALOGI("Got message: Need display context");
			g_autoptr(GstContext) context = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
			gst_context_set_gl_display(context, sc->display);
			gst_element_set_context(GST_ELEMENT(msg->src), context);
		} else if (g_str_equal(type, "gst.gl.app_context")) {
			ALOGI("Got message: Need app context");
			g_autoptr(GstContext) app_context = gst_context_new("gst.gl.app_context", TRUE);
			GstStructure *s = gst_context_writable_structure(app_context);
			gst_structure_set(s, "context", GST_TYPE_GL_CONTEXT, sc->android_main_context, NULL);
			gst_element_set_context(GST_ELEMENT(msg->src), app_context);
		}
	}

	return GST_BUS_PASS;
}

static gboolean
gst_bus_cb(GstBus *bus, GstMessage *message, gpointer data)
{
	// LOG_MSG(message);

	GstBin *pipeline = GST_BIN(data);

	switch (GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_STATE_CHANGED:
		GstState old_state, new_state;

		// The pipeline's state has changed.
		gst_message_parse_state_changed(message, &old_state, &new_state, NULL);

		if (GST_MESSAGE_SRC(message) == GST_OBJECT(pipeline) && new_state == GST_STATE_PLAYING) {
			// The pipeline is now playing. This is a good time to check if the clock has synced.
			GstClock *clock = gst_pipeline_get_clock(GST_PIPELINE(pipeline));
			if (gst_clock_is_synced(clock)) {
				ALOGI("Clock synchronized! Proceeding with operations");
				// Now your application can start doing work that depends on a synced clock.
			} else {
				ALOGW("Pipeline is PLAYING, but clock not yet synchronized. Waiting...");
			}
		}
		break;
	case GST_MESSAGE_ERROR: {
		GError *gerr = NULL;
		gchar *debug_msg = NULL;
		gst_message_parse_error(message, &gerr, &debug_msg);

		// Output pipeline dot file
		//		GST_DEBUG_BIN_TO_DOT_FILE(pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-error");
		// Print pipeline dot file
		//		gchar *dotdata = gst_debug_bin_to_dot_data(pipeline, GST_DEBUG_GRAPH_SHOW_ALL);
		//		ALOGE("gst_bus_cb: DOT data: %s", dotdata);

		ALOGE("gst_bus_cb: Error: %s (%s)", gerr->message, debug_msg);
		g_error("gst_bus_cb: Error: %s (%s)", gerr->message, debug_msg);

		g_error_free(gerr);
		g_free(debug_msg);
	} break;
	case GST_MESSAGE_WARNING: {
		GError *gerr = NULL;
		gchar *debug_msg = NULL;
		gst_message_parse_warning(message, &gerr, &debug_msg);
		GST_DEBUG_BIN_TO_DOT_FILE(pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "pipeline-warning");
		ALOGW("gst_bus_cb: Warning: %s (%s)", gerr->message, debug_msg);
		g_warning("gst_bus_cb: Warning: %s (%s)", gerr->message, debug_msg);
		g_error_free(gerr);
		g_free(debug_msg);
	} break;
	case GST_MESSAGE_EOS: {
		g_error("gst_bus_cb: Got EOS!");
	} break;
	case GST_MESSAGE_LATENCY: {
		g_warning("Handling latency");
		gst_bin_recalculate_latency(pipeline);
	} break;
	default: break;
	}
	return TRUE;
}

static GstFlowReturn
on_new_sample_cb(GstAppSink *appsink, gpointer user_data)
{
	EmStreamClient *sc = (EmStreamClient *)user_data;

	int64_t decode_end_time = os_monotonic_get_ns();

	// TODO record the frame ID, get frame pose

	GstSample *prev_sample = NULL;
	GstSample *sample = gst_app_sink_pull_sample(appsink);
	g_assert_nonnull(sample);

	// Drop it like it's hot
	bool drop_frame = false;

	GstBuffer *buffer = gst_sample_get_buffer(sample);
	GstCustomMeta *custom_meta = gst_buffer_get_custom_meta(buffer, "down-message");
	if (!custom_meta) {
		ALOGW("sample_cb: Dropping buffer without down-message.");
		//		drop_frame = true;
	}

	int64_t last_frame_diff_ns = decode_end_time - sc->sample_decode_end_time;
	if (last_frame_diff_ns >= time_s_to_ns(EM_NO_DOWN_MSG_FALLBACK_TIMEOUT_SECS)) {
		ALOGW("sample_cb: Not dropping frame, since we haven't had one since a second.");
		drop_frame = false;
	}

	bool skipped_frames_threshold_reached = false;
	{
		g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&sc->skipped_frames_mutex);
		if (sc->skipped_frames >= EM_NO_DOWN_MSG_FALLBACK_SKIPPED_FRAME_THRESHOLD) {
			skipped_frames_threshold_reached = true;
		}
	}

	if (skipped_frames_threshold_reached) {
		//		ALOGW("sample_cb: Not dropping frame, since we already skipped 10 frames.");
		drop_frame = false;
	}

	if (drop_frame) {
		return GST_FLOW_OK;
	}

	{
		g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&sc->sample_mutex);
		prev_sample = sc->sample;
		sc->sample = sample;
		sc->sample_decode_end_time = decode_end_time;
		sc->received_first_frame = true;
	}

	// Release the previous sample.
	if (prev_sample) {
		gst_sample_unref(prev_sample);
	}

	return GST_FLOW_OK;
}

static GstPadProbeReturn
rtpdepay_sink_pad_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
	EmStreamClient *sc = (EmStreamClient *)user_data;

	int64_t frame_receive_time = os_monotonic_get_ns();

	g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&sc->metadata_preservation_mutex);

	// Is not yet consumed.
	if (sc->preserved_metadata_struct_buf) {
		return GST_PAD_PROBE_OK;
	}

	GstBuffer *buffer = gst_pad_probe_info_get_buffer(info);

	GstRTPBuffer rtp_buffer = GST_RTP_BUFFER_INIT;

	// Extract Downstream metadata from rtp header
	if (!gst_rtp_buffer_map(buffer, GST_MAP_READWRITE, &rtp_buffer)) {
		ALOGE("Failed to map GstBuffer");
		return GST_PAD_PROBE_OK;
	}

	// Not all buffers has extension data attached, check.
	if (!gst_rtp_buffer_get_extension(&rtp_buffer)) {
		// TODO: This happens for most RTP buffers we receive as they are not ours.
		// Is there a smarter way to filter them?
		ALOGE("Skipping RTP buffer without extension bit.");
		goto no_buf;
	}

	guint size = 0;
	gpointer payload_ptr;
	if (!gst_rtp_buffer_get_extension_twobytes_header(&rtp_buffer, NULL, RTP_TWOBYTES_HDR_EXT_ID,
	                                                  0 /* NOTE: We do not support multi-extension-elements.*/,
	                                                  &payload_ptr, &size)) {
		ALOGE("Could not retrieve twobyte rtp extension on buffer!");
		goto no_buf;
	}

	// Repack the protobuf into a GstBuffer
	sc->preserved_metadata_struct_buf = gst_buffer_new_memdup(payload_ptr, size);
	if (!sc->preserved_metadata_struct_buf) {
		ALOGE("Failed to allocate GstBuffer with payload.");
		goto no_buf;
	}

	gst_rtp_buffer_unmap(&rtp_buffer);
	return GST_PAD_PROBE_OK;

no_buf:
	gst_rtp_buffer_unmap(&rtp_buffer);
	return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
rtpdepay_src_pad_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
	EmStreamClient *sc = (EmStreamClient *)user_data;

	g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&sc->metadata_preservation_mutex);

	if (!sc->preserved_metadata_struct_buf) {
		ALOGE("preserved_metadata_struct_buf is NULL");
		return GST_PAD_PROBE_OK;
	}

	int64_t frame_receive_time = os_monotonic_get_ns();

	GstBuffer *buffer = gst_pad_probe_info_get_buffer(info);

	// Add it to a custom meta
	GstCustomMeta *custom_meta = gst_buffer_add_custom_meta(buffer, "down-message");
	if (custom_meta == NULL) {
		ALOGE("Failed to add GstCustomMeta");
		gst_buffer_unref(sc->preserved_metadata_struct_buf);
		sc->preserved_metadata_struct_buf = NULL;
		return GST_PAD_PROBE_OK;
	}

	GstStructure *custom_structure = gst_custom_meta_get_structure(custom_meta);
	gst_structure_set(custom_structure, "protobuf", GST_TYPE_BUFFER, sc->preserved_metadata_struct_buf, NULL);

	// Set the frame receive time
	GValue frame_receive_time_value = G_VALUE_INIT;
	g_value_init(&frame_receive_time_value, G_TYPE_INT64);
	g_value_set_int64(&frame_receive_time_value, frame_receive_time);
	gst_structure_set_value(custom_structure, "frame-receive-time", &frame_receive_time_value);

	gst_buffer_unref(sc->preserved_metadata_struct_buf);
	sc->preserved_metadata_struct_buf = NULL;

	return GST_PAD_PROBE_OK;
}

static void
on_new_transceiver(GstElement *webrtc, GstWebRTCRTPTransceiver *trans)
{
	g_object_set(trans, "fec-type", GST_WEBRTC_FEC_TYPE_ULP_RED, NULL);
}

static GstPadProbeReturn
jitterbuffer_event_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
	if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_DATA(info)) == GST_EVENT_CUSTOM_DOWNSTREAM) {
		const GstStructure *s = gst_event_get_structure(GST_EVENT(GST_PAD_PROBE_INFO_DATA(info)));
		if (gst_structure_has_name(s, "GstRTPPacketLost")) {
			guint16 seqnum;
			gst_structure_get_uint(s, "seqnum", &seqnum);
			ALOGW("Packet lost detected, seqnum: %u\n", seqnum);
		}
	}
	return GST_PAD_PROBE_OK;
}

static void
on_need_pipeline_cb(EmConnection *em_conn, EmStreamClient *sc)
{
	g_assert_nonnull(sc);
	g_assert_nonnull(em_conn);
	GError *error = NULL;

	// We'll need an active egl context below before setting up gstgl (as explained previously)
	if (!em_stream_client_egl_begin_pbuffer(sc)) {
		ALOGE("%s: Failed to make EGL context current, cannot create pipeline!", __FUNCTION__);
		return;
	}

#ifdef USE_WEBRTC
	// clang-format off
    gchar *pipeline_string = g_strdup_printf(
            "webrtcbin name=webrtc bundle-policy=max-bundle latency=50 ! "
            "rtph264depay name=depay ! "
            "decodebin3 ! "
            "glsinkbin name=glsink");
	// clang-format on
#else
	// clang-format off
	gchar *pipeline_string = g_strdup_printf(
		"udpsrc port=5601 buffer-size=8000000 "
		"caps=\"application/x-rtp,media=audio\" ! "
		"rtpopusdepay ! "
		"opusdec ! "
		"openslessink "

	    "udpsrc port=5600 buffer-size=8000000 "
	    "caps=\"application/x-rtp,media=video,clock-rate=90000,encoding-name=H264\" ! "
	    "rtpjitterbuffer name=jitter do-lost=1 latency=50 ! "
	    "rtph264depay name=depay ! "
	#ifndef USE_DECODEBIN3
	    "h264parse ! "
	    "amcviddec-c2mtkavcdecoder ! "
	    "video/x-raw(memory:GLMemory),format=(string)RGBA,texture-target=(string)external-oes ! "
	#else
	    "decodebin3 ! "
	#endif
	    "glsinkbin name=glsink");
	// clang-format on
#endif

	sc->pipeline = gst_object_ref_sink(gst_parse_launch(pipeline_string, &error));
	if (sc->pipeline == NULL) {
		ALOGE("FRED: Failed creating pipeline : Bad source: %s", error->message);
		abort();
	}
	if (error) {
		ALOGE("Error creating a pipeline from string: %s", error ? error->message : "Unknown");
	}

	GstClock *client_clock = gst_net_client_clock_new("my-client-clock", DEFAULT_SERVER_IP, 52357, 0);
	gst_pipeline_use_clock(GST_PIPELINE(sc->pipeline), client_clock);

#ifdef USE_WEBRTC
	GstElement *webrtcbin = gst_bin_get_by_name(GST_BIN(sc->pipeline), "webrtc");
	g_signal_connect(webrtcbin, "on-new-transceiver", G_CALLBACK(on_new_transceiver), NULL);
	gst_object_unref(webrtcbin);
#else
	{
		GstElement *jitterbuffer = gst_bin_get_by_name(GST_BIN(sc->pipeline), "jitter");

		if (jitterbuffer) {
			GstPad *srcpad = gst_element_get_static_pad(jitterbuffer, "src");
			g_assert(srcpad);
			//        gst_pad_add_probe(srcpad, GST_PAD_PROBE_TYPE_BUFFER,
			//        jitterbuffer_event_probe_cb, NULL, NULL);
			gst_clear_object(&srcpad);

			gst_object_unref(jitterbuffer);
		}
	}
#endif

	// Un-current the EGL context
	em_stream_client_egl_end(sc);

	// We convert the string SINK_CAPS above into a GstCaps that elements below can understand.
	// the "video/x-raw(" GST_CAPS_FEATURE_MEMORY_GL_MEMORY ")," part of the caps is read :
	// video/x-raw(memory:GLMemory) and is really important for getting zero-copy gl textures.
	// It tells the pipeline (especially the decoder) that an internal android:Surface should
	// get created internally (using the provided gstgl contexts above) so that the appsink
	// can basically pull the samples out using an GLConsumer (this is just for context, as
	// all of those constructs will be hidden from you, but are turned on by that CAPS).
	g_autoptr(GstCaps) caps = gst_caps_from_string(SINK_CAPS);

	// FRED: We create the appsink 'manually' here because glsink's ALREADY a sink and so if we stick
	//       glsinkbin ! appsink in our pipeline_string for automatic linking, gst_parse will NOT like this,
	//       as glsinkbin (a sink) cannot link to anything upstream (appsink being 'another' sink). So we
	//       manually link them below using glsinkbin's 'sink' pad -> appsink.
	sc->appsink = gst_element_factory_make("appsink", NULL);
	g_object_set(sc->appsink,
	             // Set caps
	             "caps", caps,
	             // Fixed size buffer
	             "max-buffers", 1,
	             // drop old buffers when queue is filled
	             "drop", true,
	             // terminator
	             NULL);
	// Lower overhead than new-sample signal.
	GstAppSinkCallbacks callbacks = {0};
	callbacks.new_sample = on_new_sample_cb;
	gst_app_sink_set_callbacks(GST_APP_SINK(sc->appsink), &callbacks, sc, NULL);
	sc->received_first_frame = false;

	g_autoptr(GstElement) glsinkbin = gst_bin_get_by_name(GST_BIN(sc->pipeline), "glsink");
	g_object_set(glsinkbin, "sink", sc->appsink, NULL);
	// Disable clock sync to reduce latency
	g_object_set(glsinkbin, "sync", FALSE, NULL);

	g_autoptr(GstBus) bus = gst_element_get_bus(sc->pipeline);
	// We set this up to inject the EGL context
	gst_bus_set_sync_handler(bus, (GstBusSyncHandler)bus_sync_handler_cb, sc, NULL);

	// This just watches for errors and such
	gst_bus_add_watch(bus, gst_bus_cb, sc->pipeline);
	g_object_unref(bus);

	sc->pipeline_is_running = TRUE;

	GstElement *depay = gst_bin_get_by_name(GST_BIN(sc->pipeline), "depay");
	{
		GstPad *pad = gst_element_get_static_pad(depay, "sink");
		if (pad != NULL) {
			gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, rtpdepay_sink_pad_probe, sc, NULL);
			gst_object_unref(pad);
		} else {
			ALOGE("Could not find static sink pad in depay");
		}
	}
	{
		GstPad *pad = gst_element_get_static_pad(depay, "src");
		if (pad != NULL) {
			gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, rtpdepay_src_pad_probe, sc, NULL);
			gst_object_unref(pad);
		} else {
			ALOGE("Could not find static src pad in depay");
		}
	}
	gst_object_unref(depay);

	// This actually hands over the pipeline. Once our own handler returns, the pipeline will be started by the
	// connection.
	g_signal_emit_by_name(em_conn, "set-pipeline", GST_PIPELINE(sc->pipeline), NULL);
}

static void
on_drop_pipeline_cb(EmConnection *em_conn, EmStreamClient *sc)
{
	if (sc->pipeline) {
		gst_element_set_state(sc->pipeline, GST_STATE_NULL);
	}
	gst_clear_object(&sc->pipeline);
	gst_clear_object(&sc->appsink);
}

static void *
em_stream_client_thread_func(void *ptr)
{
	EmStreamClient *sc = (EmStreamClient *)ptr;

	ALOGI("%s: running GMainLoop", __FUNCTION__);
	g_main_loop_run(sc->loop);
	ALOGI("%s: g_main_loop_run returned", __FUNCTION__);

	return NULL;
}

/*
 * Public functions
 */
EmStreamClient *
em_stream_client_new()
{
#if 0
    ALOGI("%s: before g_object_new", __FUNCTION__);
    gpointer self_untyped = g_object_new(EM_TYPE_STREAM_CLIENT, NULL);
    if (self_untyped == NULL) {
        ALOGE("%s: g_object_new failed to allocate", __FUNCTION__);
        return NULL;
    }
    EmStreamClient *self = EM_STREAM_CLIENT(self_untyped);

    ALOGI("%s: after g_object_new", __FUNCTION__);
#endif
	EmStreamClient *self = calloc(1, sizeof(EmStreamClient));
	em_stream_client_init(self);
	return self;
}

void
em_stream_client_destroy(EmStreamClient **ptr_sc)
{
	if (ptr_sc == NULL) {
		return;
	}
	EmStreamClient *sc = *ptr_sc;
	if (sc == NULL) {
		return;
	}
	em_stream_client_dispose(sc);
	em_stream_client_finalize(sc);
	free(sc);
	*ptr_sc = NULL;
}

void
em_stream_client_set_egl_context(EmStreamClient *sc,
                                 EmEglMutexIface *egl_mutex,
                                 bool adopt_mutex_interface,
                                 EGLSurface pbuffer_surface)
{
	em_stream_client_free_egl_mutex(sc);
	sc->own_egl_mutex = adopt_mutex_interface;
	sc->egl_mutex = egl_mutex;

	if (!em_egl_mutex_begin(sc->egl_mutex, EGL_NO_SURFACE, EGL_NO_SURFACE)) {
		ALOGV("em_stream_client_set_egl_context: Failed to make egl context current");
		return;
	}
	ALOGI("wrapping egl context");

	sc->egl.display = egl_mutex->display;
	sc->egl.android_main_context = egl_mutex->context;
	sc->egl.surface = pbuffer_surface;

	const GstGLPlatform egl_platform = GST_GL_PLATFORM_EGL;
	guintptr android_main_egl_context_handle = gst_gl_context_get_current_gl_context(egl_platform);
	GstGLAPI gl_api = gst_gl_context_get_current_gl_api(egl_platform, NULL, NULL);
	sc->gst_gl_display = g_object_ref_sink(gst_gl_display_new());
	sc->android_main_context = g_object_ref_sink(
	    gst_gl_context_new_wrapped(sc->gst_gl_display, android_main_egl_context_handle, egl_platform, gl_api));

	ALOGV("eglMakeCurrent un-make-current");
	em_egl_mutex_end(sc->egl_mutex);
}

bool
em_stream_client_egl_begin(EmStreamClient *sc, EGLSurface draw, EGLSurface read)
{
	return em_egl_mutex_begin(sc->egl_mutex, draw, read);
}

bool
em_stream_client_egl_begin_pbuffer(EmStreamClient *sc)
{
	return em_egl_mutex_begin(sc->egl_mutex, sc->egl.surface, sc->egl.surface);
}

void
em_stream_client_egl_end(EmStreamClient *sc)
{
	// ALOGI("%s: Make egl context un-current", __FUNCTION__);
	em_egl_mutex_end(sc->egl_mutex);
}

void
em_stream_client_spawn_thread(EmStreamClient *sc, EmConnection *connection)
{
	ALOGI("%s: Starting stream client mainloop thread", __FUNCTION__);
	em_stream_client_set_connection(sc, connection);
	int ret = os_thread_helper_start(&sc->play_thread, &em_stream_client_thread_func, sc);
	(void)ret;
	g_assert(ret == 0);
}

void
em_stream_client_stop(EmStreamClient *sc)
{
	ALOGI("%s: Stopping pipeline and ending thread", __FUNCTION__);

	if (sc->pipeline != NULL) {
		gst_element_set_state(sc->pipeline, GST_STATE_NULL);
		os_thread_helper_stop_and_wait(&sc->play_thread);
	}
	if (sc->connection != NULL) {
		em_connection_disconnect(sc->connection);
	}
	gst_clear_object(&sc->pipeline);
	gst_clear_object(&sc->appsink);
	gst_clear_object(&sc->context);

	sc->pipeline_is_running = false;
}

static bool
read_down_message_from_custom_meta(GstBuffer *buffer, em_proto_DownMessage *msg, int64_t *out_frame_receive_time)
{
	GstCustomMeta *custom_meta = gst_buffer_get_custom_meta(buffer, "down-message");
	if (!custom_meta) {
		ALOGE("Failed to get custom meta from GstBuffer!");
		return false;
	}

	GstStructure *custom_structure = gst_custom_meta_get_structure(custom_meta);

	GstBuffer *struct_buf;
	if (!gst_structure_get(custom_structure, "protobuf", GST_TYPE_BUFFER, &struct_buf, NULL)) {
		ALOGE("Could not read protobuf from struct");
		return false;
	}

	GstMapInfo info;
	if (!gst_buffer_map(struct_buf, &info, GST_MAP_READ)) {
		ALOGE("Failed to map custom meta buffer.");
		return false;
	}

	const GValue *frame_receive_time_value = gst_structure_get_value(custom_structure, "frame-receive-time");
	if (G_VALUE_TYPE(frame_receive_time_value) == G_TYPE_INT64) {
		int64_t frame_receive_time = g_value_get_int64(frame_receive_time_value);
		*out_frame_receive_time = frame_receive_time;
	} else {
		ALOGE("Unexpected type for frame-receive-time");
	}

	pb_istream_t our_istream = pb_istream_from_buffer(info.data, info.size);
	bool result = pb_decode_ex(&our_istream, em_proto_DownMessage_fields, msg, PB_DECODE_NULLTERMINATED);

	gst_buffer_unmap(struct_buf, &info);

	if (!result) {
		ALOGE("Decoding protobuf with size %ld failed: %s", info.size, PB_GET_ERROR(&our_istream));
		return false;
	}

	return true;
}

gfloat
calculate_average_of_gint64(GArray *array)
{
	if (array->len == 0) {
		return 0.0f;
	}

	gint64 total_sum = 0;
	for (guint i = 0; i < array->len; i++) {
		total_sum += g_array_index(array, gint64, i);
	}

	// Perform a floating-point division to get the average
	return (gfloat)total_sum / (gfloat)array->len;
}

int64_t
em_stream_client_get_average_frame_latency(EmStreamClient *sc)
{
	int64_t ave_latency = calculate_average_of_gint64(sc->latency_collection);
	g_array_set_size(sc->latency_collection, 0);
	return ave_latency;
}

struct em_sample *
em_stream_client_try_pull_sample(EmStreamClient *sc, struct timespec *out_decode_end)
{
	if (!sc->appsink) {
		// not setup yet.
		return NULL;
	}

	// We actually pull the sample in the new-sample signal handler, so here we're just receiving the sample already
	// pulled.
	GstSample *sample = NULL;
	int64_t decode_end_time = 0;
	{
		g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&sc->sample_mutex);
		sample = sc->sample;
		sc->sample = NULL;
		decode_end_time = sc->sample_decode_end_time;
	}

	if (sample == NULL) {
		if (gst_app_sink_is_eos(GST_APP_SINK(sc->appsink))) {
			ALOGW("%s: EOS", __FUNCTION__);
			// TODO trigger teardown?
		}
		{
			g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&sc->skipped_frames_mutex);
			sc->skipped_frames += 1;
			//			ALOGW("pull_sample: The latest sample is NULL. Skipped %d frames.",
			// sc->skipped_frames);
		}
		return NULL;
	}

	{
		g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&sc->skipped_frames_mutex);
		sc->skipped_frames = 0;
	}

	os_ns_to_timespec(decode_end_time, out_decode_end);

	struct em_sc_sample *ret = calloc(1, sizeof(struct em_sc_sample));

	GstBuffer *buffer = gst_sample_get_buffer(sample);
	GstCaps *caps = gst_sample_get_caps(sample);

	// Get DownMsg from GstCustomMeta
	em_proto_DownMessage msg = em_proto_DownMessage_init_default;
	int64_t frame_receive_time = 0;
	if (!read_down_message_from_custom_meta(buffer, &msg, &frame_receive_time)) {
		ALOGE("Reading DownMessage from GstCustomMeta failed. Reusing last one");
		msg = sc->last_down_msg;
	}

	if (msg.has_frame_data && msg.frame_data.has_P_localSpace_view0 && msg.frame_data.has_P_localSpace_view1) {
		//		ALOGI("Got DownMessage: Frame #%ld V0 (%.2f %.2f %.2f) V1 (%.2f %.2f %.2f)
		// render_begin_time %ld", 		      msg.frame_data.frame_sequence_id,
		// msg.frame_data.P_localSpace_view0.position.x,
		// msg.frame_data.P_localSpace_view0.position.y, msg.frame_data.P_localSpace_view0.position.z,
		// msg.frame_data.P_localSpace_view1.position.x, msg.frame_data.P_localSpace_view1.position.y,
		// msg.frame_data.P_localSpace_view1.position.z, msg.frame_data.frame_push_time);

		GstClock *clock = gst_element_get_clock(sc->pipeline);
		const GstClockTime current_time = gst_clock_get_time(clock);
		GstClockTime base_time = gst_element_get_base_time(sc->pipeline);
		GstClockTime running_time = current_time - base_time;

		//		ALOGI("Client clock: system time %.3f pipeline time %.3f base time %.3f running time
		//%.3f", 		      time_ns_to_s(os_monotonic_get_ns()), time_ns_to_s(current_time),
		// time_ns_to_s(base_time), 		      time_ns_to_s(running_time));

		int64_t latency = current_time - msg.frame_data.frame_push_clock_time;
		//		ALOGI("Frame latency (server appsrc -> client glsinkbin): %.1f ms", latency / 1.0e6);

		g_array_append_val(sc->latency_collection, latency);

		int64_t now_ns = os_monotonic_get_ns();
		if (now_ns - sc->latency_last_time_query > sc->latency_calculation_window) {
			int64_t ave_latency = em_stream_client_get_average_frame_latency(sc);
			ALOGI("Average frame latency (server appsrc -> client glsinkbin): %.1f ms",
			      ave_latency / 1.0e6);

			sc->latency_last_time_query = now_ns;
			sc->average_latency = ave_latency;

			em_stream_client_adjust_jitterbuffer(sc);
		}

		ret->base.have_poses = true;
		ret->base.poses[0] = pose_to_openxr(&msg.frame_data.P_localSpace_view0);
		ret->base.poses[1] = pose_to_openxr(&msg.frame_data.P_localSpace_view1);

		ret->base.frame_sequence_id = msg.frame_data.frame_sequence_id;

		// Write frame begin time only if we can convert it to the client clock.
		int64_t server_clock_offset = em_connection_get_server_clock_offset(sc->connection);

		// In case we haven't got server_clock_offset from WebRTC datachannel.
		if (server_clock_offset == 0) {
			int64_t client_system_clock_pipeline_clock_offset = now_ns - current_time;
			server_clock_offset = client_system_clock_pipeline_clock_offset -
			                      msg.frame_data.server_system_clock_pipeline_clock_offset;

			//			ALOGI("client_system_clock_pipeline_clock_offset %ld now_ns %ld
			// current_time %ld", 			      client_system_clock_pipeline_clock_offset, now_ns,
			// current_time);
		}

		if (server_clock_offset != 0) {
			ret->base.server_render_begin_time = server_clock_offset + msg.frame_data.render_begin_time;
			ret->base.server_push_time = server_clock_offset + msg.frame_data.frame_push_time;
		}
		ret->base.client_receive_time = frame_receive_time;
		ret->base.client_decode_time = decode_end_time;

		sc->last_down_msg = msg;
	}

	ret->base.client_render_begin_time = os_monotonic_get_ns();

	GstVideoInfo info;
	gst_video_info_from_caps(&info, caps);
	gint width = GST_VIDEO_INFO_WIDTH(&info);
	gint height = GST_VIDEO_INFO_HEIGHT(&info);
	//	ALOGI("%s: frame %d (w) x %d (h)", __FUNCTION__, width, height);

	// TODO: Handle resize?
#if 0
    if (width != sc->width || height != sc->height) {
        sc->width = width;
        sc->height = height;
    }
#endif

	GstVideoFrame frame;
	GstMapFlags flags = (GstMapFlags)(GST_MAP_READ | GST_MAP_GL);
	gst_video_frame_map(&frame, &info, buffer, flags);
	ret->base.frame_texture_id = *(GLuint *)frame.data[0];

	if (sc->context == NULL) {
		ALOGI("%s: Retrieving the GStreamer EGL context", __FUNCTION__);
		/* Get GStreamer's gl context. */
		gst_gl_query_local_gl_context(sc->appsink, GST_PAD_SINK, &sc->context);

		/* Check if we have 2D or OES textures */
		GstStructure *s = gst_caps_get_structure(caps, 0);
		const gchar *texture_target_str = gst_structure_get_string(s, "texture-target");
		if (g_str_equal(texture_target_str, GST_GL_TEXTURE_TARGET_EXTERNAL_OES_STR)) {
			sc->frame_texture_target = GL_TEXTURE_EXTERNAL_OES;
		} else if (g_str_equal(texture_target_str, GST_GL_TEXTURE_TARGET_2D_STR)) {
			sc->frame_texture_target = GL_TEXTURE_2D;
			ALOGE("Got GL_TEXTURE_2D instead of expected GL_TEXTURE_EXTERNAL_OES");
		} else {
			g_assert_not_reached();
		}
	}
	ret->base.frame_texture_target = sc->frame_texture_target;

	GstGLSyncMeta *sync_meta = gst_buffer_get_gl_sync_meta(buffer);
	if (sync_meta) {
		/* MOSHI: the set_sync() seems to be needed for resizing */
		gst_gl_sync_meta_set_sync_point(sync_meta, sc->context);
		gst_gl_sync_meta_wait(sync_meta, sc->context);
	}

	gst_video_frame_unmap(&frame);
	// Move sample ownership into the return value
	ret->sample = sample;

	return (struct em_sample *)ret;
}

void
em_stream_client_release_sample(EmStreamClient *sc, struct em_sample *ems)
{
	struct em_sc_sample *impl = (struct em_sc_sample *)ems;
	//	ALOGI("Releasing sample with texture ID %d", ems->frame_texture_id);
	gst_sample_unref(impl->sample);
	free(impl);
}

/*
 * Helper functions
 */

static void
em_stream_client_set_connection(EmStreamClient *sc, EmConnection *connection)
{
	g_clear_object(&sc->connection);
	if (connection != NULL) {
		sc->connection = g_object_ref(connection);
		g_signal_connect(sc->connection, "on-need-pipeline", G_CALLBACK(on_need_pipeline_cb), sc);
		g_signal_connect(sc->connection, "on-drop-pipeline", G_CALLBACK(on_drop_pipeline_cb), sc);
		ALOGI("%s: EmConnection assigned", __FUNCTION__);
	}
}

static void
em_stream_client_free_egl_mutex(EmStreamClient *sc)
{
	if (sc->own_egl_mutex) {
		em_egl_mutex_destroy(&sc->egl_mutex);
	}
	sc->egl_mutex = NULL;
}

void
em_stream_client_adjust_jitterbuffer(EmStreamClient *sc)
{
	int target_jitter_latency = time_ns_to_ms_f(sc->average_latency) * 1.5f;
	sc->max_jitter_latency = MAX(sc->max_jitter_latency - 10, target_jitter_latency);

	g_autoptr(GstElement) jitterbuffer = gst_bin_get_by_name(GST_BIN(sc->pipeline), "jitter");
	g_object_set(jitterbuffer, "latency", sc->max_jitter_latency, NULL);

	ALOGI("jitterbuffer latency changed to %d ms", sc->max_jitter_latency);

	// We'll do gst_bin_recalculate_latency() in gst_bus_cb()
}
