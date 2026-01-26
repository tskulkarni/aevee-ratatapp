//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_DISCOVERY_H
#define INCLUDED_RAAT_DISCOVERY_H

#include "rc_base.h"
#include "rc_allocator.h"
#include "rc_status.h"
#include "rc_string.h"
#include "raat_log.h"

#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup raat_discovery Discovery
 *
 *  \brief
 *  This module implements the discovery protocol
 *
 *  @{
 */

/** The opaque structure definition for the RAAT__Discovery type
 */
typedef struct RAAT__Discovery_s        RAAT__Discovery;

/** The opaque structure definition for the RAAT__DiscoveryMessage type
 */
typedef struct RAAT__DiscoveryMessage_s RAAT__DiscoveryMessage;

enum {
    RAAT__DISCOVERY_START_STATUS_ALREADY_RUNNING             = RAAT__DISCOVERY_STATUS_BASE + 0,
    /**< Indicates that this call was made before the discovery module was running */

    RAAT__DISCOVERY_STOP_STATUS_NOT_RUNNING                  = RAAT__DISCOVERY_STATUS_BASE + 1,
    /**< Indicates that this call was made before the discovery module was running */

    RAAT__DISCOVERY_STATUS_NETWORK_ERROR                     = RAAT__DISCOVERY_STATUS_BASE + 2,
    /**< Indicates that there was a network error while initializing or starting discovery */

    RAAT__DISCOVERY_STATUS_PACKET_TOO_LARGE                  = RAAT__DISCOVERY_STATUS_BASE + 3,
    /**< Indicates that there was a network error while initializing or starting discovery */

    RAAT__DISCOVERY_STATUS_INVALID_KEY_OR_VALUE              = RAAT__DISCOVERY_STATUS_BASE + 4,
    /**< Indicates that there was a network error while initializing or starting discovery */
};

/** 
 * Callback type for discovery message handlers.
 *
 * /sa #RAAT__discovery_add_message_callback
 * /sa #RAAT__discovery_remove_message_callback
 * /sa #RAAT__discovery_respond
 *
 */
typedef void (*RAAT__DiscoveryMessageCallback)(RAAT__Discovery *discovery, RAAT__DiscoveryMessage *message, void *userdata);

/**
 * Create a new query message
 */
RC_API RC__Status
RAAT__discovery_message_new(RC__Allocator *alloc, RAAT__DiscoveryMessage **out_self);

/**
 * Create a new response message based on a recieved query message.
 */
RC_API RC__Status
RAAT__discovery_message_new_response(RC__Allocator *alloc, RAAT__DiscoveryMessage *query, RAAT__DiscoveryMessage **out_self);

/**
 * Get a key/value pair in a discovery message
 */
RC_API RC__Status
RAAT__discovery_message_set(RAAT__DiscoveryMessage *msg, const char *key, const char *val);

/**
 * Get a field from a discovery message
 */
RC_API const char *
RAAT__discovery_message_get(RAAT__DiscoveryMessage *msg, const char *key);

/**
 * Get the source address for a query message
 */
RC_API struct sockaddr_storage *
RAAT__discovery_message_get_source_addr(RAAT__DiscoveryMessage *msg);

/**
 * Get the destination address for a response message
 */
RC_API struct sockaddr_storage *
RAAT__discovery_message_get_dest_addr(RAAT__DiscoveryMessage *msg);

/**
 * Append a human-readable depiction of a discovery message to an #RC__String
 */
RC_API void         
RAAT__discovery_message_append_to_string(RAAT__DiscoveryMessage *msg, RC__String *string);

/**
 * Release resources associated with a discovery message
 */
RC_API void
RAAT__discovery_message_delete(RAAT__DiscoveryMessage *self);

/** Create and initialize a RAAT discovery instance.
 *
 * \param alloc the allocator to use, or NULL (See \ref rc_allocator for more information)
 * \param log a non-null #RAAT__Log instance that should be used when logging from this instance
 * \param loop a libuv event loop object that that should be used for performing asynchronous I/O operations
 * \param out_self Out parameter that will receive a pointer to the #RAAT__Discovery. This will be set to NULL in failure cases.
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__discovery_new                 (RC__Allocator *alloc, RAAT__Log *log, uv_loop_t *loop, RAAT__Discovery **out_self);

/**
 * Delete a RAAT discovery instance.
 *
 * \param self a #RAAT__Discovery instance, or NULL.
 */
RC_API void 
RAAT__discovery_delete            (RAAT__Discovery   *self);

/**
 * Start the #RAAT__Discovery
 *
 * \param self a non-NULL #RAAT__Discovery instance 
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__discovery_start               (RAAT__Discovery   *self);

/**
 * Stop the #RAAT__Discovery
 *
 * \param self a non-NULL #RAAT__Discovery instance 
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__discovery_stop                (RAAT__Discovery   *self);

/**
 * Respond to a discovery message
 *
 * This is used in conjunction with an instance of #RAAT__DiscoveryMessage created with #RAAT__discovery_message_new_response
 */
RC_API RC__Status 
RAAT__discovery_respond                         (RAAT__Discovery               *self, 
                                                 RAAT__DiscoveryMessage        *response);

/**
 * Send a message 
 *
 * This is used in conjunction with an instance of #RAAT__DiscoveryMessage created with #RAAT__discovery_message_new
 */
RC_API RC__Status 
RAAT__discovery_broadcast                       (RAAT__Discovery               *self, 
                                                 RAAT__DiscoveryMessage        *message);

/**
 * Send a message and expect a response
 *
 * This is used in conjunction with an instance of #RAAT__DiscoveryMessage created with #RAAT__discovery_message_new
 */
RC_API RC__Status 
RAAT__discovery_query                           (RAAT__Discovery               *self, 
                                                 RAAT__DiscoveryMessage        *response,
                                                 RAAT__DiscoveryMessageCallback cb,
                                                 void *userdata);

/**
 * Register a callback that is invoked whenever a message is recieved
 */
RC_API RC__Status
RAAT__discovery_add_message_callback            (RAAT__Discovery                *self, 
                                                 RAAT__DiscoveryMessageCallback  cb, 
                                                 void                           *userdata);

/**
 * Un-register a callback that is invoked whenever a message is recieved.
 *
 * If multiple callbacks have been registered with the same cb/userdata, the oldest one will be removed
 *
 * \retval true if the callback was removed, false otherwise
 */
RC_API bool
RAAT__discovery_remove_message_callback         (RAAT__Discovery                *self, 
                                                 RAAT__DiscoveryMessageCallback  cb, 
                                                 void                           *userdata);

 /** @} */

#ifdef __cplusplus
}
#endif

#endif
