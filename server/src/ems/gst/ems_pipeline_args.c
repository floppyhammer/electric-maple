// Copyright 2024, Collabora, Ltd.
//
// SPDX-License-Identifier: BSL-1.0

/*!
 * @file
 * @brief  Implementation for remote rendering pipeline arguments.
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 */

#include "ems_pipeline_args.h"

#include <gio/gfile.h>

#include "ems_config.h"
#include "util/u_logging.h"

#ifndef G_OPTION_ENTRY_NULL
#define G_OPTION_ENTRY_NULL                                                                                            \
	{                                                                                                              \
		NULL, 0, 0, 0, NULL, NULL, NULL                                                                        \
	}
#endif

static struct ems_arguments arguments_instance = {0};

const struct ems_arguments *
ems_arguments_get(void)
{
	return &arguments_instance;
}

// defaults
static gchar *output_file_name = NULL;
static gchar *encoder_name = NULL;
static gchar *device_config_json_path = "configs/bb.json";
static gboolean benchmark_down_msg_loss = FALSE;
static gboolean benchmark_latency = FALSE;
static gboolean use_localhost = FALSE;
static gint bitrate = 16384;
static EmsEncoderType default_encoder_type = EMS_ENCODER_TYPE_X264;

gboolean
ems_arguments_parse(int argc, char *argv[])
{
	GError *error = NULL;
	GOptionContext *context;

	// clang-format off
	const GOptionEntry entries[] = {
		{"stream-output-file-path", 'o', 0, G_OPTION_ARG_FILENAME, &output_file_name, "Path to store the stream in a MKV file.", "path"},
		{"bitrate", 'b', 0, G_OPTION_ARG_INT, &bitrate, "Stream bitrate", "N"},
		{"encoder", 'e', 0, G_OPTION_ARG_STRING, &encoder_name, "Encoder (x264, openh264, vulkanh264, vaapih264, vah264, nvh264, nvautogpuh264)", "str"},
		{"config", 'c', 0, G_OPTION_ARG_STRING, &device_config_json_path, "Path to device config JSON", "str"},
		{"benchmark-down-msg-loss", 0, 0, G_OPTION_ARG_NONE, &benchmark_down_msg_loss, "Benchmark DownMessage Loss", NULL},
		{"benchmark-latency", 0, 0, G_OPTION_ARG_NONE, &benchmark_latency, "Benchmark server compositor begin to after client decode time", NULL},
		{"use-localhost", 'l', 0, G_OPTION_ARG_NONE, &use_localhost, "Connect the electric maple client through localhost for network-over-usb", NULL},
		G_OPTION_ENTRY_NULL,
	};
	// clang-format on

	context = g_option_context_new("- Elecric Maple streaming server");
	g_option_context_add_main_entries(context, entries, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		U_LOG_E("Option parsing failed: %s", error->message);
		return FALSE;
	}

	// if (output_file_name) {
	// 	arguments_instance.stream_debug_file = g_file_new_for_path(output_file_name);
	// }

	arguments_instance.bitrate = bitrate;
	arguments_instance.benchmark_down_msg_loss = benchmark_down_msg_loss;
	arguments_instance.benchmark_latency = benchmark_latency;
	arguments_instance.use_localhost = use_localhost;

	if (encoder_name) {
		if (g_strcmp0(encoder_name, "nvh264") == 0) {
			arguments_instance.encoder_type = EMS_ENCODER_TYPE_NVH264;
		} else if (g_strcmp0(encoder_name, "nvautogpuh264") == 0) {
			arguments_instance.encoder_type = EMS_ENCODER_TYPE_NVAUTOGPUH264;
		} else if (g_strcmp0(encoder_name, "x264") == 0) {
			arguments_instance.encoder_type = EMS_ENCODER_TYPE_X264;
		} else if (g_strcmp0(encoder_name, "vulkanh264") == 0) {
			arguments_instance.encoder_type = EMS_ENCODER_TYPE_VULKANH264;
		} else if (g_strcmp0(encoder_name, "openh264") == 0) {
			arguments_instance.encoder_type = EMS_ENCODER_TYPE_OPENH264;
		} else if (g_strcmp0(encoder_name, "vaapih264") == 0) {
			arguments_instance.encoder_type = EMS_ENCODER_TYPE_VAAPIH264;
		} else if (g_strcmp0(encoder_name, "vah264") == 0) {
			arguments_instance.encoder_type = EMS_ENCODER_TYPE_VAH264;
		} else {
			U_LOG_W("Unknown encoder option '%s'. Falling back to default.", encoder_name);
			arguments_instance.encoder_type = default_encoder_type;
		}
	} else {
		arguments_instance.encoder_type = default_encoder_type;
	}

	g_print(device_config_json_path);

	if (device_config_json_path) {
		if (!ems_config_init_from_json(device_config_json_path)) {
			U_LOG_W("Failed to load json config from `%s`, using default config.", device_config_json_path);
			ems_config_init_default();
		}
	} else {
		ems_config_init_default();
	}

	g_option_context_free(context);

	return TRUE;
}