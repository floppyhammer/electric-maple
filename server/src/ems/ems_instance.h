// Copyright 2020-2025, Collabora, Ltd.
// Copyright 2023, Pluto VR, Inc.
//
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "xrt/xrt_instance.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_tracking.h"

struct ems_hmd;
struct ems_motion_controller;
struct ems_callbacks;

struct ems_instance
{
	//! Base class for devices.
	xrt_device xdev_base;

	//! Instance base.
	xrt_instance xinst_base;

	//! System, implemented for now using helper code.
	struct u_system *usys;

	//! System devices base.
	xrt_system_devices xsysd_base;

	//! Space overseer, implemented for now using helper code.
	xrt_space_overseer *xso;

	//! Shared tracking origin for all devices.
	xrt_tracking_origin tracking_origin;

	// For convenience
	ems_hmd *head;
	ems_motion_controller *left;
	ems_motion_controller *right;

	// Device index for hand controllers
	int32_t left_index;
	int32_t right_index;

	//! Callbacks collection
	ems_callbacks *callbacks;
};

/*!
 * Creates a @ref ems_compositor.
 *
 * @ingroup comp_ems
 */
xrt_result_t
ems_compositor_create_system(ems_instance &emsi, xrt_system_compositor **out_xsysc);
