// Copyright 2020-2023, Collabora, Ltd.
// Copyright 2023, Pluto VR, Inc.
//
// SPDX-License-Identifier: BSL-1.0

/*!
 * @file
 * @brief  Header
 * @author Rylie Pavlik <rpavlik@collabora.com>
 * @author Moshi Turner <moses@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Christoph Haag <christoph.haag@collabora.com>
 * @ingroup xrt_fs_em
 */

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>

struct em_sample
{
	GLuint frame_texture_id;
	GLenum frame_texture_target;

	bool have_poses;
	XrPosef poses[2];

	int64_t frame_sequence_id;
	int64_t server_render_begin_time;
	int64_t server_push_time;
	int64_t client_receive_time;
	int64_t client_decode_time;
	int64_t client_render_begin_time;
};
