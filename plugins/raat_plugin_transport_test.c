//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_plugin_transport_test.h"
#include "rc_list.h"

#include <uv.h>

#define RAAT__CURRENT_LOG self->log

/*
 * Transport Plugin
 */
typedef struct {
    RAAT__TransportPlugin            plugin;          // must be first item in struct
    RC__Allocator                   *alloc;
    RAAT__Log                       *log;
    RAAT__TransportControlListeners  control_listeners;
    uv_mutex_t                       lock;
    json_t                          *info;
} TestTransportPlugin;


static RC__Status transport_add_control_listener(void *vself, RAAT__TransportControlCallback cb, void *userdata) {
    TestTransportPlugin *self = (TestTransportPlugin*)vself;
    return RAAT__transport_control_listeners_add(&self->control_listeners, cb, userdata);
}

static RC__Status transport_remove_control_listener(void *vself, RAAT__TransportControlCallback cb, void *userdata) {
    TestTransportPlugin *self = (TestTransportPlugin*)vself;
    return RAAT__transport_control_listeners_remove(&self->control_listeners, cb, userdata);
}

static RC__Status transport_get_info(void *vself, json_t **out_info) {
    TestTransportPlugin *self = (TestTransportPlugin*)vself;

    RC__ASSERT(self);
    RC__ASSERT(out_info);

    json_incref(self->info);
    *out_info = self->info;

    return RC__STATUS_SUCCESS;
}

static RC__Status transport_update_status(void *vself, json_t *metadata) {
    TestTransportPlugin *self = (TestTransportPlugin*)vself;
    RAAT__TRACE("[transport/test] got update status");
    return RC__STATUS_SUCCESS;
}

static RC__Status transport_update_artwork(void *vself, const char *mime_type, void *data, size_t data_len) {
    TestTransportPlugin *self = (TestTransportPlugin*)vself;
    RAAT__TRACE("[transport/test] got update artwork");
    return RC__STATUS_SUCCESS;
}

RC__Status 
RAAT__test_transport_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__TransportPlugin **out_transport) { 
    alloc = RC__allocator_default(alloc);
    TestTransportPlugin *self            = RC__new0(alloc, TestTransportPlugin, 1);
    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;
    self->alloc                          = alloc;
    self->log                            = RAAT__device_get_log(device);
    self->plugin.get_info                = transport_get_info;
    self->plugin.add_control_listener    = transport_add_control_listener;
    self->plugin.remove_control_listener = transport_remove_control_listener;
    self->plugin.update_status           = transport_update_status;
    self->plugin.update_artwork          = transport_update_artwork;

    uv_mutex_init(&self->lock);
    RAAT__transport_control_listeners_init(&self->control_listeners, self->alloc);

    self->info = json_object();
    json_object_set(self->info, "config", config);

    RAAT__TRACE("[transport/test] initialized");

    *out_transport = &self->plugin;
    return RC__STATUS_SUCCESS;
}

void
RAAT__test_transport_plugin_delete(RAAT__TransportPlugin *transport) {
    TestTransportPlugin *self = (TestTransportPlugin*)transport;
    uv_mutex_destroy(&self->lock);
    if (self->info) json_decref(self->info);
    RAAT__transport_control_listeners_destroy(&self->control_listeners);
    RC__free(self->alloc, self);
}

