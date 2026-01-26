//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_plugin_transport.h" 

const char * RAAT__transport_plugin_status_to_string(RC__Status status) {
    RC__ASSERT(status >= RAAT__TRANSPORT_PLUGIN_STATUS_BASE && status <= RAAT__TRANSPORT_PLUGIN_STATUS_MAX);

    switch (status) {
        default: RC__ASSERT(0); return NULL;
    }
    return NULL;
}

typedef struct {
    RAAT__TransportControlCallback    cb;
    void                        *userdata;
} TransportControlListener;

RC__Status 
RAAT__transport_control_listeners_add(RAAT__TransportControlListeners *self, RAAT__TransportControlCallback cb, void *userdata) {
    uv_mutex_lock(&self->lock);
    TransportControlListener *listener = RC__new0(self->alloc, TransportControlListener, 1);
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
RAAT__transport_control_listeners_remove(RAAT__TransportControlListeners *self, RAAT__TransportControlCallback cb, void *userdata) {
    uv_mutex_lock(&self->lock);
    RC__ListIter it;
    for (it = RC__list_begin(&self->listeners); it != RC__list_end(&self->listeners); it = RC__listiter_next(it)) {
        TransportControlListener *listener = (TransportControlListener*)RC__listiter_data(it);
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
RAAT__transport_control_listeners_invoke(RAAT__TransportControlListeners *self, json_t *control) {
    uv_mutex_lock(&self->lock);
    RC__ListIter it;
    for (it = RC__list_begin(&self->listeners); it != RC__list_end(&self->listeners); it = RC__listiter_next(it)) {
        TransportControlListener *listener = (TransportControlListener*)RC__listiter_data(it);
        listener->cb(listener->userdata, control);
    }
    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

RC__Status 
RAAT__transport_control_listeners_init(RAAT__TransportControlListeners *self, RC__Allocator *alloc) {
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
RAAT__transport_control_listeners_destroy(RAAT__TransportControlListeners *self) {
    uv_mutex_destroy(&self->lock);
    RC__list_foreach_destroy(&self->listeners, destroy_cb, self->alloc);
}
