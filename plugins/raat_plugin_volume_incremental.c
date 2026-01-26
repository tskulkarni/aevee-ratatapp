//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_plugin_volume_incremental.h"
#include "rc_list.h"

#include <uv.h>

#define RAAT__CURRENT_LOG self->log

/*
 * Volume Plugin
 */
typedef struct {
    RAAT__VolumePlugin           plugin;          // must be first item in struct
    RC__Allocator               *alloc;
    RAAT__Log                   *log;
    RAAT__VolumeStateListeners   state_listeners;
    uv_mutex_t                   lock;
    json_t                      *info;
} IncrementalVolumePlugin;

static RC__Status volume_get_info(void *vself, json_t **out_info) {
    IncrementalVolumePlugin *self = (IncrementalVolumePlugin*)vself;

    RC__ASSERT(self);
    RC__ASSERT(out_info);

    json_incref(self->info);
    *out_info = self->info;

    return RC__STATUS_SUCCESS;
}

static RC__Status volume_add_state_listener(void *vself, RAAT__VolumeStateCallback cb, void *userdata) {
    IncrementalVolumePlugin *self = (IncrementalVolumePlugin*)vself;
    return RAAT__volume_state_listeners_add(&self->state_listeners, cb, userdata);
}

static RC__Status volume_remove_state_listener(void *vself, RAAT__VolumeStateCallback cb, void *userdata) {
    IncrementalVolumePlugin *self = (IncrementalVolumePlugin*)vself;
    return RAAT__volume_state_listeners_remove(&self->state_listeners, cb, userdata);
}

static void LOCKED_get_state(IncrementalVolumePlugin *self, RAAT__VolumeState *out_state) {
    memset(out_state, 0, sizeof(*out_state));
    out_state->volume_type  = RAAT__VOLUME_TYPE_INCREMENTAL;
}

static RC__Status volume_get_state(void *vself, RAAT__VolumeState *out_state) {
    IncrementalVolumePlugin *self = (IncrementalVolumePlugin*)vself;

    RC__ASSERT(self != NULL);
    RC__ASSERT(out_state != NULL);

    uv_mutex_lock(&self->lock);
    LOCKED_get_state(self, out_state);
    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

static RC__Status volume_increment_volume(void *vself, RAAT__VolumeIncrement inc) {
    IncrementalVolumePlugin *self = (IncrementalVolumePlugin*)vself;

    if (inc == RAAT__VOLUME_INCREMENT_UP) {
        RAAT__TRACE("[volume/incremental] Volume Up");
    } else if (inc == RAAT__VOLUME_INCREMENT_DOWN) {
        RAAT__TRACE("[volume/incremental] Volume Down");
    } else {
        RAAT__ERROR("[volume/incremental] unknown volume increment: %d", (int)inc);
    }

    return RC__STATUS_SUCCESS;
}

static RC__Status volume_toggle_mute(void *vself) {
    IncrementalVolumePlugin *self = (IncrementalVolumePlugin*)vself;

    RAAT__TRACE("[volume/incremental] Toggle Mute");

    return RC__STATUS_SUCCESS;
}

RC__Status 
RAAT__incremental_volume_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__VolumePlugin **out_volume) { 
    alloc = RC__allocator_default(alloc);
    IncrementalVolumePlugin *self            = RC__new0(alloc, IncrementalVolumePlugin, 1);
    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;
    self->alloc                        = alloc;
    self->log                          = RAAT__device_get_log(device);
    self->plugin.get_info              = volume_get_info;
    self->plugin.add_state_listener    = volume_add_state_listener;
    self->plugin.remove_state_listener = volume_remove_state_listener;
    self->plugin.get_state             = volume_get_state;
    self->plugin.increment_volume      = volume_increment_volume;
    self->plugin.toggle_mute           = volume_toggle_mute;

    uv_mutex_init(&self->lock);
    RAAT__volume_state_listeners_init(&self->state_listeners, self->alloc);

    self->info = json_object();
    json_object_set(self->info, "config", config);

    RAAT__TRACE("[volume/incremental] initialized");

    *out_volume = &self->plugin;
    return RC__STATUS_SUCCESS;
}

void
RAAT__incremental_volume_plugin_delete(RAAT__VolumePlugin *volume) {
    IncrementalVolumePlugin *self = (IncrementalVolumePlugin*)volume;
    uv_mutex_destroy(&self->lock);
    if (self->info) json_decref(self->info);
    RAAT__volume_state_listeners_destroy(&self->state_listeners);
    RC__free(self->alloc, self);
}

