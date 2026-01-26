//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_plugin_volume_dummy.h"
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
    bool                         mute;
    double                       volume;
    json_t                      *info;
} DummyVolumePlugin;

static RC__Status volume_get_info(void *vself, json_t **out_info) {
    DummyVolumePlugin *self = (DummyVolumePlugin*)vself;

    RC__ASSERT(self);
    RC__ASSERT(out_info);

    json_incref(self->info);
    *out_info = self->info;

    return RC__STATUS_SUCCESS;
}

static RC__Status volume_add_state_listener(void *vself, RAAT__VolumeStateCallback cb, void *userdata) {
    DummyVolumePlugin *self = (DummyVolumePlugin*)vself;
    return RAAT__volume_state_listeners_add(&self->state_listeners, cb, userdata);
}

static RC__Status volume_remove_state_listener(void *vself, RAAT__VolumeStateCallback cb, void *userdata) {
    DummyVolumePlugin *self = (DummyVolumePlugin*)vself;
    return RAAT__volume_state_listeners_remove(&self->state_listeners, cb, userdata);
}

static void LOCKED_get_state(DummyVolumePlugin *self, RAAT__VolumeState *out_state) {
    out_state->volume_type  = RAAT__VOLUME_TYPE_NUMBER;
    out_state->min_volume   = 0;
    out_state->max_volume   = 100;
    out_state->volume_value = self->volume;
    out_state->mute_value   = self->mute;
}

static RC__Status volume_get_state(void *vself, RAAT__VolumeState *out_state) {
    DummyVolumePlugin *self = (DummyVolumePlugin*)vself;

    RC__ASSERT(self != NULL);
    RC__ASSERT(out_state != NULL);

    uv_mutex_lock(&self->lock);
    LOCKED_get_state(self, out_state);
    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

static RC__Status volume_set_volume(void *vself, double volume_value) {
    DummyVolumePlugin *self = (DummyVolumePlugin*)vself;
    RAAT__VolumeState state;
    bool changed = false;

    uv_mutex_lock(&self->lock);
    if (self->volume != volume_value) {
        self->volume = volume_value;
        RAAT__TRACE("[volume/dummy] volume => %d", volume_value);
        changed = true;
        LOCKED_get_state(self, &state);
    }
    uv_mutex_unlock(&self->lock);

    if (changed) RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);

    return RC__STATUS_SUCCESS;
}

static RC__Status volume_set_mute(void *vself, bool mute_value) {
    DummyVolumePlugin *self = (DummyVolumePlugin*)vself;
    RAAT__VolumeState state;
    bool changed = false;

    uv_mutex_lock(&self->lock);
    if (self->mute != mute_value) {
        self->mute = mute_value;
        RAAT__TRACE("[volume/dummy] mute => %d", mute_value);
        changed = true;
        LOCKED_get_state(self, &state);
    }
    uv_mutex_unlock(&self->lock);
    
    if (changed) RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);

    return RC__STATUS_SUCCESS;
}

RC__Status 
RAAT__dummy_volume_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__VolumePlugin **out_volume) { 
    alloc = RC__allocator_default(alloc);
    DummyVolumePlugin *self            = RC__new0(alloc, DummyVolumePlugin, 1);
    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;
    self->alloc                        = alloc;
    self->log                          = RAAT__device_get_log(device);
    self->plugin.get_info              = volume_get_info;
    self->plugin.add_state_listener    = volume_add_state_listener;
    self->plugin.remove_state_listener = volume_remove_state_listener;
    self->plugin.get_state             = volume_get_state;
    self->plugin.set_volume            = volume_set_volume;
    self->plugin.set_mute              = volume_set_mute;

    self->mute     = false;
    self->volume   = 100;
    uv_mutex_init(&self->lock);
    RAAT__volume_state_listeners_init(&self->state_listeners, self->alloc);

    self->info = json_object();
    json_object_set(self->info, "config", config);

    RAAT__TRACE("[volume/dummy] initialized");

    *out_volume = &self->plugin;
    return RC__STATUS_SUCCESS;
}

void
RAAT__dummy_volume_plugin_delete(RAAT__VolumePlugin *volume) {
    DummyVolumePlugin *self = (DummyVolumePlugin*)volume;
    uv_mutex_destroy(&self->lock);
    if (self->info) json_decref(self->info);
    RAAT__volume_state_listeners_destroy(&self->state_listeners);
    RC__free(self->alloc, self);
}

