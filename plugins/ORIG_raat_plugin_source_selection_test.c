//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_plugin_source_selection_test.h"
#include "rc_list.h"

#include <uv.h>

#define RAAT__CURRENT_LOG self->log

/*
 * SourceSelection Plugin
 */
typedef struct {
    RAAT__SourceSelectionPlugin          plugin;          // must be first item in struct
    uv_mutex_t                           lock;
    RC__Allocator                       *alloc;
    RAAT__SourceSelectionStateListeners  state_listeners;
    RAAT__Log                           *log;
    RAAT__SourceSelectionStatus          status;
    RAAT__OutputPlugin                  *output;
    json_t                              *info;
} TestSourceSelectionPlugin;

static RC__Status source_selection_add_state_listener(void *vself, RAAT__SourceSelectionStateCallback cb, void *userdata) {
    TestSourceSelectionPlugin *self = (TestSourceSelectionPlugin*)vself;
    return RAAT__source_selection_state_listeners_add(&self->state_listeners, cb, userdata);
}

static RC__Status source_selection_remove_state_listener(void *vself, RAAT__SourceSelectionStateCallback cb, void *userdata) {
    TestSourceSelectionPlugin *self = (TestSourceSelectionPlugin*)vself;
    return RAAT__source_selection_state_listeners_remove(&self->state_listeners, cb, userdata);
}

static RC__Status source_selection_get_info(void *vself, json_t **out_info) {
    TestSourceSelectionPlugin *self = (TestSourceSelectionPlugin*)vself;

    RC__ASSERT(self);
    RC__ASSERT(out_info);

    json_incref(self->info);
    *out_info = self->info;

    return RC__STATUS_SUCCESS;
}

static RC__Status source_selection_get_state(void *vself, RAAT__SourceSelectionState *out_state) {
    TestSourceSelectionPlugin *self = (TestSourceSelectionPlugin*)vself;

    RC__ASSERT(self != NULL);
    RC__ASSERT(out_state != NULL);

    out_state->status   = self->status;

    return RC__STATUS_SUCCESS;
}

static void source_selection_request_source(void *vself, RAAT__SourceSelectionRequestSourceCallback cb, void *cb_userdata) {
    TestSourceSelectionPlugin *self = (TestSourceSelectionPlugin*)vself;

    RAAT__TRACE("[source_selection/test] requesting source");
    RC__ASSERT(self);

    RAAT__TRACE("[source_selection/test] source acquired");

    self->status = RAAT__SOURCE_SELECTION_STATUS_SELECTED; 
    RAAT__SourceSelectionState state = {0,};
    state.status = self->status;
    RAAT__source_selection_state_listeners_invoke(&self->state_listeners, &state);

    cb(cb_userdata, RC__STATUS_SUCCESS, NULL);
}

static void source_selection_request_standby(void *vself, RAAT__SourceSelectionRequestSourceCallback cb, void *cb_userdata) {
    TestSourceSelectionPlugin *self = (TestSourceSelectionPlugin*)vself;

    RAAT__TRACE("[source_selection/test] requesting standby");
    RC__ASSERT(self);

    RAAT__TRACE("[source_selection/test] in standby");

    self->status = RAAT__SOURCE_SELECTION_STATUS_STANDBY; 
    RAAT__SourceSelectionState state = {0,};
    state.status = self->status;
    RAAT__source_selection_state_listeners_invoke(&self->state_listeners, &state);

    cb(cb_userdata, RC__STATUS_SUCCESS, NULL);
}

RC__Status 
RAAT__test_source_selection_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__SourceSelectionPlugin **out_source_selection) { 
    alloc = RC__allocator_default(alloc);
    TestSourceSelectionPlugin *self            = RC__new0(alloc, TestSourceSelectionPlugin, 1);
    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;
    self->alloc                        = alloc;
    self->log                          = RAAT__device_get_log(device);
    self->plugin.get_info              = source_selection_get_info;
    self->plugin.add_state_listener    = source_selection_add_state_listener;
    self->plugin.remove_state_listener = source_selection_remove_state_listener;
    self->plugin.get_state             = source_selection_get_state;
    self->plugin.request_source        = source_selection_request_source;
    self->plugin.request_standby       = source_selection_request_standby;
    self->status                       = RAAT__SOURCE_SELECTION_STATUS_STANDBY;
    self->output                       = RAAT__device_get_output_plugin(device);

    uv_mutex_init(&self->lock);
    RAAT__source_selection_state_listeners_init(&self->state_listeners, self->alloc);

    self->info = json_object();
    json_object_set(self->info, "config", config);

    RAAT__TRACE("[source_selection/test] initialized");

    *out_source_selection = &self->plugin;
    return RC__STATUS_SUCCESS;
}

void
RAAT__test_source_selection_plugin_delete(RAAT__SourceSelectionPlugin *source_selection) {
    TestSourceSelectionPlugin *self = (TestSourceSelectionPlugin*)source_selection;
    uv_mutex_destroy(&self->lock);
    if (self->info) json_decref(self->info);
    RAAT__source_selection_state_listeners_destroy(&self->state_listeners);
    RC__free(self->alloc, self);
}


