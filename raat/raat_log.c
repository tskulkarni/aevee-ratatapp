//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_log.h"
#include "rc_list.h"
#include "rc_log.h"
#include "raat_base.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <stdarg.h>

#include <uv.h>

typedef struct LogLink_s {
    struct LogLink_s *next;
    RAAT__LogLevel level;
    int seq;
    int datalen;
    int64_t     time;
} LogLink;

typedef struct {
    RAAT__LogEntryCallback    cb;
    void                     *userdata;
} Callback;

/** \internal */
struct RAAT__Log_s {
    RC__Allocator              *alloc;
    uv_mutex_t                  lock;
    
    size_t                      log_buffer_size;
    char*                       log_buffer;

    LogLink                    *oldest;
    LogLink                    *newest;
    int                         next_seq;
    int64_t                     start_time;

    RC__List                    callbacks;
};

RC_API const char * const RAAT__LogLevelString[] = {
    "CRITICAL",
    "ERROR",
    "WARNING",
    "INFO",
    "TRACE",
    "DEBUG"
};

RC_API RC__Status 
RAAT__log_new               (RC__Allocator *alloc, size_t log_buffer_size, RAAT__Log **out_self) {
    RAAT__static_init();

    RC__Status status = RC__STATUS_UNEXPECTED_ERROR;

    RC__ASSERT(out_self != NULL);
    RC__ASSERT(log_buffer_size >= RAAT__LOG_MINIMUM_SIZE);

    *out_self = NULL;
    RAAT__Log *self = RC__new0(alloc, RAAT__Log, 1);
    if (self == NULL) {
        status = RC__STATUS_OUT_OF_MEMORY;
        goto fail;
    }
    self->alloc = RC__allocator_default(alloc);
    RC__list_init(&self->callbacks, self->alloc);

    self->log_buffer_size = log_buffer_size;
    self->log_buffer = (char*)RC__alloc(self->alloc, log_buffer_size);
    if (self->log_buffer == NULL) {
        status = RC__STATUS_OUT_OF_MEMORY;
        goto fail;
    }

    self->start_time = RC__now_us();

    uv_mutex_init(&self->lock);

    *out_self = self;
    return RC__STATUS_SUCCESS;

fail:
    if (self) {
        if (self->log_buffer) RC__free(self->alloc, self->log_buffer);
        RC__free(self->alloc, self);
    }
    return status;
}

void
RAAT__log_delete           (RAAT__Log *self) {
    if (!self) return;
    uv_mutex_destroy(&self->lock);
    RC__free(self->alloc, self);
}

RC__Status
RAAT__log_add_callback(RAAT__Log *self, RAAT__LogEntryCallback cb, void *userdata) {
    RC__ASSERT(self != NULL);
    uv_mutex_lock(&self->lock);
    Callback *callback = RC__new0(self->alloc, Callback, 1);
    if (callback == NULL) {
        uv_mutex_unlock(&self->lock);
        return RC__STATUS_OUT_OF_MEMORY;
    }
    callback->cb        = cb;
    callback->userdata = userdata;
    RC__list_push(&self->callbacks, callback);
    uv_mutex_unlock(&self->lock);
    return RC__STATUS_SUCCESS;
}

RC__Status
RAAT__log_remove_callback(RAAT__Log *self, RAAT__LogEntryCallback cb, void *userdata) {
    uv_mutex_lock(&self->lock);
    RC__ListIter it;
    for (it = RC__list_begin(&self->callbacks); it != RC__list_end(&self->callbacks); it = RC__listiter_next(it)) {
        Callback *listener = (Callback*)RC__listiter_data(it);
        if (listener->cb == cb && listener->userdata == userdata) {
            RC__list_remove(&self->callbacks, it);
            RC__free(self->alloc, listener);
            break;
        }
    }
    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

void LOCKED_append_link(RAAT__Log *self, RAAT__LogLevel level, int seq, int64_t time, const char *buf, int datalen) {
    LogLink *link;

    if (self->newest == NULL) {
        //fprintf(stderr, "append first link\n");
        link = (LogLink*) self->log_buffer;
        self->newest = self->oldest = link;
        memset(link, 0, sizeof(LogLink));

    } else {
        // compute the first position at which we could allocate a LogLink directly following newest
        char *after_link;
        char *after_newest;

        after_newest = (char*)(((LogLink*)self->newest)+1) + self->newest->datalen;
        link         = (LogLink*)(((uintptr_t)after_newest + 7) & (~(uintptr_t)7));
        after_link   = (char*)(link+1) + datalen;

        // wrap around circular buffer
        if (after_link > self->log_buffer + self->log_buffer_size) {
            //fprintf(stderr, "wrap around\n");
            link       = (LogLink*)self->log_buffer;
            after_link = (char*)(link+1) + datalen;
        }

        // if we overlapped with the oldest link in the buffer, get rid of it
        for (;;) {
            char *after_oldest = (char*)(((LogLink*)self->oldest)+1) + self->oldest->datalen;
            if ((char*)self->oldest > after_link || after_oldest < (char*)link) break;  // no overlap
            /*
            fprintf(stderr, "delete oldest link due to overlap me: %p-%p oldest %p-%p\n",
                    link, after_link,
                    self->oldest, after_oldest);
                    */
            self->oldest = self->oldest->next;
        }

        link = (LogLink*)link;
        memset(link, 0, sizeof(LogLink));
        self->newest->next = link;
        self->newest = link;
    }

    link->seq  = seq;
    link->time = time;
    link->level = level;
    link->datalen  = datalen;

    char *str = (char*)(link+1);                
    memcpy(str, buf, datalen);       
}

void RAAT__log_writef(RAAT__Log *self, RAAT__LogLevel level, const char *fmt, ...) {
    va_list ap;                         
    if (self == NULL) {
        // if we don't have a RAAT log, then fall back on RC__Log infrastructure
        va_start(ap, fmt);                  
        RC__log_writev((RC__LogLevel)level, fmt, ap);
        va_end(ap);
        return;
    }
    RC__ASSERT(self != NULL);
    int64_t     time = RC__now_us() - self->start_time;
    int datalen;                // datalen includes the terminating '\0'
    char buf[2048];
    int seq;

    va_start(ap, fmt);                  
    datalen = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

#if PLATFORM_WINDOWS
    // on windows, vsnprintf returns -1 when the buffer is too small and does not null terminate
    if (datalen < 0) datalen = sizeof(buf) - 1;
#endif

    if (datalen > sizeof(buf)) datalen = sizeof(buf) - 1;

    // windows does not guarantee null termination. OSX does. Linux is unclear. Better safe than sorry.
	buf[datalen] = '\0';

    uv_mutex_lock(&self->lock);
    seq = self->next_seq;
    if (self->next_seq == INT_MAX) self->next_seq = 0;
    else                           self->next_seq++;

    LOCKED_append_link(self, level, seq, time, buf, datalen+1);

    // Calling the callback inside of the log lock is a tradeoff. This might change.
    RAAT__LogEntry entry;
    entry.seq     = seq;
    entry.level   = level;
    entry.time    = time;
    entry.message = buf;

    RC__ListIter it;
    for (it = RC__list_begin(&self->callbacks); it != RC__list_end(&self->callbacks); it = RC__listiter_next(it)) {
        Callback *callback = (Callback*)RC__listiter_data(it);
        callback->cb(&entry, callback->userdata);
    }
    uv_mutex_unlock(&self->lock);

        //fprintf(stderr, "[%07d] %4.3f %s %s\n", seq, to_seconds(time), RAAT__LogLevelString[level], buf);
}

void RAAT__log_iterate                  (RAAT__Log                     *self,
                                         int                            minimum_seq, 
                                         RAAT__LogEntryCallback         cb, 
                                         void                          *userdata) {
    LogLink *link;
    uv_mutex_lock(&self->lock);
    link = self->oldest;
    while (link) {
        if (minimum_seq < 0 || link->seq >= minimum_seq || (minimum_seq > (INT_MAX/2) && link->seq < INT_MAX/2)) {
            RAAT__LogEntry entry;
            entry.seq     = link->seq;
            entry.level   = link->level;
            entry.time    = link->time;
            entry.message = (char*)(link+1);
            cb(&entry, userdata);
        }
        link = link->next;
    }
    uv_mutex_unlock(&self->lock);
}

static void destroy_cb(void *data, void *userdata) {
    RC__Allocator *alloc = userdata;
    RC__free(alloc, data);
}

void RAAT__log_clear                    (RAAT__Log                     *self) {
    RC__ASSERT(self != NULL);
    uv_mutex_lock(&self->lock);
    self->oldest = self->newest = NULL;
    RC__list_foreach_destroy(&self->callbacks, destroy_cb, self->alloc);
    uv_mutex_unlock(&self->lock);
}

const char * RAAT__log_status_to_string(RC__Status status) {
    return NULL;
}

