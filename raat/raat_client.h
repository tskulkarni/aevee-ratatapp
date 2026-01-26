//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//

#ifndef INCLUDED_RAAT_CLIENT_H
#define INCLUDED_RAAT_CLIENT_H

#include "rc_base.h"
#include "rc_allocator.h"
#include "rc_status.h"
#include "raat_log.h"

#include <uv.h>
#include <jansson.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup raat_client Client
 *
 *  \brief
 *  This module implements a TCP client that speaks the RAAT-LL protocol.
 *
 *  This is provided primarily for debugging, and to drive the raatool command line tool. It
 *  is not used in a production environment.
 *
 *  @{
 */

/** The opaque structure definition for the RAAT__Client type
 */
typedef struct RAAT__Client_s RAAT__Client; 

enum {
    RAAT__CLIENT_STATUS_NETWORK_ERROR                    = RAAT__CLIENT_STATUS_BASE + 0,
    /**< Indicates that a network error occurred */

    RAAT__CLIENT_STATUS_INVALID_STATE                    = RAAT__CLIENT_STATUS_BASE + 1,
    /**< Indicates that this call was made when the client was in the wrong state */

    RAAT__CLIENT_STATUS_CANCELED                         = RAAT__CLIENT_STATUS_BASE + 2,
    /**< Indicates that an operation was canceled */
};

/** This enumeration indicates the current state of a \ref RAAT__Client */
typedef enum {
    RAAT__CLIENT_DISCONNECTED           = 0,
    /**< The client is disconnected */
    RAAT__CLIENT_CONNECTING             = 1,
    /**< The client is connected */
    RAAT__CLIENT_CONNECTED              = 2,
    /**< The client is connecting */
} RAAT__ClientState;

/** Callback function prototype used by the client to report the result of a \ref RAAT__client_connect operation */
typedef void (*RAAT__ClientConnectCallback)(RAAT__Client *client, RC__Status status, int uvrc, void *userdata);

/** Callback function prototype for message responses. Used by \ref RAAT__client_request  */
typedef void (*RAAT__ClientResponseCallback)(RAAT__Client *client, uint8_t flags, json_t *response, void *userdata);

/** Callback function prototype used by the client to report disconnection */
typedef void (*RAAT__ClientDisconnectedCallback)(RAAT__Client *client, void *userdata);

/** Callback function prototype used by the client to report incoming messages */
typedef void (*RAAT__ClientMessageCallback)(RAAT__Client *client, uint32_t msgtype, uint8_t *body, uint32_t bodylen, void *userdata);

/** Create and initialize a RAAT client instance.
 *
 * \param alloc the allocator to use, or NULL (See \ref rc_allocator for more information)
 * \param log a non-null #RAAT__Log instance that should be used when logging from this instance
 * \param loop a libuv event loop that that should be used for performing asynchronous I/O operations
 * \param out_self Out parameter that will receive a pointer to the #RAAT__Client. This will be set to NULL in failure cases.
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__client_new                 (RC__Allocator *alloc, RAAT__Log *log, uv_loop_t *loop, RAAT__Client **out_self);

/** 
 * Get the current state of the client
 *
 * \retval The return status. (See \ref RAAT__ClientState for more information)
 */
RC_API RAAT__ClientState
RAAT__client_get_state           (RAAT__Client *self);

/**
 * Connect
 *
 * \param self a #RAAT__Client instance, or NULL.
 * \param addr the socket address to connect to
 * \param cb the connect callback to register
 * \param userdata a user-provided opaque pointer that will be provided to to the connect callback
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__client_connect             (RAAT__Client   *self, const struct sockaddr * addr, RAAT__ClientConnectCallback cb, void *userdata);

/**
 * Send Request + register callback for response(s)
 *
 * \param self a #RAAT__Client instance, or NULL.
 * \param json a JSON RAAT-LL message to send.
 * \param cb the response callback to register
 * \param userdata a user-provided opaque pointer that will be provided to the response callback
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__client_request             (RAAT__Client   *self, const json_t *json, RAAT__ClientResponseCallback cb, void *userdata);

/**
 * Send a fire-and-forget request
 *
 * \param self a #RAAT__Client instance, or NULL.
 * \param json a JSON RAAT-LL message to send.
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__client_request_forget      (RAAT__Client   *self, const json_t *json);

/**
 * Set disconnected callback.
 *
 * This callback will be invoked when a previously successful connection is lost
 *
 * \param self a #RAAT__Client instance, or NULL.
 * \param cb the disconnected callback to register, or NULL to clear the disconnected callback
 * \param userdata a user-provided opaque pointer that will be provided to the disconnected callback
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API void
RAAT__client_set_disconnected_callback (RAAT__Client   *self, RAAT__ClientDisconnectedCallback cb, void *userdata);

/**
 * Set the message callback. 
 *
 * This callback will receive high-level RAAT messages.
 *
 * \param self a #RAAT__Client instance, or NULL.
 * \param cb the message callback to register, or NULL to clear the message callback
 * \param userdata a user-provided opaque pointer that will be provided to the message callback
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API void
RAAT__client_set_message_callback      (RAAT__Client   *self, RAAT__ClientMessageCallback cb, void *userdata);

/**
 * Delete a RAAT client instance.
 *
 * \param self a #RAAT__Client instance, or NULL.
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API void 
RAAT__client_delete             (RAAT__Client   *self);

 /** @} */

#ifdef __cplusplus
}
#endif

#endif
