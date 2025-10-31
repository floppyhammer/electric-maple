// Copyright 2023-2025, Collabora, Inc.
// Copyright 2023, Pluto VR, Inc.
// SPDX-License-Identifier: BSL-1.0

/*!
 * @file
 * @brief Electric Maple EGL context
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * @author Moshi Turner <moses@collabora.com>
 * @author Rylie Pavlik <rpavlik@collabora.com>
 */

#include "em_egl_context.h"
#include "em/em_app_log.h"
#include "GLError.h"

#define MAX_CONFIGS 1024

struct _EmEglContext
{
	GObject parent;
	EGLDisplay display;
	EGLContext context;
	EGLSurface surface;
	EGLConfig config;
};

G_DEFINE_TYPE(EmEglContext, em_egl_context, G_TYPE_OBJECT)

static void
em_egl_context_init(EmEglContext *self)
{
	self->display = EGL_NO_DISPLAY;
	self->context = EGL_NO_CONTEXT;
	self->surface = EGL_NO_SURFACE;
	self->config = NULL;
}

static bool
create(EmEglContext *self)
{
	self->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (self->display == EGL_NO_DISPLAY) {
		ALOGE("Failed to get EGL display");
		return false;
	}

	if (!eglInitialize(self->display, NULL, NULL)) {
		ALOGE("Failed to initialize EGL");
		return false;
	}

	// RGBA8, multisample not required, ES3, pbuffer and window
	// clang-format off
	const EGLint attributes[] = {
	    EGL_RED_SIZE, 8,
	    EGL_GREEN_SIZE, 8,
	    EGL_BLUE_SIZE, 8,
	    EGL_ALPHA_SIZE, 8,
	    EGL_SAMPLES, 1,
	    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
	    EGL_SURFACE_TYPE, (EGL_PBUFFER_BIT | EGL_WINDOW_BIT),
	    EGL_NONE,
	};
	// clang-format on

	EGLint num_configs = 0;
	EGLConfig configs[MAX_CONFIGS];
	CHK_EGL(eglChooseConfig(self->display, attributes, configs, MAX_CONFIGS, &num_configs));
	if (num_configs == 0) {
		ALOGE("Failed to find suitable EGL config");
		return false;
	}
	ALOGI("Got %d egl configs, just taking the first one.", num_configs);
	self->config = configs[0];

	EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
	CHK_EGL(self->context = eglCreateContext(self->display, self->config, EGL_NO_CONTEXT, context_attributes));
	if (self->context == EGL_NO_CONTEXT) {
		ALOGE("Failed to create EGL context");
		return false;
	}
	CHECK_EGL_ERROR();

	// TODO why are we making a 16x16 pbuffer surface? Do we even need it?
	// clang-format off
	EGLint surface_attributes[] = {
	    EGL_WIDTH, 16,
	    EGL_HEIGHT, 16,
	    EGL_NONE,
	};
	// clang-format on

	CHK_EGL(self->surface = eglCreatePbufferSurface(self->display, self->config, surface_attributes));
	if (self->surface == EGL_NO_SURFACE) {
		ALOGE("Failed to create EGL surface");
		eglDestroyContext(self->display, self->context);
		return false;
	}
	CHECK_EGL_ERROR();
	ALOGI("EGL: Successfully created EGL context, display and surface");

	return true;
}

EmEglContext *
em_egl_context_new(void)
{
	EmEglContext *self = (EmEglContext *)g_object_new(EM_TYPE_EGL_CONTEXT, 0);
	if (!create(self)) {
		g_object_unref(self);
		return NULL;
	}
	return self;
}

XrGraphicsBindingOpenGLESAndroidKHR
em_egl_context_get_graphics_binding(EmEglContext *self)
{
	XrGraphicsBindingOpenGLESAndroidKHR binding = {
	    .type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR,
	    .display = self->display,
	    .config = self->config,
	    .context = self->context,
	};
	return binding;
}

bool
em_egl_context_make_current(EmEglContext *self)
{
	if (eglMakeCurrent(self->display, self->surface, self->surface, self->context) == EGL_FALSE) {
		ALOGE("%s: Failed make egl context current", __FUNCTION__);
		return false;
	}
	return true;
}

static void
finalize(GObject *gobject)
{
	EmEglContext *self = EM_EGL_CONTEXT(gobject);
	if (self->display == EGL_NO_DISPLAY) {
		self->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	}
	if (self->surface != EGL_NO_SURFACE) {
		eglDestroySurface(self->display, self->surface);
		self->surface = EGL_NO_SURFACE;
	}
	if (self->context != EGL_NO_CONTEXT) {
		eglDestroyContext(self->display, self->context);
		self->context = EGL_NO_CONTEXT;
	}
	G_OBJECT_CLASS(em_egl_context_parent_class)->finalize(gobject);
}

static void
em_egl_context_class_init(EmEglContextClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = finalize;
}

EGLContext
em_egl_context_get_context(EmEglContext *self)
{
	return self->context;
}
