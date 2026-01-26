//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "rc_log.h"
#include "rc_list.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <stdarg.h>

#include <uv.h>

typedef struct {
    RC__LogEntryCallback    cb;
    void                     *userdata;
} Callback;

static uv_once_t                   log_init_once         = UV_ONCE_INIT;
static uv_mutex_t                  log_mutex;
static int                         next_seq;
static int64_t                     start_time;
static RC__List                    callbacks;

RC_API const char * const RC__LogLevelString[] = {
    "CRITICAL",
    "ERROR",
    "WARNING",
    "INFO",
    "TRACE",
    "DEBUG"
};


static void static_init_cb() {
    RC__list_init(&callbacks, RC__ALLOCATOR_DEFAULT);
    uv_mutex_init(&log_mutex);
    start_time = RC__now_us();
}

static void RC__log_static_init() {
    uv_once(&log_init_once, static_init_cb);
}

RC__Status
RC__log_add_callback(RC__LogEntryCallback cb, void *userdata) {
    RC__log_static_init();
    uv_mutex_lock(&log_mutex);
    Callback *callback = RC__new0(RC__ALLOCATOR_DEFAULT, Callback, 1);
    if (callback == NULL) {
        uv_mutex_unlock(&log_mutex);
        return RC__STATUS_OUT_OF_MEMORY;
    }
    callback->cb        = cb;
    callback->userdata = userdata;
    RC__list_push(&callbacks, callback);
    uv_mutex_unlock(&log_mutex);
    return RC__STATUS_SUCCESS;
}

RC__Status
RC__log_remove_callback(RC__LogEntryCallback cb, void *userdata) {
    RC__log_static_init();
    RC__ListIter it;
    for (it = RC__list_begin(&callbacks); it != RC__list_end(&callbacks); it = RC__listiter_next(it)) {
        Callback *listener = (Callback*)RC__listiter_data(it);
        if (listener->cb == cb && listener->userdata == userdata) {
            RC__list_remove(&callbacks, it);
            RC__free(RC__ALLOCATOR_DEFAULT, listener);
            break;
        }
    }
    uv_mutex_unlock(&log_mutex);

    return RC__STATUS_SUCCESS;
}

void RC__log_writef(RC__LogLevel level, const char *fmt, ...) {
    va_list ap;                         
    va_start(ap, fmt);                  
    RC__log_writev(level, fmt, ap);
    va_end(ap);
}

void RC__log_writev                   (RC__LogLevel                 level, 
                                       const char                    *fmt, 
                                       va_list ap) {
    RC__log_static_init();
    int64_t time = RC__now_us() - start_time;
    size_t  datalen;
    char buf[4096];

    datalen = vsnprintf(buf, sizeof(buf), fmt, ap);

#if PLATFORM_WINDOWS
    // on windows, vsnprintf returns -1 when the buffer is too small and does not null terminate
    if (datalen < 0) datalen = sizeof(buf) - 1;
#endif

    if (datalen >= sizeof(buf)) datalen = sizeof(buf) - 1;

    // windows does not guarantee null termination. OSX does. Linux is unclear. Better safe than sorry.
    buf[datalen] = '\0';

    // Calling the callback inside of the log lock is a tradeoff. This might change.
    uv_mutex_lock(&log_mutex);

    RC__LogEntry entry;
    entry.seq     = ++next_seq;
    entry.level   = level;
    entry.time    = time;
    entry.message = buf;

    RC__ListIter it;
    for (it = RC__list_begin(&callbacks); it != RC__list_end(&callbacks); it = RC__listiter_next(it)) {
        Callback *callback = (Callback*)RC__listiter_data(it);
        callback->cb(&entry, callback->userdata);
    }
    uv_mutex_unlock(&log_mutex);
        //fprintf(stderr, "[%07d] %4.3f %s %s\n", seq, to_seconds(time), RC__LogLevelString[level], buf);
}

