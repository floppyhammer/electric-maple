// Copyright 2024, Collabora, Ltd.
//
// SPDX-License-Identifier: BSL-1.0

/*!
 * @file
 * @brief  Header for remote rendering pipeline arguments.
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 */

#pragma once

#include <gio/gio.h>
#include <stdint.h>

G_BEGIN_DECLS


typedef enum
{
	EMS_ENCODER_TYPE_X264,
	EMS_ENCODER_TYPE_NVH264,
	EMS_ENCODER_TYPE_NVAUTOGPUH264,
	EMS_ENCODER_TYPE_VULKANH264,
	EMS_ENCODER_TYPE_OPENH264,
	EMS_ENCODER_TYPE_VAAPIH264,
	EMS_ENCODER_TYPE_VAH264,
	EMS_ENCODER_TYPE_AMC,
	EMS_ENCODER_TYPE_AUTO,
} EmsEncoderType;

struct ems_arguments
{
	// GFile *stream_debug_file;
	uint32_t bitrate;
	EmsEncoderType encoder_type;
	gboolean benchmark_down_msg_loss;
	gboolean benchmark_latency;
	gboolean use_localhost;
	guint webrtc_stats_print_interval;
	// GFile *webrtc_stats_out_directory;
	gboolean use_udp;
};

const struct ems_arguments *
ems_arguments_get(void);

gboolean
ems_arguments_parse(int argc, char *argv[]);

G_END_DECLS