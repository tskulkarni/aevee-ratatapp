//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_SESSION_H
#define INCLUDED_RAAT_SESSION_H

#include "rc_base.h"
#include "rc_allocator.h"
#include "rc_status.h"
#include "raat_fwd.h"
#include "raat_log.h"
#include "raat_info.h"
#include "rc_netutil.h" 
#include "raat_device.h" 

#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RAAT__SESSION_STATUS_INVALID_SCRIPT                  = RAAT__SESSION_STATUS_BASE + 0,
    RAAT__SESSION_STATUS_ALREADY_RUNNING                 = RAAT__SESSION_STATUS_BASE + 1,
    RAAT__SESSION_STATUS_PRECONDITION_NOT_MET            = RAAT__SESSION_STATUS_BASE + 2,
};

typedef struct {
    RC__Allocator *alloc;
    uint32_t       message_type;
    uint32_t       length;
    uint8_t       *data;
} RAAT__SessionMessage;

RC_API RC__Status
RAAT__session_message_new         (RC__Allocator *alloc, uint32_t message_type, uint8_t *data, uint32_t length, bool take_ownership_of_data, RAAT__SessionMessage **out_message);

RC_API void
RAAT__session_message_delete       (RAAT__SessionMessage *message);

typedef struct {
    RC__Allocator *alloc;
    char          *module;
    char          *name;
    char          *data;                // NOTE: not necessarily NULL terminated
    uint32_t       length;
} RAAT__Script;         

RC_API RC__Status
RAAT__script_new                  (RC__Allocator *alloc, const char *name, const char *module, const char *data, uint32_t length, bool take_ownership_of_data, RAAT__Script **out_script);

RC_API void
RAAT__script_delete               (RAAT__Script *script);

RC_API RC__Status
RAAT__session_new                 (RC__Allocator *alloc, RAAT__Device *device, struct sockaddr_storage *addr, RAAT__Session **out_self);

void
RAAT__session_get_remote_addr(RAAT__Session *self, struct sockaddr_storage *out_addr);

bool 
RAAT__session_lua_pcall(RAAT__Session *self, int nargs, int nreturns);

const char *
RAAT__session_get_display_addr(RAAT__Session *self);

typedef void (*RAAT__SessionMessageCallback)(RAAT__Session *session, RAAT__SessionMessage *message, void *userdata);

typedef void (*RAAT__SessionFailureCallback)(RAAT__Session *session, void *userdata);

typedef void (*RAAT__SessionCallback)(RAAT__Session *session, void *userdata);

typedef void (*RAAT__SessionRunScriptCallback)(RAAT__Session *session, RC__Status, const char *error_message, void *userdata);

/**
 * Run a script on the session:
 *
 * NOTE: takes ownership of script and destroys it
 * NOTE: callback is invoked on session's private thread
 */
RC_API RC__Status
RAAT__session_run_script          (RAAT__Session *self, RAAT__Script *script, RAAT__SessionRunScriptCallback cb, void *userdata);

/**
 * Set the onfailure callback.
 *
 * The session will use this to report failures.
 *
 * NOTE: callback is invoked on session's private thread
 */
RC_API RC__Status
RAAT__session_set_failure_callback(RAAT__Session *self, RAAT__SessionFailureCallback cb, void *userdata);

/**
 * Set the onmessage callback.
 *
 * The session will use this to send messages back to the client.
 *
 * The callback must take ownership of message, and is responsible for deleting it.
 * NOTE: callback is invoked on session's private thread
 */
RC_API RC__Status
RAAT__session_set_message_callback(RAAT__Session *self, RAAT__SessionMessageCallback cb, void *userdata);

/** 
 * This is invoked when a message is recieved and needs to be processed on this session.
 *
 * This method is thread-safe: the session will post to its personal eventloop in order to do the work.
 *
 * This method takes ownership of message.
 */
RC_API RC__Status
RAAT__session_process_message     (RAAT__Session *self, RAAT__SessionMessage *message);

RC_API RC__Status
RAAT__session_start               (RAAT__Session *self);

RC_API RC__Status
RAAT__session_post                (RAAT__Session *self, RAAT__SessionCallback cb, void *userdata);

RC_API void 
RAAT__session_delete             (RAAT__Session   *self);

/** \internal */
void RAAT__session_send_message(RAAT__Session *self, RAAT__SessionMessage *message);

/** \internal */ 
RC_API void
RAAT__session_set_client_type(RAAT__Session *self, const char *client_type);

#ifdef __cplusplus
}
#endif

#endif
