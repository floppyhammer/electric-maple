// Copyright 2022-2023, Pluto VR, Inc.
//
// SPDX-License-Identifier: BSL-1.0

/*!
 * @file
 * @brief  Header for the stream client module of the ElectricMaple XR streaming solution
 * @author Rylie Pavlik <rpavlik@collabora.com>
 * @ingroup em_client
 */
#pragma once

#include <EGL/egl.h>
#include <glib-object.h>
#include <stdbool.h>

#include "em_connection.h"

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

struct em_sample;

typedef struct EmEglMutexIface EmEglMutexIface;

// #define EM_TYPE_STREAM_CLIENT em_stream_client_get_type()

// G_DECLARE_FINAL_TYPE(EmStreamClient, em_stream_client, EM, STREAM_CLIENT, GObject)

typedef struct _EmStreamClient EmStreamClient;

/*!
 * Create a stream client object, providing the connection object
 *
 * @memberof EmStreamClient
 */
EmStreamClient *em_stream_client_new();

/*!
 * Clear a pointer and free the associate stream client, if any.
 *
 * Handles null checking for you.
 */
void em_stream_client_destroy(EmStreamClient **ptr_sc);

/*!
 * Initialize the EGL context and surface.
 *
 * Must be called from a thread where it is safe to make the provided context active.
 * After calling this method, **do not** manually make this context active again: instead use
 * @ref em_stream_client_egl_begin and @ref em_stream_client_egl_end
 *
 * @param sc self
 * @param egl_mutex An implementation of the EGL mutex interface, which carries an EGLDisplay and EGLContext
 * @param adopt_mutex_interface True if the stream client takes ownership of the EGL mutex interface.
 * @param pbuffer_surface An EGL pbuffer surface created for the @p context
 * TODO not sure what the surface is actually used for...
 */
void em_stream_client_set_egl_context(EmStreamClient *sc,
                                      EmEglMutexIface *egl_mutex,
                                      bool adopt_mutex_interface,
                                      EGLSurface pbuffer_surface);

/*!
 * Lock the mutex for the "main" EGL context supplied via @ref em_stream_client_set_egl_context and set it as current,
 * with your choice of EGL surfaces.
 *
 * @return true if successful - you will need to call @ref em_stream_client_egl_end when done using EGL/GL/GLES to
 * restore previous context/surfaces and unlock.
 */
bool em_stream_client_egl_begin(EmStreamClient *sc, EGLSurface draw, EGLSurface read);

/*!
 * Lock the mutex for the "main" EGL context supplied via @ref em_stream_client_set_egl_context and set it as current,
 * using the pbuffer surface supplied to that same function.
 *
 * Works just like @ref em_stream_client_egl_begin except it uses the surface you already told us about.
 *
 * @return true if successful - you will need to call @ref em_stream_client_egl_end when done using EGL/GL/GLES to
 * restore previous context/surfaces and unlock.
 */
bool em_stream_client_egl_begin_pbuffer(EmStreamClient *sc);

/*!
 * Restore previous EGL context and surfaces, and unlock the mutex for the "main" EGL context supplied via @ref
 * em_stream_client_set_egl_context
 */
void em_stream_client_egl_end(EmStreamClient *sc);

/*!
 * Start the GMainLoop embedded in this object in a new thread
 *
 * @param connection The connection to use
 */
void em_stream_client_spawn_thread(EmStreamClient *sc, EmConnection *connection);

/*!
 * Stop the pipeline and the mainloop thread.
 */
void em_stream_client_stop(EmStreamClient *sc);

/*!
 * Attempt to retrieve a sample, if one has been decoded.
 *
 * Non-null return values need to be released with @ref em_stream_client_release_sample.

* @param sc self
* @param[out] out_decode_end struct to populate with decode-end time.
 */
struct em_sample *em_stream_client_try_pull_sample(EmStreamClient *sc, struct timespec *out_decode_end);

/*!
 * Release a sample returned from @ref em_stream_client_try_pull_sample
 */
void em_stream_client_release_sample(EmStreamClient *sc, struct em_sample *ems);

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus
