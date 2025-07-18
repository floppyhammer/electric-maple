// Copyright 2020-2023, Collabora, Ltd.
// Copyright 2023, Pluto VR, Inc.
//
// SPDX-License-Identifier: BSL-1.0

/*!
 * @file
 */

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "os/os_threading.h"
#include "util/u_logging.h"
#include "util/u_pacing.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_tracking.h"

struct ems_callbacks;
struct ems_instance;
struct ems_hmd;

struct ems_hmd_recvbuf {
    std::atomic_bool updated;
    std::mutex mutex;
    struct xrt_pose pose;
};

struct ems_hmd {
    //! Has to come first.
    struct xrt_device base;

    struct xrt_pose pose;

    // Should outlive us
    struct ems_instance *instance;

    // struct os_mutex mutex;
    std::unique_ptr<ems_hmd_recvbuf> received;
    enum u_logging_level log_level;
};

struct ems_motion_controller {
    //! Has to come first.
    struct xrt_device base;

    struct xrt_pose pose;

    // Should outlive us
    struct ems_instance *instance;

    enum u_logging_level log_level;

    bool active;

    float hand_grab;

    struct _em_proto_HandJointLocation *hand_joints;
};

struct ems_instance {
    //! Base class for devices.
    struct xrt_device xdev_base;

    //! Instance base.
    struct xrt_instance xinst_base;

    //! System, implemented for now using helper code.
    struct u_system *usys;

    //! System devices base.
    struct xrt_system_devices xsysd_base;

    //! Space overseer, implemented for now using helper code.
    struct xrt_space_overseer *xso;

    //! Shared tracking origin for all devices.
    struct xrt_tracking_origin tracking_origin;

    // For convenience
    struct ems_hmd *head;
    struct ems_motion_controller *left;
    struct ems_motion_controller *right;

    // Device index for hand controllers
    int32_t left_index;
    int32_t right_index;

    //! Callbacks collection
    struct ems_callbacks *callbacks;
};

// compositor interface functions

/*!
 * Creates a @ref ems_compositor.
 *
 * @ingroup comp_ems
 */
xrt_result_t ems_compositor_create_system(ems_instance &emsi, struct xrt_system_compositor **out_xsysc);

// driver interface functions

struct ems_hmd *ems_hmd_create(ems_instance &emsi);

struct ems_motion_controller *ems_motion_controller_create(ems_instance &emsi,
                                                           enum xrt_device_name device_name,
                                                           enum xrt_device_type device_type);
