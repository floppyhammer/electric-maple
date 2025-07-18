// Copyright 2020-2023, Collabora, Ltd.
// Copyright 2023, Pluto VR, Inc.
//
// SPDX-License-Identifier: BSL-1.0

/*!
 * @file
 * @brief  Shared default implementation of the instance with compositor.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 */

#include <assert.h>

#include "../compositor/main/comp_main_interface.h"
#include "ems_callbacks.h"
#include "ems_server_internal.h"
#include "util/u_builders.h"
#include "util/u_misc.h"
#include "util/u_system.h"
#include "util/u_trace_marker.h"
#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_system.h"

namespace {
inline struct ems_instance *from_xinst(struct xrt_instance *xinst) {
    return container_of(xinst, struct ems_instance, xinst_base);
}

inline struct ems_instance *from_xsysd(struct xrt_system_devices *xsysd) {
    return container_of(xsysd, struct ems_instance, xsysd_base);
}

/*
 *
 * System devices functions.
 *
 */

void ems_instance_system_devices_destroy(struct xrt_system_devices *xsysd) {
    struct ems_instance *emsi = from_xsysd(xsysd);

    ems_callbacks_reset(emsi->callbacks);

    for (size_t i = 0; i < xsysd->xdev_count; i++) {
        xrt_device_destroy(&xsysd->xdevs[i]);
    }

    (void)emsi; // We are a part of ems_instance, do not free.
}

static xrt_result_t ems_instance_system_devices_get_roles(struct xrt_system_devices *xsysd,
                                                          struct xrt_system_roles *out_roles) {
    struct ems_instance *emsi = from_xsysd(xsysd);

    struct xrt_system_roles roles = XRT_SYSTEM_ROLES_INIT;
    roles.generation_id = 1; // Never changes.

    // Assign hand controllers
    roles.left = emsi->left_index;
    roles.right = emsi->right_index;

    *out_roles = roles;

    return XRT_SUCCESS;
}

/*
 *
 * Instance functions.
 *
 */

xrt_result_t ems_instance_get_prober(struct xrt_instance *xinst, struct xrt_prober **out_xp) {
    return XRT_ERROR_PROBER_NOT_SUPPORTED;
}

xrt_result_t ems_instance_create_system(struct xrt_instance *xinst,
                                        struct xrt_system **out_xsys,
                                        struct xrt_system_devices **out_xsysd,
                                        struct xrt_space_overseer **out_xso,
                                        struct xrt_system_compositor **out_xsysc) {
    assert(out_xsys != NULL);
    assert(*out_xsys == NULL);
    assert(out_xsysd != NULL);
    assert(*out_xsysd == NULL);
    assert(out_xso != NULL);
    assert(*out_xso == NULL);
    assert(out_xsysc == NULL || *out_xsysc == NULL);

    struct ems_instance *emsi = from_xinst(xinst);

    emsi->usys = u_system_create();
    assert(emsi->usys != NULL); // Should never fail.

    u_system_fill_properties(emsi->usys, emsi->xsysd_base.static_roles.head->str);
    *out_xsys = &emsi->usys->base;
    *out_xsysd = &emsi->xsysd_base;
    *out_xso = emsi->xso;

    // Early out if we only want devices.
    if (out_xsysc == NULL) {
        return XRT_SUCCESS;
    }

    struct xrt_system_compositor *xsysc = NULL;
    xrt_result_t xret = ems_compositor_create_system(*emsi, &xsysc);

    // Tell the system about the system compositor.
    u_system_set_system_compositor(emsi->usys, xsysc);

    *out_xsysc = xsysc;

    return XRT_SUCCESS;
}

void ems_instance_destroy(struct xrt_instance *xinst) {
    struct ems_instance *emsi = from_xinst(xinst);

    ems_callbacks_reset(emsi->callbacks);

    ems_callbacks_destroy(&emsi->callbacks);

    delete emsi;
}

/*
 *
 * Exported function(s).
 *
 */

void ems_instance_system_devices_init(struct ems_instance *emsi) {
    // needed before creating devices
    emsi->callbacks = ems_callbacks_create();

    emsi->xsysd_base.destroy = ems_instance_system_devices_destroy;
    emsi->xsysd_base.get_roles = ems_instance_system_devices_get_roles;

    xrt_tracking_origin &origin = emsi->tracking_origin;
    origin.type = XRT_TRACKING_TYPE_OTHER;
    origin.initial_offset = (xrt_pose)XRT_POSE_IDENTITY;
    snprintf(origin.name, ARRAY_SIZE(origin.name), "Electric Maple Server Tracking Space");

    struct ems_hmd *eh = ems_hmd_create(*emsi);
    struct ems_motion_controller *emcl = ems_motion_controller_create( //
        *emsi,                                                         //
        XRT_DEVICE_SIMPLE_CONTROLLER,                                  //
        XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER);                         //
    struct ems_motion_controller *emcr = ems_motion_controller_create( //
        *emsi,                                                         //
        XRT_DEVICE_SIMPLE_CONTROLLER,                                  //
        XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER);                        //

    emsi->head = eh;
    emsi->left = emcl;
    emsi->right = emcr;

    struct xrt_device *head = &eh->base;
    struct xrt_device *left = &emcl->base;
    struct xrt_device *right = &emcr->base;

    // Set up the device base as the only device.
    emsi->xsysd_base.xdevs[0] = head;
    emsi->xsysd_base.xdevs[1] = left;
    emsi->left_index = 1;
    emsi->xsysd_base.xdevs[2] = right;
    emsi->right_index = 2;
    emsi->xsysd_base.xdev_count = 3;
    emsi->xsysd_base.static_roles.head = head;
    emsi->xsysd_base.static_roles.hand_tracking.unobstructed.left = left;
    emsi->xsysd_base.static_roles.hand_tracking.unobstructed.right = right;

    u_builder_create_space_overseer_legacy( //
        &emsi->usys->broadcast,             // broadcast
        head,                               // head
        left,                               // left
        right,                              // right
        nullptr,
        emsi->xsysd_base.xdevs,      // xdevs
        emsi->xsysd_base.xdev_count, // xdev_count
        false,                       // root_is_unbounded
        true,                        // per_app_local_spaces
        &emsi->xso);                 // out_xso
}

void ems_instance_init(struct ems_instance *emsi) {
    emsi->xinst_base.create_system = ems_instance_create_system;
    emsi->xinst_base.get_prober = ems_instance_get_prober;
    emsi->xinst_base.destroy = ems_instance_destroy;
}

} // namespace

xrt_result_t xrt_instance_create(struct xrt_instance_info *ii, struct xrt_instance **out_xinst) {
    u_trace_marker_init();

    struct ems_instance *emsi = new ems_instance();

    ems_instance_system_devices_init(emsi);
    ems_instance_init(emsi);

    *out_xinst = &emsi->xinst_base;

    return XRT_SUCCESS;
}
