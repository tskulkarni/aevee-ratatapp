//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_plugin_volume_null.h"
#include "rc_list.h"

#include <uv.h>

#define RAAT__CURRENT_LOG self->log

/*
 * Volume Plugin
 */
typedef struct {
    RAAT__VolumePlugin  plugin;          // must be first item in struct
    RC__Allocator      *alloc;
    RAAT__Log          *log;
    json_t             *info;
} NullVolumePlugin;

static RC__Status volume_add_state_listener(void *vself, RAAT__VolumeStateCallback cb, void *userdata) {
    return RC__STATUS_SUCCESS;
}

static RC__Status volume_remove_state_listener(void *vself, RAAT__VolumeStateCallback cb, void *userdata) {
    return RC__STATUS_SUCCESS;
}

static RC__Status volume_get_info(void *vself, json_t **out_info) {
    NullVolumePlugin *self = (NullVolumePlugin*)vself;

    RC__ASSERT(self);
    RC__ASSERT(out_info);

    json_incref(self->info);
    *out_info = self->info;

    return RC__STATUS_SUCCESS;
}

static RC__Status volume_get_state(void *vself, RAAT__VolumeState *out_state) {
    NullVolumePlugin *self = (NullVolumePlugin*)vself;

    RC__ASSERT(self != NULL);
    RC__ASSERT(out_state != NULL);

    out_state->volume_type  = RAAT__VOLUME_TYPE_NONE;
    out_state->min_volume   = 0;
    out_state->max_volume   = 0;
    out_state->volume_value = 0;
    out_state->mute_value   = false;

    return RC__STATUS_SUCCESS;
}

static RC__Status volume_set_volume(void *vself, double volume_value) {
    return RC__STATUS_NOT_SUPPORTED;
}

static RC__Status volume_set_mute(void *vself, bool mute_value) {
    return RC__STATUS_NOT_SUPPORTED;
}

RC__Status 
RAAT__null_volume_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__VolumePlugin **out_volume) { 
    alloc = RC__allocator_default(alloc);
    NullVolumePlugin *self            = RC__new0(alloc, NullVolumePlugin, 1);
    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;
    self->alloc                        = alloc;
    self->log                          = RAAT__device_get_log(device);
    self->plugin.get_info              = volume_get_info;
    self->plugin.add_state_listener    = volume_add_state_listener;
    self->plugin.remove_state_listener = volume_remove_state_listener;
    self->plugin.get_state             = volume_get_state;
    self->plugin.set_volume            = volume_set_volume;
    self->plugin.set_mute              = volume_set_mute;

    self->info = json_object();
    json_object_set(self->info, "config", config);

    RAAT__TRACE("[volume/null] initialized");

    *out_volume = &self->plugin;
    return RC__STATUS_SUCCESS;
}

void
RAAT__null_volume_plugin_delete(RAAT__VolumePlugin *volume) {
    NullVolumePlugin *self = (NullVolumePlugin*)volume;
    if (self->info) json_decref(self->info);
    RC__free(self->alloc, self);
}

