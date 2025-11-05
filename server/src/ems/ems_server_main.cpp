// Copyright 2020, Collabora, Ltd.
// Copyright 2023, Pluto VR, Inc.
//
// SPDX-License-Identifier: BSL-1.0

/*!
 * @file
 * @brief  Main file for Monado service.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup ipc
 */

#include "gst/ems_pipeline_args.h"
#include "server/ipc_server_interface.h"
#include "util/u_logging.h"
#include "util/u_metrics.h"
#include "util/u_trace_marker.h"
#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_WINDOWS
#include "util/u_windows.h"
#endif

// Insert on load constructor to init trace marker.
U_TRACE_TARGET_SETUP(U_TRACE_WHICH_SERVICE)

int
main(int argc, char *argv[])
{
	if (!ems_arguments_parse(argc, argv)) {
		U_LOG_E("Could not parse command line arguments.");
		return -1;
	}

#ifdef XRT_OS_WINDOWS
	u_win_try_privilege_or_priority_from_args(U_LOGGING_INFO, argc, argv);
#endif

	u_trace_marker_init();
	u_metrics_init();

	struct ipc_server_main_info ismi = {
		u_debug_gui_create_info{
			"EMS",
			U_DEBUG_GUI_OPEN_AUTO,
		},
	};

	int ret = ipc_server_main(argc, argv, &ismi);

	u_metrics_close();

	return ret;
}