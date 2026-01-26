//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_plugin_watch_alsa.h"
#include "raat_dsp.h"
#include "rc_list.h"

#include <uv.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <stdlib.h>

#define RAAT__CURRENT_LOG (self->log)

typedef struct {
    RAAT__WatchPlugin            plugin;          // must be first item in struct
    RC__Allocator               *alloc;
    RAAT__Log                   *log;
    char                        *unique_id;          
    bool                         exit;
    uv_thread_t                  tid;
    bool                         done;
    bool                         exit_on_lost;
} AlsaWatchPlugin;

static void watch_thread(void *arg) {
    AlsaWatchPlugin *self = arg;

    while (!self->done) {
        snd_ctl_t *ctl = NULL;
        int rc = snd_ctl_open(&ctl, self->unique_id, 0);
        if (0 != rc) { 
            RAAT__INFO("[watch/alsa] device %s lost", self->unique_id);
            if (self->exit_on_lost) {
                RAAT__INFO("[watch/alsa] exiting");
                exit(0);
            }
        }
        snd_ctl_close(ctl);
        sleep(2);
    }
}

RC__Status 
RAAT__alsa_watch_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__WatchPlugin **out_watch) {
    const char *raw_unique_id = json_string_value(json_object_get(config, "device"));
    if (raw_unique_id == NULL) return RC__STATUS_INVALID_ARGUMENT;

    //
    // turn hw:CARD=foo,DEV=BAR into hw:CARD=foo
    // turn hw:1,0 into hw:1
    //
    char unique_id[1024];
    strcpy(unique_id, raw_unique_id);
    char *comma = strchr(unique_id, ',');
    if (comma) *comma = '\0';

    json_t *lost_action = json_object_get(config, "lost_action");
    bool exit_on_lost = lost_action != NULL && !strcmp(json_string_value(lost_action), "exit");

    alloc = RC__allocator_default(alloc);
    AlsaWatchPlugin *self            = RC__new0(alloc, AlsaWatchPlugin, 1);
    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;

    // initialize the state that is associated with AlsaWatchPlugin 
    self->alloc                        = alloc;
    self->log                          = RAAT__device_get_log(device);
    self->unique_id                    = RC__allocator_strdup(alloc, unique_id);
    self->exit_on_lost                 = exit_on_lost;

    RAAT__TRACE("[watch/alsa] initializing watch device=%s exit_on_lost=%d", unique_id, exit_on_lost);

    uv_thread_create(&self->tid, watch_thread, self);

    RAAT__TRACE("[watch/alsa] initialized");
    *out_watch = &self->plugin;
    return RC__STATUS_SUCCESS;
}

void
RAAT__alsa_watch_plugin_delete(RAAT__WatchPlugin *watch) {
    AlsaWatchPlugin *self = (AlsaWatchPlugin*)watch;

    self->done = true;
    uv_thread_join(&self->tid);

    RC__free(self->alloc, self->unique_id);
    RC__free(self->alloc, self);
}

