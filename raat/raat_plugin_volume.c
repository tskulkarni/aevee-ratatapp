//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_plugin_volume.h"

const char * RAAT__volume_plugin_status_to_string(RC__Status status) {
    RC__ASSERT(status >= RAAT__VOLUME_PLUGIN_STATUS_BASE && status <= RAAT__VOLUME_PLUGIN_STATUS_MAX);
    switch (status) {
        case RAAT__VOLUME_PLUGIN_STATUS_DEVICE_OPEN_FAILED:          return "RAAT__VOLUME_PLUGIN_STATUS_DEVICE_OPEN_FAILED";
        case RAAT__VOLUME_PLUGIN_STATUS_DEVICE_INIT_FAILED:          return "RAAT__VOLUME_PLUGIN_STATUS_DEVICE_INIT_FAILED";
        case RAAT__VOLUME_PLUGIN_STATUS_INVALID_CONFIG:              return "RAAT__VOLUME_PLUGIN_STATUS_INVALID_CONFIG";
        case RAAT__VOLUME_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED:        return "RAAT__VOLUME_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED";
        case RAAT__VOLUME_PLUGIN_STATUS_OUTPUT_PLUGIN_NOT_FOUND:     return "RAAT__VOLUME_PLUGIN_STATUS_OUTPUT_PLUGIN_NOT_FOUND";
        case RAAT__VOLUME_PLUGIN_STATUS_OUTPUT_PLUGIN_NOT_SUPPORTED: return "RAAT__VOLUME_PLUGIN_STATUS_OUTPUT_PLUGIN_NOT_SUPPORTED";
        case RAAT__VOLUME_PLUGIN_STATUS_VOLUME_NOT_SUPPORTED:        return "RAAT__VOLUME_PLUGIN_STATUS_VOLUME_NOT_SUPPORTED";
        default: RC__ASSERT(0); return NULL;
    }
    return NULL;
}

typedef struct {
    RAAT__VolumeStateCallback    cb;
    void                        *userdata;
} VolumeStateListener;

RC__Status 
RAAT__volume_state_listeners_add(RAAT__VolumeStateListeners *self, RAAT__VolumeStateCallback cb, void *userdata) {
    uv_mutex_lock(&self->lock);
    VolumeStateListener *listener = RC__new0(self->alloc, VolumeStateListener, 1);
    if (listener == NULL) {
        uv_mutex_unlock(&self->lock);
        return RC__STATUS_OUT_OF_MEMORY;
    }
    listener->cb        = cb;
    listener->userdata = userdata;
    RC__list_push(&self->listeners, listener);
    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

RC__Status 
RAAT__volume_state_listeners_remove(RAAT__VolumeStateListeners *self, RAAT__VolumeStateCallback cb, void *userdata) {
    uv_mutex_lock(&self->lock);
    RC__ListIter it;
    for (it = RC__list_begin(&self->listeners); it != RC__list_end(&self->listeners); it = RC__listiter_next(it)) {
        VolumeStateListener *listener = (VolumeStateListener*)RC__listiter_data(it);
        if (listener->cb == cb && listener->userdata == userdata) {
            RC__list_remove(&self->listeners, it);
            RC__free(self->alloc, listener);
            break;
        }
    }
    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

RC__Status 
RAAT__volume_state_listeners_invoke(RAAT__VolumeStateListeners *self, RAAT__VolumeState *state) {
    uv_mutex_lock(&self->lock);
    RC__ListIter it;
    for (it = RC__list_begin(&self->listeners); it != RC__list_end(&self->listeners); it = RC__listiter_next(it)) {
        VolumeStateListener *listener = (VolumeStateListener*)RC__listiter_data(it);
        listener->cb(listener->userdata, state);
    }
    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

RC__Status 
RAAT__volume_state_listeners_init(RAAT__VolumeStateListeners *self, RC__Allocator *alloc) {
    self->alloc = alloc;
    uv_mutex_init(&self->lock);
    RC__list_init(&self->listeners, self->alloc);
    return RC__STATUS_SUCCESS;
}

static void destroy_cb(void *data, void *userdata) {
    RC__Allocator *alloc = userdata;
    RC__free(alloc, data);
}

void 
RAAT__volume_state_listeners_destroy(RAAT__VolumeStateListeners *self) {
    uv_mutex_destroy(&self->lock);
    RC__list_foreach_destroy(&self->listeners, destroy_cb, self->alloc);
}
