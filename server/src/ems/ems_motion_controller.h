// Copyright 2020-2025, Collabora, Ltd.
// Copyright 2023, Pluto VR, Inc.
//
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "electricmaple.pb.h"
#include "util/u_logging.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_device.h"

struct ems_instance;

struct ems_motion_controller
{
	//! Has to come first.
	xrt_device base;

	xrt_pose grip_pose;

	xrt_pose aim_pose;

	u_logging_level log_level;

	bool active;

	float grab_action;

	em_proto_HandJointLocation *hand_joints;
};

ems_motion_controller *
ems_motion_controller_create(ems_instance &emsi, xrt_device_name device_name, xrt_device_type device_type);