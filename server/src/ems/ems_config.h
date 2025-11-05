// Copyright 2024, Collabora, Ltd.
//
// SPDX-License-Identifier: BSL-1.0

/*!
 * @file
 * @brief  Header for device specific configuration.
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 */

#pragma once

#include <stdint.h>
#include <glib.h>

#include "xrt/xrt_defines.h"

G_BEGIN_DECLS

struct ems_device_config
{
	uint32_t refresh_rate_hz;
	struct xrt_size resolution_native_per_eye_pixels;
	struct xrt_size resolution_stream_stereo_pixels;
	struct xrt_fov fov_radians[2];
};

const struct ems_device_config *
ems_config_get(void);

void
ems_config_init_default(void);

bool
ems_config_init_from_json(const gchar *path_str);

G_END_DECLS
