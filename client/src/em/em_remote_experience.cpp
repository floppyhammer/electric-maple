// Copyright 2023, Pluto VR, Inc.
//
// SPDX-License-Identifier: BSL-1.0

/*!
 * @file
 * @brief  Implementation for remote experience object
 * @author Rylie Pavlik <rpavlik@collabora.com>
 * @ingroup em_client
 */

#include "em_remote_experience.h"

// clang-format off
#include "render/xr_platform_deps.h"

#include <linux/time.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <memory>

#include "electricmaple.pb.h"
#include "em_app_log.h"
#include "em_connection.h"
#include "em_stream_client.h"
#include "gst_common.h"
#include "pb_encode.h"
#include "render/GLSwapchain.h"
#include "render/render.hpp"
// clang-format on

struct _EmRemoteExperience {
    EmConnection *connection;
    EmStreamClient *stream_client;
    std::unique_ptr<Renderer> renderer;
    struct em_sample *prev_sample;

    XrExtent2Di eye_extents;

    PFN_xrConvertTimespecTimeToTimeKHR convertTimespecTimeToTime;

    struct {
        XrInstance instance{XR_NULL_HANDLE};
        // XrSystemId system;
        XrSession session{XR_NULL_HANDLE};
    } xr_not_owned;

    struct {
        XrSpace worldSpace;
        XrSpace viewSpace;
        XrSwapchain swapchain;
    } xr_owned;

    GLSwapchain swapchainBuffers;

    std::atomic_int64_t nextUpMessage{1};
};

static constexpr size_t em_proto_UpMessage_size =
    sizeof(_em_proto_HandJointLocation) * 26 * 2 + sizeof(_em_proto_UpMessage);
static constexpr size_t kUpBufferSize = em_proto_UpMessage_size + 10;

bool em_remote_experience_emit_upmessage(EmRemoteExperience *exp, em_proto_UpMessage *upMessage) {
    int64_t message_id = exp->nextUpMessage++;
    upMessage->up_message_id = message_id;

    // Serialize data
    uint8_t buffer[kUpBufferSize];
    pb_ostream_t os = pb_ostream_from_buffer(buffer, sizeof(buffer));

    pb_encode(&os, &em_proto_UpMessage_msg, upMessage);

    // Send data
    GBytes *bytes = g_bytes_new(buffer, os.bytes_written);
    bool bResult = em_connection_send_bytes(exp->connection, bytes);
    g_bytes_unref(bytes);

    return bResult;
}

bool ProtoMessage_encode_hand_joint_locations(pb_ostream_t *ostream, const pb_field_t *field, void *const *arg) {
    auto *source = (em_proto_HandJointLocation *)(*arg);

    // Encode all elements
    for (int i = 0; i < 26; i++) {
        if (!pb_encode_tag_for_field(ostream, field)) {
            const char *error = PB_GET_ERROR(ostream);
            ALOGE("ProtoMessage_encode_hand_joint_locations error: %s", error);
            return false;
        }

        if (!pb_encode_submessage(ostream, em_proto_HandJointLocation_fields, source + i)) {
            const char *error = PB_GET_ERROR(ostream);
            ALOGE("ProtoMessage_encode_hand_joint_locations error: %s", error);
            return false;
        }

        //		ALOGE("Up %d %f %f %f", i, (source + i)->pose.position.x, (source + i)->pose.position.y,
        //		      (source + i)->pose.position.z);
    }

    return true;
}

_em_proto_Pose convert_pose(XrPosef pose) {
    _em_proto_Pose new_pose{};

    new_pose.has_position = true;
    new_pose.position = {pose.position.x, pose.position.y, pose.position.z};

    new_pose.has_orientation = true;
    new_pose.orientation.w = pose.orientation.w;
    new_pose.orientation.x = pose.orientation.x;
    new_pose.orientation.y = pose.orientation.y;
    new_pose.orientation.z = pose.orientation.z;

    return new_pose;
}

/// Send pose data back to server.
static void em_remote_experience_report_pose(EmRemoteExperience *exp,
                                             XrTime predictedDisplayTime,
                                             InputState &inputState) {
    XrResult result = XR_SUCCESS;

    em_proto_TrackingMessage tracking = em_proto_TrackingMessage_init_default;

    {
        // Get HMD location.
        XrSpaceLocation hmdLocalLocation = {};
        hmdLocalLocation.type = XR_TYPE_SPACE_LOCATION;
        hmdLocalLocation.next = nullptr;
        result =
            xrLocateSpace(exp->xr_owned.viewSpace, exp->xr_owned.worldSpace, predictedDisplayTime, &hmdLocalLocation);
        if (result != XR_SUCCESS) {
            ALOGE("Failed to locate HMD location!");
            return;
        }

        XrPosef hmdLocalPose = hmdLocalLocation.pose;

        tracking.has_P_localSpace_viewSpace = true;
        tracking.P_localSpace_viewSpace = convert_pose(hmdLocalPose);

        //        ALOGD("HMD orientation: %f, %f, %f, %f",
        //              hmdLocalPose.orientation.w,
        //              hmdLocalPose.orientation.x,
        //              hmdLocalPose.orientation.y,
        //              hmdLocalPose.orientation.z);
    }

    // Get left hand location.
    {
        XrSpaceLocation handLocalLocation = {};
        handLocalLocation.type = XR_TYPE_SPACE_LOCATION;
        handLocalLocation.next = NULL;
        result = xrLocateSpace(inputState.handSpace[Side::LEFT],
                               exp->xr_owned.worldSpace,
                               predictedDisplayTime,
                               &handLocalLocation);
        if (result != XR_SUCCESS) {
            ALOGE("Failed to locate left hand space!");
            return;
        }

        XrPosef handLocalPose = handLocalLocation.pose;

        tracking.has_controller_grip_left = inputState.handActive[Side::LEFT];
        tracking.controller_grip_left = convert_pose(handLocalPose);

        tracking.has_controller_aim_left = inputState.handActive[Side::LEFT];
        tracking.controller_aim_left = convert_pose(handLocalPose);

        tracking.controller_grip_value_left = inputState.handGrab[Side::LEFT];
    }

    // Get right hand location.
    {
        XrSpaceLocation handLocalLocation = {};
        handLocalLocation.type = XR_TYPE_SPACE_LOCATION;
        handLocalLocation.next = NULL;
        result = xrLocateSpace(inputState.handSpace[Side::RIGHT],
                               exp->xr_owned.worldSpace,
                               predictedDisplayTime,
                               &handLocalLocation);
        if (result != XR_SUCCESS) {
            ALOGE("Failed to locate right hand space!");
            return;
        }

        XrPosef handLocalPose = handLocalLocation.pose;

        tracking.has_controller_grip_right = inputState.handActive[Side::RIGHT];
        tracking.controller_grip_right = convert_pose(handLocalPose);

        tracking.has_controller_aim_right = inputState.handActive[Side::RIGHT];
        tracking.controller_aim_right = convert_pose(handLocalPose);

        tracking.controller_grip_value_right = inputState.handGrab[Side::RIGHT];
    }

    std::array<em_proto_HandJointLocation, XR_HAND_JOINT_COUNT_EXT> hand_joint_locations_left{};
    std::array<em_proto_HandJointLocation, XR_HAND_JOINT_COUNT_EXT> hand_joint_locations_right{};

    // Get hand joint locations
    if (inputState.pfnXrLocateHandJointsEXT != nullptr && inputState.xrHandTrackerEXTLeft != nullptr &&
        inputState.xrHandTrackerEXTRight != nullptr) {
        for (auto hand : {inputState.xrHandTrackerEXTLeft, inputState.xrHandTrackerEXTRight}) {
            XrHandJointLocationEXT jointLocations[XR_HAND_JOINT_COUNT_EXT];
            XrHandJointLocationsEXT locationsEXT = {.type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT,
                                                    .jointCount = XR_HAND_JOINT_COUNT_EXT,
                                                    .jointLocations = jointLocations};

            XrHandJointsLocateInfoEXT locateInfoEXT = {.type = XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT,
                                                       .next = nullptr,
                                                       .baseSpace = exp->xr_owned.worldSpace,
                                                       .time = predictedDisplayTime};

            if (inputState.pfnXrLocateHandJointsEXT(hand, &locateInfoEXT, &locationsEXT) != XR_SUCCESS) {
                ALOGV("Failed to get hand joint locations");
                continue;
            }

            if (!locationsEXT.isActive) {
                continue;
            }

            auto &hand_joint_locations =
                hand == inputState.xrHandTrackerEXTLeft ? hand_joint_locations_left : hand_joint_locations_right;

            for (int i = 0; i < locationsEXT.jointCount; i++) {
                hand_joint_locations[i].index = i;
                hand_joint_locations[i].has_pose = locationsEXT.isActive;

                const auto &joint_pose = locationsEXT.jointLocations[i].pose;

                hand_joint_locations[i].pose = convert_pose(joint_pose);

                hand_joint_locations[i].radius = locationsEXT.jointLocations[i].radius;
            }

            auto &locations = hand == inputState.xrHandTrackerEXTLeft ? tracking.hand_joint_locations_left
                                                                      : tracking.hand_joint_locations_right;
            locations.arg = hand_joint_locations.data();
            locations.funcs.encode = ProtoMessage_encode_hand_joint_locations;
        }
    }

    em_proto_UpMessage upMessage = em_proto_UpMessage_init_default;
    upMessage.has_tracking = true;
    upMessage.tracking = tracking;

    // Send message.
    if (!em_remote_experience_emit_upmessage(exp, &upMessage)) {
        ALOGE("RYLIE: Could not queue HMD pose message!");
    }
}

static void em_remote_experience_dispose(EmRemoteExperience *exp) {
    if (exp->stream_client) {
        em_stream_client_stop(exp->stream_client);
        if (exp->renderer) {
            em_stream_client_egl_begin_pbuffer(exp->stream_client);
            exp->renderer->reset();
            exp->renderer = nullptr;
            em_stream_client_egl_end(exp->stream_client);
        }
        if (exp->prev_sample) {
            em_stream_client_release_sample(exp->stream_client, exp->prev_sample);
            exp->prev_sample = nullptr;
        }
    }

    if (exp->connection) {
        em_connection_disconnect(exp->connection);
    }

    // stream client is not gobject (yet?)
    em_stream_client_destroy(&exp->stream_client);
    g_clear_object(&exp->connection);
    exp->swapchainBuffers.reset();

    if (exp->renderer) {
        ALOGW(
            "%s: Renderer outlived stream client somehow (should not happen), "
            "will take a chance at destroying it anyway",
            __FUNCTION__);
        exp->renderer->reset();
        exp->renderer = nullptr;
    }
}

static void em_remote_experience_finalize(EmRemoteExperience *exp) {
    if (exp->xr_owned.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(exp->xr_owned.swapchain);
        exp->xr_owned.swapchain = XR_NULL_HANDLE;
    }

    if (exp->xr_owned.viewSpace != XR_NULL_HANDLE) {
        xrDestroySpace(exp->xr_owned.viewSpace);
        exp->xr_owned.viewSpace = XR_NULL_HANDLE;
    }

    if (exp->xr_owned.worldSpace != XR_NULL_HANDLE) {
        xrDestroySpace(exp->xr_owned.worldSpace);
        exp->xr_owned.worldSpace = XR_NULL_HANDLE;
    }
}

EmRemoteExperience *em_remote_experience_new(EmConnection *connection,
                                             EmStreamClient *stream_client,
                                             XrInstance instance,
                                             XrSession session,
                                             const XrExtent2Di *eye_extents) {
    EmRemoteExperience *self = reinterpret_cast<EmRemoteExperience *>(calloc(1, sizeof(EmRemoteExperience)));
    self->connection = g_object_ref_sink(connection);
    // self->stream_client = g_object_ref_sink(stream_client);
    self->stream_client = stream_client;
    self->eye_extents = *eye_extents;
    self->xr_not_owned.instance = instance;
    self->xr_not_owned.session = session;

    // Get the extension function for converting times.
    {
        XrResult result =
            xrGetInstanceProcAddr(instance,
                                  "xrConvertTimespecTimeToTimeKHR",
                                  reinterpret_cast<PFN_xrVoidFunction *>(&self->convertTimespecTimeToTime));

        if (XR_FAILED(result)) {
            ALOGE("%s: Failed to get extension function xrConvertTimespecTimeToTimeKHR (%d)\n", __FUNCTION__, result);
            em_remote_experience_destroy(&self);
            return nullptr;
        }
    }

    // Quest requires the EGL context to be current when calling xrCreateSwapchain
    em_stream_client_egl_begin_pbuffer(stream_client);

    {
        //		uint32_t formatCount;
        //		xrEnumerateSwapchainFormats(session, 0, &formatCount, NULL);
        //
        //		std::vector<int64_t> formats(formatCount);
        //		xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data());

        ALOGI("%s: Creating OpenXR Swapchain...", __FUNCTION__);
        // OpenXR swapchain
        XrSwapchainCreateInfo swapchainInfo = {};
        swapchainInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        swapchainInfo.format = GL_SRGB8_ALPHA8;
        swapchainInfo.width = self->eye_extents.width * 2;
        swapchainInfo.height = self->eye_extents.height;
        swapchainInfo.sampleCount = 1;
        swapchainInfo.faceCount = 1;
        swapchainInfo.arraySize = 1;
        swapchainInfo.mipCount = 1;

        XrResult result = xrCreateSwapchain(session, &swapchainInfo, &self->xr_owned.swapchain);

        if (XR_FAILED(result)) {
            ALOGE("%s: Failed to create OpenXR swapchain (%d)\n", __FUNCTION__, result);
            em_stream_client_egl_end(stream_client);
            em_remote_experience_destroy(&self);
            return nullptr;
        }
    }

    if (!self->swapchainBuffers.enumerateAndGenerateFramebuffers(self->xr_owned.swapchain)) {
        ALOGE("%s: Failed to enumerate swapchain images or associate them with framebuffer object names.",
              __FUNCTION__);
        em_stream_client_egl_end(stream_client);
        em_remote_experience_destroy(&self);
        return nullptr;
    }

    try {
        ALOGI("%s: Setup renderer...", __FUNCTION__);
        self->renderer = std::make_unique<Renderer>();
        self->renderer->setupRender();
    } catch (std::exception const &e) {
        ALOGE("%s: Caught exception setting up renderer: %s", __FUNCTION__, e.what());
        self->renderer->reset();
        em_stream_client_egl_end(stream_client);
        em_remote_experience_destroy(&self);
        return nullptr;
    }

    em_stream_client_egl_end(stream_client);

    {
        ALOGI("%s: Creating OpenXR Spaces...", __FUNCTION__);

        XrResult result = XR_SUCCESS;

        XrReferenceSpaceCreateInfo spaceInfo = {.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
                                                .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE,
                                                .poseInReferenceSpace = {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}}};

        result = xrCreateReferenceSpace(session, &spaceInfo, &self->xr_owned.worldSpace);

        if (XR_FAILED(result)) {
            ALOGE("%s: Failed to create world reference space (%d)", __FUNCTION__, result);
            em_remote_experience_destroy(&self);
            return nullptr;
        }
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;

        result = xrCreateReferenceSpace(session, &spaceInfo, &self->xr_owned.viewSpace);

        if (XR_FAILED(result)) {
            ALOGE("%s: Failed to create view reference space (%d)", __FUNCTION__, result);
            em_remote_experience_destroy(&self);
            return nullptr;
        }
    }

    ALOGI("%s: done", __FUNCTION__);
    return self;
}

void em_remote_experience_destroy(EmRemoteExperience **ptr_exp) {
    if (ptr_exp == NULL) {
        return;
    }
    EmRemoteExperience *exp = *ptr_exp;
    if (exp == NULL) {
        return;
    }
    em_remote_experience_dispose(exp);
    em_remote_experience_finalize(exp);
    free(exp);
    *ptr_exp = NULL;
}

EmPollRenderResult em_remote_experience_poll_and_render_frame(EmRemoteExperience *exp, InputState &inputState) {
    XrFrameState frameState = {.type = XR_TYPE_FRAME_STATE};
    XrSession session = exp->xr_not_owned.session;
    XrResult result = xrWaitFrame(session, NULL, &frameState);

    if (XR_FAILED(result)) {
        ALOGE("xrWaitFrame failed");
        return EM_POLL_RENDER_RESULT_ERROR_WAITFRAME;
    }

    XrFrameBeginInfo beginfo = {.type = XR_TYPE_FRAME_BEGIN_INFO};

    result = xrBeginFrame(session, &beginfo);

    if (XR_FAILED(result)) {
        ALOGE("xrBeginFrame failed");
        std::abort();
    }
    struct timespec beginTime;
    if (0 != clock_gettime(CLOCK_MONOTONIC, &beginTime)) {
        ALOGE("%s: clock_gettime failed, which is very unexpected", __FUNCTION__);
        // TODO how to handle this?
        return EM_POLL_RENDER_RESULT_SHOULD_NOT_RENDER;
    }

    XrViewLocateInfo locateInfo = {.type = XR_TYPE_VIEW_LOCATE_INFO,
                                   .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                   .displayTime = frameState.predictedDisplayTime,
                                   .space = exp->xr_owned.worldSpace};

    XrViewState viewState = {.type = XR_TYPE_VIEW_STATE};

    // Locate views, set up layers
    XrView views[2] = {};
    views[0].type = XR_TYPE_VIEW;
    views[1].type = XR_TYPE_VIEW;

    uint32_t viewCount = 0;
    result = xrLocateViews(session, &locateInfo, &viewState, sizeof(views) / sizeof(views[0]), &viewCount, views);

    if (XR_FAILED(result)) {
        ALOGE("Failed to locate views");
        // TODO how to handle this?
        return EM_POLL_RENDER_RESULT_SHOULD_NOT_RENDER;
    }

    XrCompositionLayerProjection layer = {};
    layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    layer.viewCount = 2;

    // TODO use multiview/array swapchain instead of two draw calls for side by side?
    XrCompositionLayerProjectionView projectionViews[2] = {};
    projectionViews[0].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
    projectionViews[1].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;

    layer.views = projectionViews;

    // Render

    if (!em_stream_client_egl_begin_pbuffer(exp->stream_client)) {
        ALOGE("FRED: mainloop_one: Failed make egl context current");
        return EM_POLL_RENDER_RESULT_ERROR_EGL;
    }

    bool shouldRender = frameState.shouldRender == XR_TRUE;
    EmPollRenderResult prResult = EM_POLL_RENDER_RESULT_SHOULD_NOT_RENDER;
    if (shouldRender) {
        prResult = em_remote_experience_inner_poll_and_render_frame(exp,
                                                                    &beginTime,
                                                                    frameState.predictedDisplayTime,
                                                                    views,
                                                                    &layer,
                                                                    projectionViews);
    }

    // Submit frame
    XrFrameEndInfo endInfo = {};
    endInfo.type = XR_TYPE_FRAME_END_INFO;
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = em_poll_render_result_include_layer(prResult) ? 1 : 0;
    endInfo.layers = (const XrCompositionLayerBaseHeader *[1]){(XrCompositionLayerBaseHeader *)&layer};

    result = xrEndFrame(session, &endInfo);
    if (XR_FAILED(result)) {
        ALOGE("Failed to end frame");
        return EM_POLL_RENDER_RESULT_ERROR_ENDFRAME;
    }

    em_stream_client_egl_end(exp->stream_client);

    if (em_connection_get_status(exp->connection) == EM_STATUS_CONNECTED) {
        em_remote_experience_report_pose(exp, frameState.predictedDisplayTime, inputState);
    }

    return prResult;
}

static void report_frame_timing(EmRemoteExperience *exp,
                                const struct timespec *beginFrameTime,
                                const struct timespec *decodeEndTime,
                                XrTime predictedDisplayTime) {
    XrTime xrTimeDecodeEnd = 0;
    XrTime xrTimeBeginFrame = 0;
    XrResult result = exp->convertTimespecTimeToTime(exp->xr_not_owned.instance, decodeEndTime, &xrTimeDecodeEnd);
    if (XR_FAILED(result)) {
        ALOGE("%s: Failed to convert decode-end time (%d)", __FUNCTION__, result);
        return;
    }
    result = exp->convertTimespecTimeToTime(exp->xr_not_owned.instance, beginFrameTime, &xrTimeBeginFrame);
    if (XR_FAILED(result)) {
        ALOGE("%s: Failed to convert begin-frame time (%d)", __FUNCTION__, result);
        return;
    }
    em_proto_UpFrameMessage msg = em_proto_UpFrameMessage_init_default;
    // TODO frame ID
    // msg.frame_sequence_id = ???;
    msg.decode_complete_time = xrTimeDecodeEnd;
    msg.begin_frame_time = xrTimeBeginFrame;
    msg.display_time = predictedDisplayTime;
    em_proto_UpMessage upMsg = em_proto_UpMessage_init_default;
    upMsg.frame = msg;
    upMsg.has_frame = true;
    em_remote_experience_emit_upmessage(exp, &upMsg);
}

EmPollRenderResult em_remote_experience_inner_poll_and_render_frame(EmRemoteExperience *exp,
                                                                    const struct timespec *beginFrameTime,
                                                                    XrTime predictedDisplayTime,
                                                                    XrView *views,
                                                                    XrCompositionLayerProjection *projectionLayer,
                                                                    XrCompositionLayerProjectionView *projectionViews) {
    XrResult result;

    // TODO these may not be the extents of the frame we receive, thus introducing repeated scaling!
    uint32_t width = exp->eye_extents.width;
    uint32_t height = exp->eye_extents.height;

    static bool showedFov = false;
    if (!showedFov) {
        showedFov = true;
        ALOGI(
            "RYLIE XrFovf 0: (xrt_fov){ .angle_left = %0.03ff, .angle_right = %0.03ff, .angle_up = %0.03ff, "
            ".angle_down = %0.03ff }",
            views[0].fov.angleLeft,
            views[0].fov.angleRight,
            views[0].fov.angleUp,
            views[0].fov.angleDown);
        ALOGI(
            "RYLIE XrFovf 1: (xrt_fov){ .angle_left = %0.03ff, .angle_right = %0.03ff, .angle_up = %0.03ff, "
            ".angle_down = %0.03ff }",
            views[1].fov.angleLeft,
            views[1].fov.angleRight,
            views[1].fov.angleUp,
            views[1].fov.angleDown);
    }

    projectionLayer->space = exp->xr_owned.worldSpace;

    projectionViews[0].subImage.swapchain = exp->xr_owned.swapchain;
    projectionViews[0].pose = views[0].pose; // TODO use poses from server
    projectionViews[0].fov = views[0].fov;
    projectionViews[0].subImage.imageRect.offset = {0, 0};
    projectionViews[0].subImage.imageRect.extent = {static_cast<int32_t>(width), static_cast<int32_t>(height)};
    projectionViews[1].subImage.swapchain = exp->xr_owned.swapchain;
    projectionViews[1].pose = views[1].pose; // TODO use poses from server
    projectionViews[1].fov = views[1].fov;
    projectionViews[1].subImage.imageRect.offset = {static_cast<int32_t>(width), 0};
    projectionViews[1].subImage.imageRect.extent = {static_cast<int32_t>(width), static_cast<int32_t>(height)};

    struct timespec decodeEndTime;
    struct em_sample *sample = em_stream_client_try_pull_sample(exp->stream_client, &decodeEndTime);

    if (sample == nullptr) {
        if (exp->prev_sample) {
            return EM_POLL_RENDER_RESULT_REUSED_SAMPLE;
        }
        return EM_POLL_RENDER_RESULT_NO_SAMPLE_AVAILABLE;
    }

    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t imageIndex;
    result = xrAcquireSwapchainImage(exp->xr_owned.swapchain, &acquireInfo, &imageIndex);

    if (XR_FAILED(result)) {
        ALOGE("Failed to acquire swapchain image (%d)", result);
        std::abort();
    }

    XrSwapchainImageWaitInfo waitInfo = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION};

    result = xrWaitSwapchainImage(exp->xr_owned.swapchain, &waitInfo);

    if (XR_FAILED(result)) {
        ALOGE("Failed to wait for swapchain image (%d)", result);
        std::abort();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, exp->swapchainBuffers.framebufferNameAtSwapchainIndex(imageIndex));

    glViewport(0, 0, width * 2, height);
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);

    // Disable gamma correction, as the frame texture is already in sRGB space.
    glDisable(GL_FRAMEBUFFER_SRGB_EXT); // This has effects only when we are drawing to a sRGB framebuffer

    exp->renderer->draw(sample->frame_texture_id, sample->frame_texture_target);

    // Release

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    xrReleaseSwapchainImage(exp->xr_owned.swapchain, NULL);

    // TODO check here to see if we already overshot the predicted display time, maybe?

    if (exp->prev_sample != NULL) {
        em_stream_client_release_sample(exp->stream_client, exp->prev_sample);
    }
    exp->prev_sample = sample;

    // Send frame report
    report_frame_timing(exp, beginFrameTime, &decodeEndTime, predictedDisplayTime);

    return EM_POLL_RENDER_RESULT_NEW_SAMPLE;
}
