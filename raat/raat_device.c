//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_device.h"
#include "raat_server.h"
#include "raat_log.h"
#include "raat_discovery.h"
#include "raat_info.h"
#include "raat_base.h"
#include "rc_netutil.h"

#include "rc_guid.h"

#include <uv.h>

#include <string.h>
#include <stdlib.h>

#define RAAT__CURRENT_LOG (self->log)

/** \private */
struct RAAT__Device_s {
    RC__Allocator               *alloc;
    uv_mutex_t                   lock;

    RAAT__Log                   *log;
    RAAT__Discovery             *discovery;
    RAAT__Server                *server;
    RAAT__Info                  *info;

    RC__NetworkStatus           *networkstatus;
 
    bool                         is_running;
    uv_loop_t                    loop;

    bool                         disable_discovery;
    RAAT__OutputPlugin          *output;
    RAAT__VolumePlugin          *volume;
    RAAT__TransportPlugin       *transport;
    RAAT__SourceSelectionPlugin *source_selection;

    RC__List                     client_types;  /* ClientType* */
    RAAT__ClientTypesCallback    client_types_cb;
    void                        *client_types_userdata;
};

typedef struct {
    char *name;
    int   count;
} ClientType;

static void message_cb(RAAT__Discovery *discovery, RAAT__DiscoveryMessage *message, void *userdata);

RC_API RC__Status    
RAAT__device_new                (RC__Allocator *alloc, RAAT__Log *log, RAAT__Device **out_self) {
    RAAT__static_init();

    RC__Status status;   
    RAAT__Device *self;
    bool inited_loop = false;
    int rc;

    RC__ASSERT(out_self != NULL);
    *out_self = NULL;

    self = RC__new0(alloc, RAAT__Device, 1);
    if (self == NULL) {
        return RC__STATUS_OUT_OF_MEMORY;
    }

    self->alloc = RC__allocator_default(alloc);

    rc = uv_loop_init(&self->loop);
    if (rc) {
        status = RC__STATUS_UNEXPECTED_ERROR;
        goto fail;
    }
    inited_loop = true;

    self->log = log;

    status = RAAT__info_new(alloc, self->log, &self->info);
    if (!RC__STATUS_IS_SUCCESS(status)) { goto fail; }

    status = RAAT__discovery_new(alloc, self->log, &self->loop, &self->discovery);
    if (!RC__STATUS_IS_SUCCESS(status)) { goto fail; }

    status = RAAT__server_new(alloc, self, &self->loop, &self->server);
    if (!RC__STATUS_IS_SUCCESS(status)) { goto fail; }

    status = RAAT__discovery_add_message_callback(self->discovery, message_cb, self);
    if (!RC__STATUS_IS_SUCCESS(status)) { goto fail; }

    RC__list_init(&self->client_types, self->alloc);

    uv_mutex_init(&self->lock);

    *out_self = self;

    return RC__STATUS_SUCCESS;

fail:
    if (self->info)        RAAT__info_delete(self->info);
    if (self->discovery)   RAAT__discovery_delete(self->discovery);
    if (self->server)      RAAT__server_delete(self->server);
    if (inited_loop)       uv_loop_close(&self->loop);
    RC__free(alloc, self);
    return status;
}

static void info_foreach_cb(const char *key, const char *val, void *userdata) {
    RAAT__DiscoveryMessage *message = userdata;
    RAAT__discovery_message_set(message, key, val);
}

static void message_cb(RAAT__Discovery *discovery, RAAT__DiscoveryMessage *message, void *userdata) {
    RAAT__Device *self = (RAAT__Device*)userdata;
    const char *service_id;
    RC__Guid guid;
    RC__Guid raat_service_guid;
    RAAT__DiscoveryMessage *response;
    RC__Status status;
    int port;
    char portstr[20];

    /*
    {
        char buf[RC__MAX_ADDR_LEN];
        RC__String string;
        RC__string_init(&string, self->alloc);
        RAAT__discovery_message_append_to_string(message, &string);
        RC__sockaddr_to_string(RAAT__discovery_message_get_source_addr(message), buf);
        RAAT__TRACE("[device] got discovery message from %s: %s", string.str);
        RC__string_destroy(&string);
    }
    */

    if (RC__guid_init_string(&raat_service_guid, RAAT__SERVICE_GUID_STRING)) {
        RAAT__CRITICAL("[device] I can't parse my own service guid");
        return;
    }

    service_id = RAAT__discovery_message_get(message, "service_id");    
    if (service_id == NULL || 0 != RC__guid_init_string(&guid, service_id)) return;        // ignore devices without a parseable service id

    if (!RC__guid_equals(&guid, &raat_service_guid)) return;    // ignore queries 

    status = RAAT__discovery_message_new_response(self->alloc, message, &response);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        RAAT__WARNING("[device] failed to create discovery response: %s", RC__status_to_string(status));
        return;
    }

    status = RAAT__server_get_bound_port(self->server, &port);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        RAAT__WARNING("[device] failed to get bound port: %s", RC__status_to_string(status));
        return;
    }

    // populate fields
    RAAT__discovery_message_set(response, "service_id", RAAT__SERVICE_GUID_STRING);
    snprintf(portstr, 20, "%d", port);
    RAAT__discovery_message_set(response, "tcp_port", portstr);
    RAAT__info_foreach(self->info, info_foreach_cb, response);

    //RAAT__INFO("[device] responding to discovery query");
    RAAT__discovery_respond(self->discovery, response);

    RAAT__discovery_message_delete(response);
}

static RC__Status announce(RAAT__Device *self) {
    RC__Status status;
    RAAT__DiscoveryMessage *message;
    int port;
    char portstr[20];


    status = RAAT__discovery_message_new(self->alloc, &message);
    if (!RC__STATUS_IS_SUCCESS(status)) return status;

    status = RAAT__server_get_bound_port(self->server, &port);
    if (!RC__STATUS_IS_SUCCESS(status)) return status;

    // populate fields
    RAAT__discovery_message_set(message, "service_id", RAAT__SERVICE_GUID_STRING);
    snprintf(portstr, 20, "%d", port);
    RAAT__discovery_message_set(message, "tcp_port", portstr);
    RAAT__info_foreach(self->info, info_foreach_cb, message);

    {
        RC__String string;
        RC__string_init(&string, self->alloc);
        RAAT__discovery_message_append_to_string(message, &string);
        RAAT__TRACE("[device] announcing %s", string.str);
        RC__string_destroy(&string);
    }

    status = RAAT__discovery_broadcast(self->discovery, message);
    RAAT__discovery_message_delete(message);

    return status;
}

static void rebind_cb(RAAT__Server *server, void *ud) {
    RAAT__Device *self   = ud;
    RAAT__TRACE("self %p server %p", self, self->server);
    RAAT__server_rebind(self->server);
}

RC__Status
RAAT__device_rebind             (RAAT__Device *self) {
    if (self->server) {
        RAAT__server_post(self->server, rebind_cb, self);
    }
}

RC_API void
RAAT__device_disable_discovery   (RAAT__Device   *self) {
    RC__ASSERT(self != NULL);

    self->disable_discovery = true;
}

RC_API RC__Status  
RAAT__device_run                 (RAAT__Device *self) {
    RC__Status status = RAAT__device_run_phase0(self);
    if (!RC__STATUS_IS_SUCCESS(status)) return status;
    return RAAT__device_run_phase1(self);
}

static void 
networkstatus_cb(void *vself) {
    RAAT__Device *self = vself;
    if (self->is_running) {
        RC__Status status;

        RAAT__TRACE("Network Status Changed. Refreshing Discovery");

        RAAT__discovery_stop(self->discovery);
        RAAT__discovery_start(self->discovery);
        status = announce(self);
        if (!RC__STATUS_IS_SUCCESS(status)) {
            RAAT__WARNING("[device] announce failed: %s", RC__status_to_string(status));
        }
    }
}

RC_API RC__Status  
RAAT__device_run_phase0          (RAAT__Device *self) {
    RC__Status status;
    int bound_port;

    RC__ASSERT(self != NULL);

    uv_mutex_lock(&self->lock);
    if (self->is_running) {
        uv_mutex_unlock(&self->lock);
        return RAAT__DEVICE_RUN_STATUS_ALREADY_RUNNING;
    }

    status = RAAT__server_start(self->server);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        uv_mutex_unlock(&self->lock);
        return status;
    }

    status = RAAT__server_get_bound_port(self->server, &bound_port);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        RAAT__server_stop(self->server);
        uv_mutex_unlock(&self->lock);
        return status;
    }

    if (!self->disable_discovery) {
        status = RAAT__discovery_start(self->discovery);
        if (!RC__STATUS_IS_SUCCESS(status)) {
            RAAT__server_stop(self->server);
            uv_mutex_unlock(&self->lock);
            return status;
        }

        status = announce(self);
        if (!RC__STATUS_IS_SUCCESS(status)) {
            RAAT__WARNING("[device] announce failed: %s", RC__status_to_string(status));
        }

        if (!self->networkstatus) {
            self->networkstatus = RC__networkstatus_begin_watch(self->alloc, &self->loop, networkstatus_cb, self);
        }
    }

    self->is_running = true;
    uv_mutex_unlock(&self->lock);

    return status;
}

RC_API RC__Status  
RAAT__device_run_phase1          (RAAT__Device *self) {
    int rc;
    rc = uv_run(&self->loop, UV_RUN_DEFAULT);
    if (rc) {
        RAAT__ERROR("[device] failed to start main uv loop: %d", uv_strerror(rc));
        return RC__STATUS_UNEXPECTED_ERROR;
    } else {
        RAAT__TRACE("[device] run exited successfully");
    }

    return RC__STATUS_SUCCESS;
}


typedef struct {
    RAAT__Device         *self;
    uv_async_t            async;
} StopCallbackState;

static void stop_close_cb(uv_handle_t *handle) {
    StopCallbackState *state  = handle->data;
    RAAT__Device      *self   = state->self;
    RC__free(self->alloc, state);
}

static void stop_callback_async_cb(uv_async_t *async) {
    StopCallbackState *state  = async->data;
    RAAT__Device      *self   = state->self;

    if (!self->disable_discovery) {
        RAAT__TRACE("[device] stopping discovery");
        RAAT__discovery_stop(self->discovery);
    }
    RAAT__TRACE("[device] stopping server");
    RAAT__server_stop(self->server);

    uv_close((uv_handle_t*)async, stop_close_cb);
}

RC__Status
RAAT__device_stop               (RAAT__Device *self) {
    RC__ASSERT(self != NULL);

    uv_mutex_lock(&self->lock);

    if (!self->is_running) {
        uv_mutex_unlock(&self->lock);
        return RAAT__DEVICE_STOP_STATUS_NOT_RUNNING;
    }

    self->is_running = false;

    if (self->networkstatus) {
        RC__networkstatus_end_watch(self->networkstatus);
        self->networkstatus = NULL;
    }

    StopCallbackState *state = RC__new0(self->alloc, StopCallbackState, 1);
    uv_async_init(&self->loop, &state->async, stop_callback_async_cb);
    state->self       = self;
    state->async.data = state;
    uv_async_send(&state->async);

    uv_mutex_unlock(&self->lock);
    
    return RC__STATUS_SUCCESS;
}

RC_API RAAT__Log *
RAAT__device_get_log             (RAAT__Device   *self) {
    RC__ASSERT(self != NULL);
    return self->log;
}

RC_API RAAT__Server *
RAAT__device_get_server          (RAAT__Device   *self) {
    RC__ASSERT(self != NULL);
    return self->server;
}

RC_API RAAT__Info *
RAAT__device_get_info            (RAAT__Device   *self) {
    RC__ASSERT(self != NULL);
    return self->info;
}

RC_API RAAT__Discovery *
RAAT__device_get_discovery            (RAAT__Device   *self) {
    RC__ASSERT(self != NULL);
    return self->discovery;
}

RC_API void
RAAT__device_set_source_selection_plugin   (RAAT__Device *self, RAAT__SourceSelectionPlugin *source_selection) {
    RC__ASSERT(self != NULL);
    uv_mutex_lock(&self->lock);
    self->source_selection = source_selection;
    uv_mutex_unlock(&self->lock);
}


RC_API void
RAAT__device_set_transport_plugin   (RAAT__Device *self, RAAT__TransportPlugin *transport) {
    RC__ASSERT(self != NULL);
    uv_mutex_lock(&self->lock);
    self->transport = transport;
    uv_mutex_unlock(&self->lock);
}

RC_API void
RAAT__device_set_output_plugin   (RAAT__Device *self, RAAT__OutputPlugin *output) {
    RC__ASSERT(self != NULL);
    uv_mutex_lock(&self->lock);
    self->output = output;
    uv_mutex_unlock(&self->lock);
}

RC_API void
RAAT__device_set_volume_plugin   (RAAT__Device *self, RAAT__VolumePlugin *volume) {
    RC__ASSERT(self != NULL);
    uv_mutex_lock(&self->lock);
    self->volume = volume;
    uv_mutex_unlock(&self->lock);
}

RC_API RAAT__OutputPlugin *
RAAT__device_get_output_plugin   (RAAT__Device *self) {
    RAAT__OutputPlugin *output;

    RC__ASSERT(self != NULL);

    uv_mutex_lock(&self->lock);
    output = self->output;
    uv_mutex_unlock(&self->lock);

    return output;
}

RC_API RAAT__VolumePlugin *
RAAT__device_get_volume_plugin   (RAAT__Device *self) {
    RAAT__VolumePlugin *volume;

    RC__ASSERT(self != NULL);

    uv_mutex_lock(&self->lock);
    volume = self->volume;
    uv_mutex_unlock(&self->lock);

    return volume;
}

RC_API RAAT__TransportPlugin *
RAAT__device_get_transport_plugin   (RAAT__Device *self) {
    RAAT__TransportPlugin *transport;

    RC__ASSERT(self != NULL);

    uv_mutex_lock(&self->lock);
    transport = self->transport;
    uv_mutex_unlock(&self->lock);

    return transport;
}
RC_API RAAT__SourceSelectionPlugin *
RAAT__device_get_source_selection_plugin   (RAAT__Device *self) {
    RAAT__SourceSelectionPlugin *source_selection;

    RC__ASSERT(self != NULL);

    uv_mutex_lock(&self->lock);
    source_selection = self->source_selection;
    uv_mutex_unlock(&self->lock);

    return source_selection;
}

static void walk_handle(uv_handle_t *handle, void *vself) {
    RAAT__Device *self = vself;
    RAAT__DEBUG("[device]    live handle %p type=%d", handle, handle->type);
}

static void destroy_client_type_cb(void *data, void *userdata) {
    ClientType    *ct    = data;
    RC__Allocator *alloc = userdata;
    RC__free(alloc, ct->name);
    RC__free(alloc, ct);
}

RC_API void 
RAAT__device_delete            (RAAT__Device   *self) {
    int rc;
    if (!self) return;
    RAAT__TRACE("deleting device");

    do {
        rc = uv_loop_close(&self->loop);
        if (!rc) break;
        if (rc != UV_EBUSY) {
            RAAT__ERROR("[device] failed to close uv loop: %s", uv_strerror(rc));
            break;
        }
        RC__usleep(100000);
        RAAT__TRACE("[device] retrying uv_loop_close because of UV_EBUSY");
        uv_walk(&self->loop, walk_handle, self);
    } while (rc == UV_EBUSY);

    RAAT__info_delete(self->info);
    RAAT__discovery_delete(self->discovery);
    RAAT__TRACE("deleting server");
    RAAT__server_delete(self->server);

    RC__list_foreach_destroy(&self->client_types, destroy_client_type_cb, self->alloc);

    uv_mutex_destroy(&self->lock);
    RC__free(self->alloc, self);
}

const char * RAAT__device_status_to_string(RC__Status status) {
    switch (status) {
        case RAAT__DEVICE_RUN_STATUS_ALREADY_RUNNING:      return "RAAT__DEVICE_RUN_STATUS_ALREADY_RUNNING";             
        case RAAT__DEVICE_RUN_STATUS_PRECONDITION_NOT_MET: return "RAAT__DEVICE_RUN_STATUS_PRECONDITION_NOT_MET";       
        case RAAT__DEVICE_STOP_STATUS_NOT_RUNNING:         return "RAAT__DEVICE_STOP_STATUS_NOT_RUNNING";              
        default: return NULL;
    }
}

void RAAT__device_notify_client_type(RAAT__Device *self, const char *client_type, bool connected) {
    int n_client_types;
    char **names;
    int i;

    uv_mutex_lock(&self->lock);
    RAAT__ClientTypesCallback  cb       = self->client_types_cb;
    void                      *userdata = self->client_types_userdata;

    RC__ListIter it;
    bool found   = false;
    bool changed = false;
    RC__ListIter next;
    for (it = RC__list_begin(&self->client_types); it != RC__list_end(&self->client_types); it = next) {
        ClientType *ct = (ClientType*)RC__listiter_data(it);
        next = RC__listiter_next(it);

        if (!strcmp(ct->name, client_type)) {
            if (connected) {
                if (ct->count++ == 0) changed = true;
            } else {
                if (--ct->count == 0) {
                    changed = true;
                    RC__free(self->alloc, ct->name);
                    RC__free(self->alloc, ct);
                    RC__list_remove(&self->client_types, it);
                }
            }
            found = true;
            break;
        }
    }

    if (!found) {
        ClientType *ct = RC__new0(self->alloc, ClientType, 1);
        ct->name = RC__allocator_strdup(self->alloc, client_type);
        ct->count = 1;
        RC__list_push(&self->client_types, ct);
        changed = true;
    }

    if (!cb) changed = false;

    if (changed) {
        n_client_types = RC__list_length(&self->client_types);
        names = RC__new0(self->alloc, char*, n_client_types);
        i = 0;
        for (it = RC__list_begin(&self->client_types); it != RC__list_end(&self->client_types); it = RC__listiter_next(it)) {
            ClientType *ct = (ClientType*)RC__listiter_data(it);
            names[i++] = RC__allocator_strdup(self->alloc, ct->name);
        }
    }

    uv_mutex_unlock(&self->lock);

    if (changed) {
        cb(userdata, n_client_types, names);
        for (i = 0; i < n_client_types; i++) 
            RC__free(self->alloc, names[i]);
        RC__free(self->alloc, names);
    }
}

RC__Status
RAAT__device_set_client_types_callback    (RAAT__Device              *self, 
                                           RAAT__ClientTypesCallback  cb,
                                           void                      *userdata) {
    uv_mutex_lock(&self->lock);
    self->client_types_cb       = cb;
    self->client_types_userdata = userdata;
    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}
