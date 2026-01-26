//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_plugin_output.h" 

const char * RAAT__output_plugin_status_to_string(RC__Status status) {
    RC__ASSERT(status >= RAAT__OUTPUT_PLUGIN_STATUS_BASE && status <= RAAT__OUTPUT_PLUGIN_STATUS_MAX);

    switch (status) {
        case RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED:       return "RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED";
        case RAAT__OUTPUT_PLUGIN_STATUS_INVALID_TOKEN:              return "RAAT__OUTPUT_PLUGIN_STATUS_INVALID_TOKEN";
        case RAAT__OUTPUT_PLUGIN_STATUS_INVALID_STATE:              return "RAAT__OUTPUT_PLUGIN_STATUS_INVALID_STATE";
        case RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED:         return "RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED";
        case RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED:         return "RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED";
        case RAAT__OUTPUT_PLUGIN_STATUS_INVALID_CONFIG:             return "RAAT__OUTPUT_PLUGIN_STATUS_INVALID_CONFIG";
        case RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_IN_USE:              return "RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_IN_USE";
        default: RC__ASSERT(0); return NULL;
    }
    return NULL;
}

typedef struct {
    RAAT__OutputMessageCallback    cb;
    void                          *userdata;
} OutputMessageListener;

RC__Status 
RAAT__output_message_listeners_add(RAAT__OutputMessageListeners *self, RAAT__OutputMessageCallback cb, void *userdata) {
    uv_mutex_lock(&self->lock);
    OutputMessageListener *listener = RC__new0(self->alloc, OutputMessageListener, 1);
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
RAAT__output_message_listeners_remove(RAAT__OutputMessageListeners *self, RAAT__OutputMessageCallback cb, void *userdata) {
    uv_mutex_lock(&self->lock);
    RC__ListIter it;
    for (it = RC__list_begin(&self->listeners); it != RC__list_end(&self->listeners); it = RC__listiter_next(it)) {
        OutputMessageListener *listener = (OutputMessageListener*)RC__listiter_data(it);
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
RAAT__output_message_listeners_invoke(RAAT__OutputMessageListeners *self, json_t *message) {
    uv_mutex_lock(&self->lock);
    RC__ListIter it;
    for (it = RC__list_begin(&self->listeners); it != RC__list_end(&self->listeners); it = RC__listiter_next(it)) {
        OutputMessageListener *listener = (OutputMessageListener*)RC__listiter_data(it);
        listener->cb(listener->userdata, message);
    }
    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

RC__Status 
RAAT__output_message_listeners_init(RAAT__OutputMessageListeners *self, RC__Allocator *alloc) {
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
RAAT__output_message_listeners_destroy(RAAT__OutputMessageListeners *self) {
    uv_mutex_destroy(&self->lock);
    RC__list_foreach_destroy(&self->listeners, destroy_cb, self->alloc);
}
