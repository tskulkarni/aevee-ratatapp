//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "rc_base.h"
#include "rc_status.h"
#include "rc_allocator.h"
#include "rc_list.h"

#include <string.h>
#include <stdio.h>
#include <uv.h>

typedef struct {
    int                min;
    int                max;
    RC__StatusToString fn;
} StatusRange;

static uv_once_t  status_init_once      = UV_ONCE_INIT;
static uv_mutex_t status_lock;
static RC__List   status_ranges;

static void static_init_cb(void) {
    RC__list_init(&status_ranges, RC__ALLOCATOR_DEFAULT);
    uv_mutex_init(&status_lock);
}

static void static_init() {
    uv_once(&status_init_once, static_init_cb);
}

void RC__status_register(int min, int max, RC__StatusToString fn) {
    static_init();
    uv_mutex_lock(&status_lock);
    RC__ListIter it;
    for (it = RC__list_begin(&status_ranges); it != RC__list_end(&status_ranges); it = RC__listiter_next(it)) {
        StatusRange *range = RC__listiter_data(it);
        if (range->fn == fn) {
            uv_mutex_unlock(&status_lock);
            return;    // don't insert dups
        }
    }
    StatusRange *range = RC__new0(RC__ALLOCATOR_DEFAULT, StatusRange, 1);
    if (!range) return;
    range->min = min;
    range->max = max;
    range->fn = fn;
    RC__list_push(&status_ranges, range);
    uv_mutex_unlock(&status_lock);
}

/** Convert a status code to a string */
const char *RC__status_to_string(RC__Status status) {
    static_init();
    const char *ret = NULL;

    if (RC__STATUS_IS_GENERIC(status)) {
        switch (status) {
            case RC__STATUS_SUCCESS:          ret = "RC__STATUS_SUCCESS";               break;
            case RC__STATUS_UNEXPECTED_ERROR: ret = "RC__STATUS_UNEXPECTED_ERROR";      break;
            case RC__STATUS_OUT_OF_MEMORY:    ret = "RC__STATUS_OUT_OF_MEMORY";         break;
            case RC__STATUS_NOT_IMPLEMENTED:  ret = "RC__STATUS_NOT_IMPLEMENTED";       break;
            case RC__STATUS_NOT_SUPPORTED:    ret = "RC__STATUS_NOT_SUPPORTED";         break;
            case RC__STATUS_INVALID_ARGUMENT: ret = "RC__STATUS_INVALID_ARGUMENT";      break;
            case RC__STATUS_CANCELED:         ret = "RC__STATUS_CANCELED";              break;
            case RC__STATUS_NOT_FOUND:        ret = "RC__STATUS_NOT_FOUND";             break;
            default:                          break;
        }
    }

    uv_mutex_lock(&status_lock);
    RC__ListIter it;
    for (it = RC__list_begin(&status_ranges); it != RC__list_end(&status_ranges); it = RC__listiter_next(it)) {
        StatusRange *range = RC__listiter_data(it);
        if (status >= range->min && status <= range->max) {
            ret = range->fn(status);
            break;
        }
    }
    uv_mutex_unlock(&status_lock);

    if (ret == NULL) {
        fprintf(stderr, "RC__status_to_string: unknown status %d\n", status);
    }

    RC__ASSERT(ret != NULL);

    return ret;
}

