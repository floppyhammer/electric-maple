// Copyright 2024, Collabora, Ltd.
//
// SPDX-License-Identifier: BSL-1.0

/*!
 * @file
 * @brief  Implementation for device specific configuration.
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 */

#include "ems_config.h"

#include <json-glib/json-glib.h>

#include "util/u_logging.h"

static struct ems_device_config config_instance;

const struct ems_device_config *
ems_config_get(void)
{
	return &config_instance;
}

void
ems_config_init_default(void)
{
	config_instance = (struct ems_device_config){
	    .refresh_rate_hz = 72,
	    .resolution_native_per_eye_pixels =
	        {
	            .w = 1680,
	            .h = 1760,
	        },
	    // At full resolution [2 * native width, native height].
	    // Using 25% of the native pixels for the stream due to the readback / encoding bottleneck.
	    .resolution_stream_stereo_pixels =
	        {
	            .w = 1680,
	            .h = 880,
	        },
	    .fov_radians =
	        {
	            {
	                .angle_left = -0.942f,
	                .angle_right = 0.698f,
	                .angle_up = 0.768f,
	                .angle_down = -0.960f,
	            },
	            {
	                .angle_left = -0.698f,
	                .angle_right = 0.942f,
	                .angle_up = 0.768f,
	                .angle_down = -0.960f,
	            },
	        },
	};
}

bool
ems_config_init_from_json(const gchar *path_str)
{
	JsonParser *parser = json_parser_new();

	GError *error = NULL;
	json_parser_load_from_file(parser, path_str, &error);
	if (error) {
		U_LOG_E("Unable to parse device config at `%s': %s", path_str, error->message);
		g_error_free(error);
		g_object_unref(parser);
		return false;
	}

	JsonNode *root = json_parser_get_root(parser);
	JsonReader *reader = json_reader_new(root);

	if (!json_reader_read_member(reader, "refresh_rate_hz")) {
		U_LOG_E("Failed to read `refresh_rate_hz` from JSON.");
		return false;
	}
	config_instance.refresh_rate_hz = json_reader_get_int_value(reader);
	json_reader_end_member(reader);

	if (!json_reader_read_member(reader, "resolution_native_per_eye_pixels")) {
		U_LOG_E("Failed to read `resolution_native_per_eye_pixels` from JSON.");
		return false;
	}
	if (!json_reader_read_member(reader, "width")) {
		U_LOG_E("Failed to read `width` from JSON.");
		return false;
	}
	config_instance.resolution_native_per_eye_pixels.w = json_reader_get_int_value(reader);
	json_reader_end_member(reader);
	if (!json_reader_read_member(reader, "height")) {
		U_LOG_E("Failed to read `height` from JSON.");
		return false;
	}
	config_instance.resolution_native_per_eye_pixels.h = json_reader_get_int_value(reader);
	json_reader_end_member(reader);
	json_reader_end_member(reader);

	if (!json_reader_read_member(reader, "resolution_stream_stereo_pixels")) {
		U_LOG_E("Failed to read `resolution_stream_stereo_pixels` from JSON.");
		return false;
	}
	if (!json_reader_read_member(reader, "width")) {
		U_LOG_E("Failed to read `width` from JSON.");
		return false;
	}
	config_instance.resolution_stream_stereo_pixels.w = json_reader_get_int_value(reader);
	json_reader_end_member(reader);
	if (!json_reader_read_member(reader, "height")) {
		U_LOG_E("Failed to read `height` from JSON.");
		return false;
	}
	config_instance.resolution_stream_stereo_pixels.h = json_reader_get_int_value(reader);
	json_reader_end_member(reader);
	json_reader_end_member(reader);

	if (!json_reader_read_member(reader, "fov_radians")) {
		U_LOG_E("Failed to read `fov_radians` from JSON.");
		return false;
	}
	for (uint32_t i = 0; i < 2; i++) {
		if (!json_reader_read_element(reader, i)) {
			U_LOG_E("Failed to read element %d from JSON.", i);
			return false;
		}

		if (!json_reader_read_member(reader, "angle_left")) {
			U_LOG_E("Failed to read `angle_left` from JSON.");
			return false;
		}
		config_instance.fov_radians[i].angle_left = json_reader_get_double_value(reader);
		json_reader_end_member(reader);

		if (!json_reader_read_member(reader, "angle_right")) {
			U_LOG_E("Failed to read `angle_right` from JSON.");
			return false;
		}
		config_instance.fov_radians[i].angle_right = json_reader_get_double_value(reader);
		json_reader_end_member(reader);

		if (!json_reader_read_member(reader, "angle_up")) {
			U_LOG_E("Failed to read `angle_up` from JSON.");
			return false;
		}
		config_instance.fov_radians[i].angle_up = json_reader_get_double_value(reader);
		json_reader_end_member(reader);

		if (!json_reader_read_member(reader, "angle_down")) {
			U_LOG_E("Failed to read `angle_down` from JSON.");
			return false;
		}
		config_instance.fov_radians[i].angle_down = json_reader_get_double_value(reader);
		json_reader_end_member(reader);

		json_reader_end_element(reader);
	}
	json_reader_end_member(reader);

	g_object_unref(reader);
	g_object_unref(parser);

	return true;
}
