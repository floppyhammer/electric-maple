// Copyright 2022-2023, Pluto VR, Inc.
//
// SPDX-License-Identifier: BSL-1.0

/*!
 * @file
 * @brief  Internal header for the connection module of the ElectricMaple XR streaming solution
 * @author Rylie Pavlik <rpavlik@collabora.com>
 * @ingroup em_client
 */
#pragma once

#include <glib-object.h>
#include <gst/gstpipeline.h>
#include <stdbool.h>

#include "em_status.h"

G_BEGIN_DECLS

#define EM_TYPE_CONNECTION em_connection_get_type()

G_DECLARE_FINAL_TYPE(EmConnection, em_connection, EM, CONNECTION, GObject)

/*!
 * Create a connection object
 *
 * @param websocket_uri The websocket URI to connect to. Ownership does not transfer (we copy it)
 *
 * @memberof EmConnection
 */
EmConnection *em_connection_new(const gchar *websocket_uri);

EmConnection *em_connection_new_localhost();

/*!
 * Actually start connecting to the server
 *
 * @memberof EmConnection
 */
void em_connection_connect(EmConnection *em_conn);

/*!
 * Drop the server connection, if any.
 *
 * @memberof EmConnection
 */
void em_connection_disconnect(EmConnection *em_conn);

/*!
 * Send a message to the server
 *
 * @memberof EmConnection
 */
bool em_connection_send_bytes(EmConnection *em_conn, GBytes *bytes);

/*!
 * Assign a pipeline for use.
 *
 * Will be started when the websocket connection comes up in order to negotiate using the webrtcbin.
 */
void em_connection_set_pipeline(EmConnection *em_conn, GstPipeline *pipeline);

enum em_status em_connection_get_status(EmConnection *em_conn);

G_END_DECLS
