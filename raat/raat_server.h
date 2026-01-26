//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_SERVER_H
#define INCLUDED_RAAT_SERVER_H

#include "rc_base.h"
#include "rc_allocator.h"
#include "rc_status.h"
#include "raat_fwd.h"
#include "raat_device.h"

#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup raat_server Server
 *
 *  \brief
 *  This module implements a TCP server that speaks the RAAT-LL protocol.
 *
 *  @{
 */

enum {
    RAAT__SERVER_GET_BOUND_PORT_STATUS_NOT_STARTED             = RAAT__SERVER_STATUS_BASE + 0,
    /**< Indicates that this call was made before the server was running */

    RAAT__SERVER_START_STATUS_ALREADY_RUNNING                  = RAAT__SERVER_STATUS_BASE + 1,
    /**< Indicates that a #RAAT__server_start call was made after the server was already running */

    RAAT__SERVER_STATUS_NETWORK_ERROR                          = RAAT__SERVER_STATUS_BASE + 2,
    /**< Indicates that a network error occurred */

    RAAT__SERVER_STOP_STATUS_NOT_RUNNING                       = RAAT__SERVER_STATUS_BASE + 3,
    /**< Indicates that stop was called, but the server was not running */

    RAAT__SERVER_REBIND_STATUS_NOT_RUNNING                       = RAAT__SERVER_STATUS_BASE + 4,
    /**< Indicates that rebind was called, but the server was not running */

};

/** Create and initialize a RAAT server instance.
 *
 * \param alloc the allocator to use, or NULL (See \ref rc_allocator for more information)
 * \param device a non-null #RAAT__Device instance that this server should control
 * \param loop a libuv event loop object that that should be used for performing asynchronous I/O operations
 * \param out_self Out parameter that will receive a pointer to the #RAAT__Server. This will be set to NULL in failure cases.
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__server_new                 (RC__Allocator *alloc, RAAT__Device *device, uv_loop_t *loop, RAAT__Server **out_self);

/** 
 * Set the desired listen port for this RAAT server.
 *
 * Calling this is optional. If a port is not specified, one will be automatically assigned.
 *
 * Note that calls to #RAAT__server_set_port must be made before calling #RAAT__server_start 
 *
 * \param self a non-NULL #RAAT__Server instance 
 * \param port the desired TCP port
 */
RC_API void
RAAT__server_set_port            (RAAT__Server   *self, int port);

/** 
 * Get the actual bound port for the server.
 *
 * This must be called after RAAT__server_start. 
 *
 * \param self a non-NULL #RAAT__Server instance 
 * \param out_port an output parameter that will receive the desired TCP port
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__server_get_bound_port      (RAAT__Server   *self, int *out_port);

/**
 * Start the #RAAT__Server
 *
 * One of the important parts of starting the server is binding the listen port. After starting the server, you 
 * can find out what the actual bound port was using #RAAT__server_get_bound_port.
 *
 * \param self a non-NULL #RAAT__Server instance 
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__server_start               (RAAT__Server   *self);

/**
 * Stop the #RAAT__Server
 *
 * \param self a non-NULL #RAAT__Server instance 
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__server_stop                (RAAT__Server   *self);

/**
 * Rebind the listen socket for the #RAAT__Server
 *
 * \param self a non-NULL #RAAT__Server instance 
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__server_rebind              (RAAT__Server   *self);

/**
 * Delete a RAAT server instance.
 *
 * \param self a #RAAT__Server instance, or NULL.
 */
RC_API void 
RAAT__server_delete             (RAAT__Server   *self);

typedef void (*RAAT__ServerCallback)(RAAT__Server *server, void *userdata);
 
RC_API RC__Status
RAAT__server_post                (RAAT__Server *self, RAAT__ServerCallback cb, void *userdata);

 /** @} */

#ifdef __cplusplus
}
#endif

#endif
