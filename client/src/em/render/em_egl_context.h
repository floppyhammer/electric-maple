// Copyright 2025, Collabora, Inc.
// SPDX-License-Identifier: BSL-1.0

/*!
 * @file
 * @brief Electric Maple EGL context
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 */

#pragma once

#include <glib-object.h>
#include <stdbool.h>
#include <jni.h> // needed by openxr_platform
#include <EGL/egl.h>
#include <openxr/openxr_platform.h>

G_BEGIN_DECLS

#define EM_TYPE_EGL_CONTEXT em_egl_context_get_type()
G_DECLARE_FINAL_TYPE(EmEglContext, em_egl_context, EM, EGL_CONTEXT, GObject)

EmEglContext *
em_egl_context_new(void);

XrGraphicsBindingOpenGLESAndroidKHR
em_egl_context_get_graphics_binding(EmEglContext *self);

bool
em_egl_context_make_current(EmEglContext *self);

EGLContext
em_egl_context_get_context(EmEglContext *self);

G_END_DECLS