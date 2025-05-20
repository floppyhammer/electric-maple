// Copyright 2020-2023, Collabora, Ltd.
// Copyright 2023, Pluto VR, Inc.
//
// SPDX-License-Identifier: BSL-1.0

/*!
 * @file
 * @brief Electric Maple Server motion controller device
 *
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Moshi Turner <moses@collabora.com>
 * @ingroup drv_ems
 */

#include "xrt/xrt_device.h"

#include "os/os_time.h"

#include "math/m_api.h"
#include "math/m_mathinclude.h"

#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_logging.h"
#include "util/u_distortion_mesh.h"

#include "electricmaple.pb.h"
#include "ems_callbacks.h"
#include "pb_decode.h"

#include "ems_server_internal.h"
#include "util/u_hand_simulation.h"

#include <thread>

#include <cstdio>
#include <cassert>


/*
 *
 * Structs and defines.
 *
 */


/// Casting helper function
static inline struct ems_motion_controller *
ems_motion_controller(struct xrt_device *xdev)
{
	return (struct ems_motion_controller *)xdev;
}

DEBUG_GET_ONCE_LOG_OPTION(sample_log, "EMS_LOG", U_LOGGING_WARN)

#define EMS_TRACE(p, ...) U_LOG_XDEV_IFL_T(&p->base, p->log_level, __VA_ARGS__)
#define EMS_DEBUG(p, ...) U_LOG_XDEV_IFL_D(&p->base, p->log_level, __VA_ARGS__)
#define EMS_ERROR(p, ...) U_LOG_XDEV_IFL_E(&p->base, p->log_level, __VA_ARGS__)

static void
controller_destroy(struct xrt_device *xdev)
{
	struct ems_motion_controller *emc = ems_motion_controller(xdev);

	// Remove the variable tracking.
	u_var_remove_root(emc);

	u_device_free(&emc->base);

	delete[] emc->hand_joints;
	emc->hand_joints = NULL;
}

// You should put code to update the attached input fields (if any)
static xrt_result_t
controller_update_inputs(struct xrt_device *xdev)
{
	struct ems_motion_controller *emc = ems_motion_controller(xdev);

	uint64_t now = os_monotonic_get_ns();

	if (!emc->active) {
		for (uint32_t i = 0; i < xdev->input_count; i++) {
			xdev->inputs[i].active = false;
			xdev->inputs[i].timestamp = now;
			U_ZERO(&xdev->inputs[i].value);
		}
		return XRT_SUCCESS;
	}

	for (uint32_t i = 0; i < xdev->input_count; i++) {
		xdev->inputs[i].active = true;
		xdev->inputs[i].timestamp = now;
	}

	return XRT_SUCCESS;
}

static void
controller_set_output(struct xrt_device *xdev, enum xrt_output_name name, const union xrt_output_value *value)
{
	// Since we don't have a data channel yet, this is a no-op.
}

static void
controller_get_hand_tracking(struct xrt_device *xdev,
                             enum xrt_input_name name,
                             int64_t requested_timestamp_ns,
                             struct xrt_hand_joint_set *out_value,
                             int64_t *out_timestamp_ns)
{
	struct ems_motion_controller *emc = ems_motion_controller(xdev);

	if (name != XRT_INPUT_GENERIC_HAND_TRACKING_LEFT && name != XRT_INPUT_GENERIC_HAND_TRACKING_RIGHT) {
		U_LOG_E("Unknown input name for hand tracker: 0x%0x", name);
		return;
	}

	out_value->hand_pose.pose = emc->pose;

	out_value->is_active = emc->active;

	if (emc->active) {
		for (int i = 0; i < 26; i++) {
			auto &joint_pose = out_value->values.hand_joint_set_default[i].relation.pose;
			joint_pose.position.x = emc->hand_joints[i].pose.position.x;
			joint_pose.position.y = emc->hand_joints[i].pose.position.y;
			joint_pose.position.z = emc->hand_joints[i].pose.position.z;

			joint_pose.orientation.x = emc->hand_joints[i].pose.orientation.x;
			joint_pose.orientation.y = emc->hand_joints[i].pose.orientation.y;
			joint_pose.orientation.z = emc->hand_joints[i].pose.orientation.z;
			joint_pose.orientation.w = emc->hand_joints[i].pose.orientation.w;

			out_value->values.hand_joint_set_default[i].radius = emc->hand_joints[i].radius;

			out_value->values.hand_joint_set_default[i].relation.relation_flags =
			    (enum xrt_space_relation_flags)(                 //
			        XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |   //
			        XRT_SPACE_RELATION_POSITION_VALID_BIT |      //
			        XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | //
			        XRT_SPACE_RELATION_POSITION_TRACKED_BIT);    //
		}

		U_LOG_E("handLocalPose %f %f %f", out_value->values.hand_joint_set_default[0].relation.pose.position.x,
		        out_value->values.hand_joint_set_default[0].relation.pose.position.y,
		        out_value->values.hand_joint_set_default[0].relation.pose.position.z);
	} else {
		for (int i = 0; i < 26; i++) {
			auto &joint_pose = out_value->values.hand_joint_set_default[i].relation.pose;
			joint_pose.position.x = 0;
			joint_pose.position.y = 0;
			joint_pose.position.z = 0;

			joint_pose.orientation.x = 0;
			joint_pose.orientation.y = 0;
			joint_pose.orientation.z = 0;
			joint_pose.orientation.w = 0;

			out_value->values.hand_joint_set_default[i].radius = 0;

			out_value->values.hand_joint_set_default[i].relation.relation_flags =
			    XRT_SPACE_RELATION_BITMASK_NONE;
		}
	}

	// This is a lie
	*out_timestamp_ns = requested_timestamp_ns;
}

static xrt_result_t
controller_get_tracked_pose(struct xrt_device *xdev,
                            enum xrt_input_name name,
                            int64_t at_timestamp_ns,
                            struct xrt_space_relation *out_relation)
{
	struct ems_motion_controller *emc = ems_motion_controller(xdev);

	switch (name) {
	case XRT_INPUT_INDEX_GRIP_POSE:
	case XRT_INPUT_INDEX_AIM_POSE: break;
	default: {
		EMS_ERROR(emc, "unknown input name: %d", name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	}

	// Estimate pose at timestamp at_timestamp_ns!
	math_quat_normalize(&emc->pose.orientation);
	out_relation->pose = emc->pose;
	out_relation->relation_flags = (enum xrt_space_relation_flags)( //
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |                  //
	    XRT_SPACE_RELATION_POSITION_VALID_BIT |                     //
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |                //
	    XRT_SPACE_RELATION_POSITION_TRACKED_BIT);                   //

	return XRT_SUCCESS;
}

static void
controller_get_view_poses(struct xrt_device *xdev,
                          const struct xrt_vec3 *default_eye_relation,
                          int64_t at_timestamp_ns,
                          uint32_t view_count,
                          struct xrt_space_relation *out_head_relation,
                          struct xrt_fov *out_fovs,
                          struct xrt_pose *out_poses)
{
	assert(false);
}

/// Fetch remote input data.
static void
controller_handle_data(enum ems_callbacks_event event, const UpMessageSuper *messageSuper, void *userdata)
{
	auto *emc = (struct ems_motion_controller *)userdata;
	emc->active = false;

	auto *message = &messageSuper->protoMessage;

	if (!message->has_tracking) {
		return;
	}

	xrt_pose pose = {};

	if (emc->base.device_type == XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER) {
		if (!message->tracking.has_controller_grip_left) {
			return;
		}

		pose.position = {message->tracking.controller_grip_left.position.x,
		                 message->tracking.controller_grip_left.position.y,
		                 message->tracking.controller_grip_left.position.z};

		pose.orientation.w = message->tracking.controller_grip_left.orientation.w;
		pose.orientation.x = message->tracking.controller_grip_left.orientation.x;
		pose.orientation.y = message->tracking.controller_grip_left.orientation.y;
		pose.orientation.z = message->tracking.controller_grip_left.orientation.z;

		memcpy(emc->hand_joints, messageSuper->hand_joint_locations_left,
		       sizeof(em_proto_HandJointLocation) * 26);
	} else if (emc->base.device_type == XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER) {
		if (!message->tracking.has_controller_grip_right) {
			return;
		}

		pose.position = {message->tracking.controller_grip_right.position.x,
		                 message->tracking.controller_grip_right.position.y,
		                 message->tracking.controller_grip_right.position.z};

		pose.orientation.w = message->tracking.controller_grip_right.orientation.w;
		pose.orientation.x = message->tracking.controller_grip_right.orientation.x;
		pose.orientation.y = message->tracking.controller_grip_right.orientation.y;
		pose.orientation.z = message->tracking.controller_grip_right.orientation.z;

		memcpy(emc->hand_joints, messageSuper->hand_joint_locations_right,
		       sizeof(em_proto_HandJointLocation) * 26);
	} else {
		return;
	}

	// U_LOG_E("handLocalPose %f %f %f", pose.position.x, pose.position.y, pose.position.z);

	// TODO handle timestamp, etc

	emc->active = true;
	emc->pose = pose;
}


/*
 *
 * Bindings
 *
 */

static struct xrt_binding_input_pair simple_inputs_index[4] = {
    {XRT_INPUT_SIMPLE_SELECT_CLICK, XRT_INPUT_INDEX_TRIGGER_VALUE},
    {XRT_INPUT_SIMPLE_MENU_CLICK, XRT_INPUT_INDEX_B_CLICK},
    {XRT_INPUT_SIMPLE_GRIP_POSE, XRT_INPUT_INDEX_GRIP_POSE},
    {XRT_INPUT_SIMPLE_AIM_POSE, XRT_INPUT_INDEX_AIM_POSE},
};

static struct xrt_binding_output_pair simple_outputs_index[1] = {
    {XRT_OUTPUT_NAME_SIMPLE_VIBRATION, XRT_OUTPUT_NAME_INDEX_HAPTIC},
};

static struct xrt_binding_profile binding_profiles_index[1] = {
    {
        .name = XRT_DEVICE_SIMPLE_CONTROLLER,
        .inputs = simple_inputs_index,
        .input_count = ARRAY_SIZE(simple_inputs_index),
        .outputs = simple_outputs_index,
        .output_count = ARRAY_SIZE(simple_outputs_index),
    },
};


/*
 *
 * 'Exported' functions.
 *
 */

struct ems_motion_controller *
ems_motion_controller_create(ems_instance &emsi, enum xrt_device_name device_name, enum xrt_device_type device_type)
{
	uint32_t input_count;
	uint32_t output_count;

	switch (device_name) {
	case XRT_DEVICE_SIMPLE_CONTROLLER:
		input_count = ARRAY_SIZE(simple_inputs_index) + 1; // ARRAY_SIZE + hand tracker
		output_count = ARRAY_SIZE(simple_outputs_index);
		break;
	default: U_LOG_E("Device name not supported!"); return nullptr;
	}

	const char *hand_str = nullptr;
	xrt_pose default_pose;
	switch (device_type) {
	case XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER:
		hand_str = "Right";
		default_pose = (xrt_pose){XRT_QUAT_IDENTITY, {0.2f, 1.4f, -0.4f}};
		break;
	case XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER:
		hand_str = "Left";
		default_pose = (xrt_pose){XRT_QUAT_IDENTITY, {-0.2f, 1.4f, -0.4f}};
		break;
	default: U_LOG_E("Device type not supported!"); return nullptr;
	}

	// We don't need anything special from allocate except inputs and outputs.
	u_device_alloc_flags flags{};
	struct ems_motion_controller *emc =
	    U_DEVICE_ALLOCATE(struct ems_motion_controller, flags, input_count, output_count);

	// Functions.
	emc->base.update_inputs = controller_update_inputs;
	emc->base.set_output = controller_set_output;
	emc->base.get_hand_tracking = controller_get_hand_tracking;
	emc->base.hand_tracking_supported = true;
	emc->base.get_tracked_pose = controller_get_tracked_pose;
	emc->base.get_view_poses = controller_get_view_poses;
	emc->base.destroy = controller_destroy;

	// Data.
	emc->base.tracking_origin = &emsi.tracking_origin;
	// emc->base.binding_profiles = binding_profiles_index;
	// emc->base.binding_profile_count = ARRAY_SIZE(binding_profiles_index);
	emc->base.orientation_tracking_supported = true;
	emc->base.position_tracking_supported = true;
	emc->base.name = device_name;
	emc->base.device_type = device_type;

	// Private fields.
	emc->instance = &emsi;
	emc->pose = default_pose;
	emc->log_level = debug_get_log_option_sample_log();
	emc->hand_joints = new struct _em_proto_HandJointLocation[26];

	// Print name.
	snprintf(emc->base.str, XRT_DEVICE_NAME_LEN, "Hand %s Controller (Electric Maple)", hand_str);
	snprintf(emc->base.serial, XRT_DEVICE_NAME_LEN, "N/A S/N");

	// Setup input.
	switch (device_name) {
	case XRT_DEVICE_SIMPLE_CONTROLLER:
		emc->base.inputs[0].name = XRT_INPUT_INDEX_TRIGGER_VALUE;
		emc->base.inputs[1].name = XRT_INPUT_INDEX_B_CLICK;
		emc->base.inputs[2].name = XRT_INPUT_INDEX_GRIP_POSE;
		emc->base.inputs[3].name = XRT_INPUT_INDEX_AIM_POSE;
		if (device_type == XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER) {
			emc->base.inputs[4].name = XRT_INPUT_GENERIC_HAND_TRACKING_LEFT;
		} else {
			emc->base.inputs[4].name = XRT_INPUT_GENERIC_HAND_TRACKING_RIGHT;
		}

		emc->base.outputs[0].name = XRT_OUTPUT_NAME_INDEX_HAPTIC;
		break;
	default: assert(false);
	}

	ems_callbacks_add(emsi.callbacks, EMS_CALLBACKS_EVENT_CONTROLLER, controller_handle_data, emc);

	// Lastly, setup variable tracking.
	u_var_add_root(emc, emc->base.str, true);
	u_var_add_pose(emc, &emc->pose, "pose");
	u_var_add_log_level(emc, &emc->log_level, "log_level");

	return emc;
}

// Has to be standard layout because of the first element casts we do.
static_assert(std::is_standard_layout<struct ems_motion_controller>::value);
