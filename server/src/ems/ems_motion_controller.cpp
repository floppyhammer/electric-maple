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

#include <cassert>
#include <cstdio>
#include <thread>

#include "electricmaple.pb.h"
#include "ems_callbacks.h"
#include "ems_server_internal.h"
#include "math/m_api.h"
#include "math/m_mathinclude.h"
#include "math/m_space.h"
#include "os/os_time.h"
#include "pb_decode.h"
#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "util/u_hand_simulation.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_var.h"
#include "xrt/xrt_device.h"

/*
 *
 * Structs and defines.
 *
 */

/// Casting helper function
static inline struct ems_motion_controller *ems_motion_controller(struct xrt_device *xdev) {
    return (struct ems_motion_controller *)xdev;
}

DEBUG_GET_ONCE_LOG_OPTION(sample_log, "EMS_LOG", U_LOGGING_WARN)

#define EMS_TRACE(p, ...) U_LOG_XDEV_IFL_T(&p->base, p->log_level, __VA_ARGS__)
#define EMS_DEBUG(p, ...) U_LOG_XDEV_IFL_D(&p->base, p->log_level, __VA_ARGS__)
#define EMS_ERROR(p, ...) U_LOG_XDEV_IFL_E(&p->base, p->log_level, __VA_ARGS__)

xrt_pose convert_pose(_em_proto_Pose pose) {
    //    assert(pose.has_position && pose.has_orientation);
    xrt_pose new_pose{};
    new_pose.position = {pose.position.x, pose.position.y, pose.position.z};

    new_pose.orientation.w = pose.orientation.w;
    new_pose.orientation.x = pose.orientation.x;
    new_pose.orientation.y = pose.orientation.y;
    new_pose.orientation.z = pose.orientation.z;

    return new_pose;
}

static void controller_destroy(struct xrt_device *xdev) {
    struct ems_motion_controller *emc = ems_motion_controller(xdev);

    // Remove the variable tracking.
    u_var_remove_root(emc);

    u_device_free(&emc->base);

    delete[] emc->hand_joints;
    emc->hand_joints = NULL;
}

// You should put code to update the attached input fields (if any)
static xrt_result_t controller_update_inputs(struct xrt_device *xdev) {
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

    xdev->inputs[0].active = true;
    xdev->inputs[0].timestamp = now;
    xdev->inputs[0].value.vec1 = {emc->grab_action};

    // AIM for ray casting.
    xdev->inputs[3].active = true;
    xdev->inputs[3].timestamp = now;

    return XRT_SUCCESS;
}

static xrt_result_t controller_set_output(struct xrt_device *xdev,
                                          enum xrt_output_name name,
                                          const xrt_output_value *value) {
    // Since we don't have a data channel yet, this is a no-op.
    return XRT_SUCCESS;
}

static xrt_result_t controller_get_hand_tracking(struct xrt_device *xdev,
                                                 enum xrt_input_name name,
                                                 int64_t requested_timestamp_ns,
                                                 struct xrt_hand_joint_set *out_value,
                                                 int64_t *out_timestamp_ns) {
    struct ems_motion_controller *emc = ems_motion_controller(xdev);

    if (name != XRT_INPUT_HT_UNOBSTRUCTED_LEFT && name != XRT_INPUT_HT_UNOBSTRUCTED_RIGHT) {
        U_LOG_E("Unknown input name for hand tracker: 0x%0x", name);
        return XRT_ERROR_INPUT_UNSUPPORTED;
    }

    out_value->hand_pose.pose = emc->grip_pose;
    m_space_relation_ident(&out_value->hand_pose);

    out_value->is_active = true;

    for (int i = 0; i < XRT_HAND_JOINT_COUNT; i++) {
        auto &joint_set = out_value->values.hand_joint_set_default[i];
        joint_set.relation.pose = convert_pose(emc->hand_joints[i].pose);

        joint_set.relation.relation_flags = (enum xrt_space_relation_flags)(
            XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
            XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT);

        joint_set.radius = emc->hand_joints[i].radius;
    }

    // This is a lie
    *out_timestamp_ns = requested_timestamp_ns;

    return XRT_SUCCESS;
}

static xrt_result_t controller_get_tracked_pose(struct xrt_device *xdev,
                                                enum xrt_input_name name,
                                                int64_t at_timestamp_ns,
                                                struct xrt_space_relation *out_relation) {
    struct ems_motion_controller *emc = ems_motion_controller(xdev);

    switch (name) {
        case XRT_INPUT_WMR_GRIP_POSE: {
            //            math_quat_normalize(&emc->grip_pose.orientation);
            //            out_relation->pose = emc->grip_pose;
            //            out_relation->relation_flags = (enum xrt_space_relation_flags) ( //
            //                    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |                  //
            //                    XRT_SPACE_RELATION_POSITION_VALID_BIT |                     //
            //                    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |                //
            //                    XRT_SPACE_RELATION_POSITION_TRACKED_BIT);                   //
        } break;
        case XRT_INPUT_WMR_AIM_POSE: {
            math_quat_normalize(&emc->aim_pose.orientation);
            out_relation->pose = emc->aim_pose;
            out_relation->relation_flags = (enum xrt_space_relation_flags)( //
                XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |                  //
                XRT_SPACE_RELATION_POSITION_VALID_BIT |                     //
                XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |                //
                XRT_SPACE_RELATION_POSITION_TRACKED_BIT);                   //
        } break;
        default: {
            EMS_ERROR(emc, "Unknown input name: %d", name);
            return XRT_ERROR_INPUT_UNSUPPORTED;
        }
    }

    // Estimate pose at timestamp at_timestamp_ns!

    return XRT_SUCCESS;
}

static xrt_result_t controller_get_view_poses(struct xrt_device *xdev,
                                              const struct xrt_vec3 *default_eye_relation,
                                              int64_t at_timestamp_ns,
                                              uint32_t view_count,
                                              struct xrt_space_relation *out_head_relation,
                                              struct xrt_fov *out_fovs,
                                              struct xrt_pose *out_poses) {
    assert(false);
    return XRT_ERROR_POSE_NOT_ACTIVE;
}

/// Fetch remote input data.
static void controller_handle_data(enum ems_callbacks_event event, const UpMessageSuper *messageSuper, void *userdata) {
    auto *emc = (struct ems_motion_controller *)userdata;
    emc->active = false;

    auto *message = &messageSuper->protoMessage;

    if (!message->has_tracking) {
        return;
    }

    if (emc->base.device_type == XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER) {
        if (message->tracking.has_controller_grip_left) {
            emc->grip_pose = convert_pose(message->tracking.controller_grip_left);
        }

        if (message->tracking.has_controller_aim_left) {
            emc->aim_pose = convert_pose(message->tracking.controller_aim_left);
        }

        memcpy(emc->hand_joints,
               messageSuper->hand_joint_locations_left,
               sizeof(em_proto_HandJointLocation) * XRT_HAND_JOINT_COUNT);

        emc->grab_action = message->tracking.controller_grab_value_left;
    } else if (emc->base.device_type == XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER) {
        if (message->tracking.has_controller_grip_right) {
            emc->grip_pose = convert_pose(message->tracking.controller_grip_right);
        }

        if (message->tracking.has_controller_aim_right) {
            emc->aim_pose = convert_pose(message->tracking.controller_aim_right);

            U_LOG_E("aim_pose %f %f %f, %f %f %f %f",
                    emc->aim_pose.position.x,
                    emc->aim_pose.position.y,
                    emc->aim_pose.position.z,
                    emc->aim_pose.orientation.x,
                    emc->aim_pose.orientation.y,
                    emc->aim_pose.orientation.z,
                    emc->aim_pose.orientation.w);
        }

        memcpy(emc->hand_joints,
               messageSuper->hand_joint_locations_right,
               sizeof(em_proto_HandJointLocation) * XRT_HAND_JOINT_COUNT);

        emc->grab_action = message->tracking.controller_grab_value_right;
    } else {
        return;
    }

    // U_LOG_E("handLocalPose %f %f %f", pose.position.x, pose.position.y, pose.position.z);
    // printf("hand grab %f\n", hand_grab);

    // TODO handle timestamp, etc

    emc->active = true;
}

/*
 *
 * Bindings
 *
 */

static struct xrt_binding_input_pair simple_inputs_index[4] = {
    {XRT_INPUT_SIMPLE_SELECT_CLICK, XRT_INPUT_WMR_TRIGGER_VALUE},
    {XRT_INPUT_SIMPLE_MENU_CLICK, XRT_INPUT_WMR_MENU_CLICK},
    {XRT_INPUT_SIMPLE_GRIP_POSE, XRT_INPUT_WMR_GRIP_POSE},
    {XRT_INPUT_SIMPLE_AIM_POSE, XRT_INPUT_WMR_AIM_POSE},
};

static struct xrt_binding_output_pair simple_outputs_index[1] = {
    {XRT_OUTPUT_NAME_SIMPLE_VIBRATION, XRT_OUTPUT_NAME_WMR_HAPTIC},
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

struct ems_motion_controller *ems_motion_controller_create(ems_instance &emsi,
                                                           enum xrt_device_name device_name,
                                                           enum xrt_device_type device_type) {
    uint32_t input_count;
    uint32_t output_count;

    switch (device_name) {
        case XRT_DEVICE_SIMPLE_CONTROLLER:
            input_count = ARRAY_SIZE(simple_inputs_index) + 1; // ARRAY_SIZE + hand tracker
            output_count = ARRAY_SIZE(simple_outputs_index);
            break;
        default:
            U_LOG_E("Device name not supported!");
            return nullptr;
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
        default:
            U_LOG_E("Device type not supported!");
            return nullptr;
    }

    // We don't need anything special from allocate except inputs and outputs.
    u_device_alloc_flags flags{};
    struct ems_motion_controller *emc =
        U_DEVICE_ALLOCATE(struct ems_motion_controller, flags, input_count, output_count);

    // Functions.
    emc->base.update_inputs = controller_update_inputs;
    emc->base.set_output = controller_set_output;
    emc->base.get_hand_tracking = controller_get_hand_tracking;
    emc->base.supported.hand_tracking = true;
    emc->base.get_tracked_pose = controller_get_tracked_pose;
    emc->base.get_view_poses = controller_get_view_poses;
    emc->base.destroy = controller_destroy;

    // Data.
    emc->base.tracking_origin = &emsi.tracking_origin;
    emc->base.binding_profiles = binding_profiles_index;
    emc->base.binding_profile_count = ARRAY_SIZE(binding_profiles_index);
    emc->base.supported.orientation_tracking = true;
    emc->base.supported.position_tracking = true;
    emc->base.name = device_name;
    emc->base.device_type = device_type;

    // Private fields.
    emc->instance = &emsi;
    emc->grip_pose = default_pose;
    emc->aim_pose = default_pose;
    emc->log_level = debug_get_log_option_sample_log();
    emc->hand_joints = new struct _em_proto_HandJointLocation[XRT_HAND_JOINT_COUNT];

    // Print name.
    snprintf(emc->base.str, XRT_DEVICE_NAME_LEN, "Hand %s Controller (Electric Maple)", hand_str);
    snprintf(emc->base.serial, XRT_DEVICE_NAME_LEN, "N/A S/N");

    // Setup input.
    switch (device_name) {
        case XRT_DEVICE_SIMPLE_CONTROLLER:
            emc->base.inputs[0].name = XRT_INPUT_WMR_TRIGGER_VALUE;
            emc->base.inputs[1].name = XRT_INPUT_WMR_MENU_CLICK;
            emc->base.inputs[2].name = XRT_INPUT_WMR_GRIP_POSE;
            emc->base.inputs[3].name = XRT_INPUT_WMR_AIM_POSE;
            if (device_type == XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER) {
                emc->base.inputs[4].name = XRT_INPUT_HT_UNOBSTRUCTED_LEFT;
            } else {
                emc->base.inputs[4].name = XRT_INPUT_HT_UNOBSTRUCTED_RIGHT;
            }

            emc->base.outputs[0].name = XRT_OUTPUT_NAME_WMR_HAPTIC;
            break;
        default:
            assert(false);
    }

    ems_callbacks_add(emsi.callbacks, EMS_CALLBACKS_EVENT_CONTROLLER, controller_handle_data, emc);

    // Lastly, setup variable tracking.
    u_var_add_root(emc, emc->base.str, true);
    u_var_add_pose(emc, &emc->grip_pose, "grip_pose");
    u_var_add_pose(emc, &emc->aim_pose, "aim_pose");
    u_var_add_log_level(emc, &emc->log_level, "log_level");

    return emc;
}

// Has to be standard layout because of the first element casts we do.
static_assert(std::is_standard_layout<struct ems_motion_controller>::value);
