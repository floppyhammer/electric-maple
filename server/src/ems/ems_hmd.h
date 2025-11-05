// Copyright 2020-2025, Collabora, Ltd.
// Copyright 2023, Pluto VR, Inc.
//
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include "xrt/xrt_defines.h"
#include "xrt/xrt_device.h"

#include "util/u_logging.h"

#include "electricmaple.pb.h"

#define USE_PREDICTION

struct ems_callbacks;
struct ems_instance;
struct ems_hmd;

struct ems_hmd_recvbuf
{
	std::atomic_bool updated;
	std::mutex mutex;

#ifdef USE_PREDICTION
	uint64_t timestamp;
	xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
#else
	struct xrt_pose pose;
#endif
};

struct ems_hmd
{
	//! Has to come first.
	xrt_device base;

#ifdef USE_PREDICTION
	struct m_relation_history *pose_history;
#else
	struct xrt_pose pose;
#endif

	std::unique_ptr<ems_hmd_recvbuf> received;
	u_logging_level log_level;
};

ems_hmd *
ems_hmd_create(ems_instance &emsi);