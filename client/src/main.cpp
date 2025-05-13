// Copyright 2023, Pluto VR, Inc.
//
// SPDX-License-Identifier: BSL-1.0

/*!
 * @file
 * @brief Main file for WebRTC client.
 * @author Moshi Turner <moses@collabora.com>
 * @author Rylie Pavlik <rpavlik@collabora.com>
 */
#include "EglData.hpp"
#include "em/em_egl.h"
#include "em/em_remote_experience.h"
#include "em/render/xr_platform_deps.h"


#include "em/em_app_log.h"
#include "em/em_connection.h"
#include "em/em_stream_client.h"
#include "em/gst_common.h"
#include "em/render/render.hpp"

#include "os/os_time.h"
#include "util/u_time.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES3/gl32.h>

#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android_native_app_glue.h>

#include <gst/gst.h>

#include <memory>
#include <vector>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <pthread.h>
#include <jni.h>
#include <errno.h>
#include <stdbool.h>
#include <thread>
#include <unistd.h>

#include <array>
#include <assert.h>
#include <cmath>
#include <cstdlib>
#include <ctime>


#define XR_LOAD(fn) xrGetInstanceProcAddr(_state.instance, #fn, (PFN_xrVoidFunction *)&fn);

namespace {

struct em_state
{
	bool connected;

	XrInstance instance;
	XrSystemId system;
	XrSession session;
	XrSessionState sessionState;

	uint32_t width;
	uint32_t height;

	EmConnection *connection;

	InputState input;
};

em_state _state = {};

void
onAppCmd(struct android_app *app, int32_t cmd)
{
	switch (cmd) {
	case APP_CMD_START: ALOGI("APP_CMD_START"); break;
	case APP_CMD_RESUME: ALOGI("APP_CMD_RESUME"); break;
	case APP_CMD_PAUSE: ALOGI("APP_CMD_PAUSE"); break;
	case APP_CMD_STOP:
		ALOGE("APP_CMD_STOP - shutting down connection");
		em_connection_disconnect(_state.connection);
		_state.connected = false;
		break;
	case APP_CMD_DESTROY: ALOGI("APP_CMD_DESTROY"); break;
	case APP_CMD_INIT_WINDOW: ALOGI("APP_CMD_INIT_WINDOW: %p", app->window); break;
	case APP_CMD_TERM_WINDOW:
		ALOGI("APP_CMD_TERM_WINDOW - shutting down connection");
		em_connection_disconnect(_state.connection);
		_state.connected = false;
		break;
	}
}

inline XrResult
CheckXrResult(XrResult res, const char *originator = nullptr, const char *sourceLocation = nullptr)
{
	if (XR_FAILED(res)) {
		ALOGE("XR call failed with: %i, %s, %s", res, originator, sourceLocation);
	}

	return res;
}

void
initialize_actions(struct em_state &state)
{
	// Create an action set.
	{
		XrActionSetCreateInfo actionSetInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
		strcpy(actionSetInfo.actionSetName, "gameplay");
		strcpy(actionSetInfo.localizedActionSetName, "Gameplay");
		actionSetInfo.priority = 0;
		CheckXrResult(xrCreateActionSet(state.instance, &actionSetInfo, &state.input.actionSet));
	}

	// Get the XrPath for the left and right hands - we will use them as subaction paths.
	CheckXrResult(xrStringToPath(state.instance, "/user/hand/left", &state.input.handSubactionPath[Side::LEFT]));
	CheckXrResult(xrStringToPath(state.instance, "/user/hand/right", &state.input.handSubactionPath[Side::RIGHT]));

	// Create actions.
	{
		// Create an input action for grabbing objects with the left and right hands.
		XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
		actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
		strcpy(actionInfo.actionName, "grab_object");
		strcpy(actionInfo.localizedActionName, "Grab Object");
		actionInfo.countSubactionPaths = uint32_t(state.input.handSubactionPath.size());
		actionInfo.subactionPaths = state.input.handSubactionPath.data();
		CheckXrResult(xrCreateAction(state.input.actionSet, &actionInfo, &state.input.grabAction));

		// Create an input action getting the left and right hand poses.
		actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
		strcpy(actionInfo.actionName, "hand_pose");
		strcpy(actionInfo.localizedActionName, "Hand Pose");
		actionInfo.countSubactionPaths = uint32_t(state.input.handSubactionPath.size());
		actionInfo.subactionPaths = state.input.handSubactionPath.data();
		CheckXrResult(xrCreateAction(state.input.actionSet, &actionInfo, &state.input.poseAction));

		// Create output actions for vibrating the left and right controller.
		actionInfo.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
		strcpy(actionInfo.actionName, "vibrate_hand");
		strcpy(actionInfo.localizedActionName, "Vibrate Hand");
		actionInfo.countSubactionPaths = uint32_t(state.input.handSubactionPath.size());
		actionInfo.subactionPaths = state.input.handSubactionPath.data();
		CheckXrResult(xrCreateAction(state.input.actionSet, &actionInfo, &state.input.vibrateAction));

		// Create input actions for quitting the session using the left and right controller.
		// Since it doesn't matter which hand did this, we do not specify subaction paths for it.
		// We will just suggest bindings for both hands, where possible.
		actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
		strcpy(actionInfo.actionName, "quit_session");
		strcpy(actionInfo.localizedActionName, "Quit Session");
		actionInfo.countSubactionPaths = 0;
		actionInfo.subactionPaths = nullptr;
		CheckXrResult(xrCreateAction(state.input.actionSet, &actionInfo, &state.input.quitAction));
	}

	std::array<XrPath, Side::COUNT> selectPath;
	std::array<XrPath, Side::COUNT> squeezeValuePath;
	std::array<XrPath, Side::COUNT> squeezeForcePath;
	std::array<XrPath, Side::COUNT> squeezeClickPath;
	std::array<XrPath, Side::COUNT> posePath;
	std::array<XrPath, Side::COUNT> hapticPath;
	std::array<XrPath, Side::COUNT> menuClickPath;
	std::array<XrPath, Side::COUNT> bClickPath;
	std::array<XrPath, Side::COUNT> triggerValuePath;
	CheckXrResult(xrStringToPath(state.instance, "/user/hand/left/input/select/click", &selectPath[Side::LEFT]));
	CheckXrResult(xrStringToPath(state.instance, "/user/hand/right/input/select/click", &selectPath[Side::RIGHT]));
	CheckXrResult(
	    xrStringToPath(state.instance, "/user/hand/left/input/squeeze/value", &squeezeValuePath[Side::LEFT]));
	CheckXrResult(
	    xrStringToPath(state.instance, "/user/hand/right/input/squeeze/value", &squeezeValuePath[Side::RIGHT]));
	CheckXrResult(
	    xrStringToPath(state.instance, "/user/hand/left/input/squeeze/force", &squeezeForcePath[Side::LEFT]));
	CheckXrResult(
	    xrStringToPath(state.instance, "/user/hand/right/input/squeeze/force", &squeezeForcePath[Side::RIGHT]));
	CheckXrResult(
	    xrStringToPath(state.instance, "/user/hand/left/input/squeeze/click", &squeezeClickPath[Side::LEFT]));
	CheckXrResult(
	    xrStringToPath(state.instance, "/user/hand/right/input/squeeze/click", &squeezeClickPath[Side::RIGHT]));
	CheckXrResult(xrStringToPath(state.instance, "/user/hand/left/input/grip/pose", &posePath[Side::LEFT]));
	CheckXrResult(xrStringToPath(state.instance, "/user/hand/right/input/grip/pose", &posePath[Side::RIGHT]));
	CheckXrResult(xrStringToPath(state.instance, "/user/hand/left/output/haptic", &hapticPath[Side::LEFT]));
	CheckXrResult(xrStringToPath(state.instance, "/user/hand/right/output/haptic", &hapticPath[Side::RIGHT]));
	CheckXrResult(xrStringToPath(state.instance, "/user/hand/left/input/menu/click", &menuClickPath[Side::LEFT]));
	CheckXrResult(xrStringToPath(state.instance, "/user/hand/right/input/menu/click", &menuClickPath[Side::RIGHT]));
	CheckXrResult(xrStringToPath(state.instance, "/user/hand/left/input/b/click", &bClickPath[Side::LEFT]));
	CheckXrResult(xrStringToPath(state.instance, "/user/hand/right/input/b/click", &bClickPath[Side::RIGHT]));
	CheckXrResult(
	    xrStringToPath(state.instance, "/user/hand/left/input/trigger/value", &triggerValuePath[Side::LEFT]));
	CheckXrResult(
	    xrStringToPath(state.instance, "/user/hand/right/input/trigger/value", &triggerValuePath[Side::RIGHT]));

	// Suggest bindings for KHR Simple.
	{
		XrPath khrSimpleInteractionProfilePath;
		CheckXrResult(
		    xrStringToPath(state.instance, "/interaction_profiles/khr/simple_controller", &khrSimpleInteractionProfilePath));
		std::vector<XrActionSuggestedBinding> bindings{{// Fall back to a click input for the grab action.
		                                                {state.input.grabAction, selectPath[Side::LEFT]},
		                                                {state.input.grabAction, selectPath[Side::RIGHT]},
		                                                {state.input.poseAction, posePath[Side::LEFT]},
		                                                {state.input.poseAction, posePath[Side::RIGHT]},
		                                                {state.input.quitAction, menuClickPath[Side::LEFT]},
		                                                {state.input.quitAction, menuClickPath[Side::RIGHT]},
		                                                {state.input.vibrateAction, hapticPath[Side::LEFT]},
		                                                {state.input.vibrateAction, hapticPath[Side::RIGHT]}}};
		XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
		suggestedBindings.interactionProfile = khrSimpleInteractionProfilePath;
		suggestedBindings.suggestedBindings = bindings.data();
		suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
		CheckXrResult(xrSuggestInteractionProfileBindings(state.instance, &suggestedBindings));
	}
	
	XrActionSpaceCreateInfo actionSpaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
	actionSpaceInfo.action = state.input.poseAction;
	actionSpaceInfo.poseInActionSpace.orientation.w = 1.f;
	actionSpaceInfo.subactionPath = state.input.handSubactionPath[Side::LEFT];
	CheckXrResult(xrCreateActionSpace(state.session, &actionSpaceInfo, &state.input.handSpace[Side::LEFT]));
	actionSpaceInfo.subactionPath = state.input.handSubactionPath[Side::RIGHT];
	CheckXrResult(xrCreateActionSpace(state.session, &actionSpaceInfo, &state.input.handSpace[Side::RIGHT]));

	XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &state.input.actionSet;
	CheckXrResult(xrAttachSessionActionSets(state.session, &attachInfo));
}

/**
 * Poll for Android and OpenXR events, and handle them
 *
 * @param state app state
 *
 * @return true if we should go to the render code
 */
bool
poll_events(struct android_app *app, struct em_state &state)
{
	// Poll Android events
	for (;;) {
		int events;
		struct android_poll_source *source;
		bool wait = !app->window || app->activityState != APP_CMD_RESUME;
		int timeout = wait ? -1 : 0;
		if (ALooper_pollAll(timeout, NULL, &events, (void **)&source) >= 0) {
			if (source) {
				source->process(app, source);
			}

			if (timeout == 0 && (!app->window || app->activityState != APP_CMD_RESUME)) {
				break;
			}
		} else {
			break;
		}
	}

	// Poll OpenXR events
	XrResult result = XR_SUCCESS;
	XrEventDataBuffer buffer;
	buffer.type = XR_TYPE_EVENT_DATA_BUFFER;
	buffer.next = NULL;

	while (xrPollEvent(state.instance, &buffer) == XR_SUCCESS) {
		if (buffer.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			XrEventDataSessionStateChanged *event = (XrEventDataSessionStateChanged *)&buffer;

			switch (event->state) {
			case XR_SESSION_STATE_IDLE: ALOGI("OpenXR session is now IDLE"); break;
			case XR_SESSION_STATE_READY: {
				ALOGI("OpenXR session is now READY, beginning session");
				XrSessionBeginInfo beginInfo = {};
				beginInfo.type = XR_TYPE_SESSION_BEGIN_INFO;
				beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

				result = xrBeginSession(state.session, &beginInfo);

				if (XR_FAILED(result)) {
					ALOGI("Failed to begin OpenXR session (%d)", result);
				}
			} break;
			case XR_SESSION_STATE_SYNCHRONIZED: ALOGI("OpenXR session is now SYNCHRONIZED"); break;
			case XR_SESSION_STATE_VISIBLE: ALOGI("OpenXR session is now VISIBLE"); break;
			case XR_SESSION_STATE_FOCUSED: ALOGI("OpenXR session is now FOCUSED"); break;
			case XR_SESSION_STATE_STOPPING:
				ALOGI("OpenXR session is now STOPPING");
				xrEndSession(state.session);
				break;
			case XR_SESSION_STATE_LOSS_PENDING: ALOGI("OpenXR session is now LOSS_PENDING"); break;
			case XR_SESSION_STATE_EXITING: ALOGI("OpenXR session is now EXITING"); break;
			default: break;
			}

			state.sessionState = event->state;
		}

		buffer.type = XR_TYPE_EVENT_DATA_BUFFER;
	}

	// If session isn't ready, return. We'll be called again and will poll events again.
	if (state.sessionState < XR_SESSION_STATE_READY) {
		ALOGI("Waiting for session ready state!");
		using namespace std::chrono_literals;
		std::this_thread::sleep_for(100ms);
		return false;
	}

	state.input.handActive = {XR_FALSE, XR_FALSE};

	// Sync actions
	const XrActiveActionSet activeActionSet{state.input.actionSet, XR_NULL_PATH};
	XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &activeActionSet;
	CheckXrResult(xrSyncActions(state.session, &syncInfo));

	// Get pose and grab action state and start haptic vibrate when hand is 90% squeezed.
	for (auto hand : {Side::LEFT, Side::RIGHT}) {
		XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
		getInfo.action = state.input.grabAction;
		getInfo.subactionPath = state.input.handSubactionPath[hand];

		XrActionStateFloat grabValue{XR_TYPE_ACTION_STATE_FLOAT};
		CheckXrResult(xrGetActionStateFloat(state.session, &getInfo, &grabValue));
		if (grabValue.isActive == XR_TRUE) {
			// Scale the rendered hand by 1.0f (open) to 0.5f (fully squeezed).
			state.input.handScale[hand] = 1.0f - 0.5f * grabValue.currentState;
			if (grabValue.currentState > 0.9f) {
				XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
				vibration.amplitude = 0.5;
				vibration.duration = XR_MIN_HAPTIC_DURATION;
				vibration.frequency = XR_FREQUENCY_UNSPECIFIED;

				XrHapticActionInfo hapticActionInfo{XR_TYPE_HAPTIC_ACTION_INFO};
				hapticActionInfo.action = state.input.vibrateAction;
				hapticActionInfo.subactionPath = state.input.handSubactionPath[hand];
				CheckXrResult(xrApplyHapticFeedback(state.session, &hapticActionInfo,
				                                    (XrHapticBaseHeader *)&vibration));
			}
		}

		getInfo.action = state.input.poseAction;
		XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
		CheckXrResult(xrGetActionStatePose(state.session, &getInfo, &poseState));
		state.input.handActive[hand] = poseState.isActive;
	}

	return true;
}

void
connected_cb(EmConnection *connection, struct em_state *state)
{
	ALOGI("%s: Got signal that we are connected!", __FUNCTION__);

	state->connected = true;
}

} // namespace

void
android_main(struct android_app *app)
{
	// Debugging gstreamer.
	// GST_DEBUG = *:3 will give you ONLY ERROR-level messages.
	// GST_DEBUG = *:6 will give you ALL messages (make sure you BOOST your android-studio's
	// Logcat buffer to be able to capture everything gstreamer's going to spit at you !
	// in Tools -> logcat -> Cycle Buffer Size (I set it to 102400 KB).

	// setenv("GST_DEBUG", "*:3", 1);
	// setenv("GST_DEBUG", "*ssl*:9,*tls*:9,*webrtc*:9", 1);
	// setenv("GST_DEBUG", "GST_CAPS:5", 1);
	setenv("GST_DEBUG", "*:2,webrtc*:9,sctp*:9,dtls*:9,amcvideodec:9", 1);

	// Do not do ansi color codes
	setenv("GST_DEBUG_NO_COLOR", "1", 1);

	JNIEnv *env = nullptr;
	(*app->activity->vm).AttachCurrentThread(&env, NULL);
	app->onAppCmd = onAppCmd;

	auto initialEglData = std::make_unique<EglData>();

	//
	// Normal OpenXR app startup
	//

	// Initialize OpenXR loader
	PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = NULL;
	XR_LOAD(xrInitializeLoaderKHR);
	XrLoaderInitInfoAndroidKHR loaderInfo = {
	    .type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR,
	    .applicationVM = app->activity->vm,
	    .applicationContext = app->activity->clazz,
	};

	XrResult result = xrInitializeLoaderKHR((XrLoaderInitInfoBaseHeaderKHR *)&loaderInfo);

	if (XR_FAILED(result)) {
		ALOGE("Failed to initialize OpenXR loader");
		return;
	}

	// Create OpenXR instance

	const char *extensions[] = {XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
	                            XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
	                            XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME};

	XrInstanceCreateInfoAndroidKHR androidInfo = {};
	androidInfo.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
	androidInfo.applicationActivity = app->activity->clazz;
	androidInfo.applicationVM = app->activity->vm;

	XrInstanceCreateInfo instanceInfo = {};
	instanceInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.next = &androidInfo;

	strncpy(instanceInfo.applicationInfo.engineName, "N/A", XR_MAX_APPLICATION_NAME_SIZE - 1);
	instanceInfo.applicationInfo.engineName[XR_MAX_APPLICATION_NAME_SIZE - 1] = '\0';

	strncpy(instanceInfo.applicationInfo.applicationName, "N/A", XR_MAX_APPLICATION_NAME_SIZE - 1);
	instanceInfo.applicationInfo.applicationName[XR_MAX_APPLICATION_NAME_SIZE - 1] = '\0';

	instanceInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	instanceInfo.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
	instanceInfo.enabledExtensionNames = extensions;

	result = xrCreateInstance(&instanceInfo, &_state.instance);

	if (XR_FAILED(result)) {
		ALOGE("Failed to initialize OpenXR instance");
		return;
	}

	// OpenXR system

	XrSystemGetInfo systemInfo = {.type = XR_TYPE_SYSTEM_GET_INFO,
	                              .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};

	result = xrGetSystem(_state.instance, &systemInfo, &_state.system);

	uint32_t viewConfigurationCount;
	XrViewConfigurationType viewConfigurations[2];
	result = xrEnumerateViewConfigurations(_state.instance, _state.system, 2, &viewConfigurationCount,
	                                       viewConfigurations);

	if (XR_FAILED(result)) {
		ALOGE("Failed to enumerate view configurations");
		return;
	}

	XrViewConfigurationView viewInfo[2] = {};
	viewInfo[0].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	viewInfo[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;

	uint32_t viewCount = 0;
	xrEnumerateViewConfigurationViews(_state.instance, _state.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
	                                  &viewCount, NULL);
	result = xrEnumerateViewConfigurationViews(_state.instance, _state.system,
	                                           XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, viewInfo);

	if (XR_FAILED(result) || viewCount != 2) {
		ALOGE("Failed to enumerate view configuration views");
		return;
	}

	_state.width = viewInfo[0].recommendedImageRectWidth;
	_state.height = viewInfo[0].recommendedImageRectHeight;
	ALOGI("Recommended image rect size: %u, %u", _state.width, _state.height);

	// OpenXR session
	ALOGI("FRED: Creating OpenXR session...");
	PFN_xrGetOpenGLESGraphicsRequirementsKHR xrGetOpenGLESGraphicsRequirementsKHR = NULL;
	XR_LOAD(xrGetOpenGLESGraphicsRequirementsKHR);
	XrGraphicsRequirementsOpenGLESKHR graphicsRequirements = {.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
	xrGetOpenGLESGraphicsRequirementsKHR(_state.instance, _state.system, &graphicsRequirements);

	XrGraphicsBindingOpenGLESAndroidKHR graphicsBinding = {
	    .type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR,
	    .display = initialEglData->display,
	    .config = initialEglData->config,
	    .context = initialEglData->context,
	};

	XrSessionCreateInfo sessionInfo = {
	    .type = XR_TYPE_SESSION_CREATE_INFO, .next = &graphicsBinding, .systemId = _state.system};

	result = xrCreateSession(_state.instance, &sessionInfo, &_state.session);

	if (XR_FAILED(result)) {
		ALOGE("ERROR: Failed to create OpenXR session (%d)\n", result);
		return;
	}

	initialize_actions(_state);

	//
	// End of normal OpenXR app startup
	//

	EmEglMutexIface *egl_mutex = em_egl_mutex_create(initialEglData->display, initialEglData->context);

	//
	// Start of remote-rendering-specific code
	//

	// Set up gstreamer
	gst_init(0, NULL);

	// Set rank for decoder c2qtiavcdecoder
	GstRegistry *plugins_register = gst_registry_get();
	GstPluginFeature *dec = gst_registry_lookup_feature(plugins_register, "amcviddec-c2qtiavcdecoder");
	if (dec == NULL) {
		ALOGW("c2qtiavcdecoder not available!");
	} else {
		gst_plugin_feature_set_rank(dec, GST_RANK_PRIMARY + 1);
		gst_object_unref(dec);
	}

	// Set up gst logger
	{
		// gst_debug_set_default_threshold(GST_LEVEL_WARNING);
		// gst_debug_set_threshold_for_name("webrtcbin", GST_LEVEL_MEMDUMP);
		// gst_debug_set_threshold_for_name("webrtcbindatachannel", GST_LEVEL_TRACE);
	}

	// Set up our own objects
	ALOGI("%s: creating stream client object", __FUNCTION__);
	EmStreamClient *stream_client = em_stream_client_new();

	ALOGI("%s: telling stream client about EGL", __FUNCTION__);
	// retaining ownership
	em_stream_client_set_egl_context(stream_client, egl_mutex, false, initialEglData->surface);

	ALOGI("%s: creating connection object", __FUNCTION__);
	_state.connection = g_object_ref_sink(em_connection_new_localhost());

	g_signal_connect(_state.connection, "connected", G_CALLBACK(connected_cb), &_state);

	ALOGI("%s: starting connection", __FUNCTION__);
	em_connection_connect(_state.connection);

	XrExtent2Di eye_extents{static_cast<int32_t>(_state.width), static_cast<int32_t>(_state.height)};
	EmRemoteExperience *remote_experience =
	    em_remote_experience_new(_state.connection, stream_client, _state.instance, _state.session, &eye_extents);
	if (!remote_experience) {
		ALOGE("%s: Failed during remote experience init.", __FUNCTION__);
		return;
	}

	ALOGI("%s: starting stream client mainloop thread", __FUNCTION__);
	em_stream_client_spawn_thread(stream_client, _state.connection);

	//
	// End of remote-rendering-specific setup, into main loop
	//

	// Main rendering loop.
	ALOGI("DEBUG: Starting main loop");
	while (!app->destroyRequested) {
		if (poll_events(app, _state)) {
			em_remote_experience_poll_and_render_frame(remote_experience, _state.input);
		}
	}

	ALOGI("DEBUG: Exited main loop, cleaning up");
	//
	// Clean up RR structures
	//

	g_clear_object(&_state.connection);
	// without gobject for stream client, the EmRemoteExperience takes ownership
	// g_clear_object(&stream_client);

	em_remote_experience_destroy(&remote_experience);

	em_egl_mutex_destroy(&egl_mutex);

	//
	// End RR cleanup
	//

	initialEglData = nullptr;

	(*app->activity->vm).DetachCurrentThread();
}
