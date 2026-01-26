//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_plugin_source_selection.h" 

const char * RAAT__source_selection_plugin_status_to_string(RC__Status status) {
    RC__ASSERT(status >= RAAT__SOURCE_SELECTION_PLUGIN_STATUS_BASE && status <= RAAT__SOURCE_SELECTION_PLUGIN_STATUS_MAX);

    switch (status) {
        case RAAT__SOURCE_SELECTION_PLUGIN_STATUS_SOURCE_NOT_AVAILABLE: return "RAAT__SOURCE_SELECTION_PLUGIN_STATUS_SOURCE_NOT_AVAILABLE";
        case RAAT__SOURCE_SELECTION_PLUGIN_STATUS_TIMEOUT:              return "RAAT__SOURCE_SELECTION_PLUGIN_STATUS_TIMEOUT";
        default: RC__ASSERT(0); return NULL;
    }
    return NULL;
}

typedef struct {
    RAAT__SourceSelectionStateCallback    cb;
    void                        *userdata;
} SourceSelectionStateListener;

RC__Status 
RAAT__source_selection_state_listeners_add(RAAT__SourceSelectionStateListeners *self, RAAT__SourceSelectionStateCallback cb, void *userdata) {
    uv_mutex_lock(&self->lock);
    SourceSelectionStateListener *listener = RC__new0(self->alloc, SourceSelectionStateListener, 1);
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
RAAT__source_selection_state_listeners_remove(RAAT__SourceSelectionStateListeners *self, RAAT__SourceSelectionStateCallback cb, void *userdata) {
    uv_mutex_lock(&self->lock);
    RC__ListIter it;
    for (it = RC__list_begin(&self->listeners); it != RC__list_end(&self->listeners); it = RC__listiter_next(it)) {
        SourceSelectionStateListener *listener = (SourceSelectionStateListener*)RC__listiter_data(it);
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
RAAT__source_selection_state_listeners_invoke(RAAT__SourceSelectionStateListeners *self, RAAT__SourceSelectionState *state) {
    uv_mutex_lock(&self->lock);
    RC__ListIter it;
    for (it = RC__list_begin(&self->listeners); it != RC__list_end(&self->listeners); it = RC__listiter_next(it)) {
        SourceSelectionStateListener *listener = (SourceSelectionStateListener*)RC__listiter_data(it);
        listener->cb(listener->userdata, state);
    }
    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

RC__Status 
RAAT__source_selection_state_listeners_init(RAAT__SourceSelectionStateListeners *self, RC__Allocator *alloc) {
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
RAAT__source_selection_state_listeners_destroy(RAAT__SourceSelectionStateListeners *self) {
    uv_mutex_destroy(&self->lock);
    RC__list_foreach_destroy(&self->listeners, destroy_cb, self->alloc);
}
