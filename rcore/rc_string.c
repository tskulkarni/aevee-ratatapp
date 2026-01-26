//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifdef PLATFORM_LINUX
#define _GNU_SOURCE 
#endif

#include "rc_string.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RC__STRING_DEFAULT_SIZE (64)

void
RC__string_init(RC__String *self, RC__Allocator *alloc) {
    RC__ASSERT(self != NULL);

    self->str  = (char*)RC__alloc(alloc, RC__STRING_DEFAULT_SIZE);
    RC__ASSERT(self->str != NULL);
    self->alloc     = alloc;
    self->capacity  = RC__STRING_DEFAULT_SIZE;
    self->length    = 0;

    *self->str = '\0';          // NULL terminate
}

static
void reserve(RC__String *self, size_t nbytes) {
    bool did_resize = false;
    RC__ASSERT(self != NULL);

    while (self->length + nbytes + 1 > self->capacity) {
        self->capacity *= 2;
        did_resize = true;
    }
    if (did_resize) {
        void *str = RC__alloc(self->alloc, self->capacity);
        RC__ASSERT(str != NULL);
        memcpy(str, self->str, self->length + 1);
        RC__free(self->alloc, self->str);
        self->str = str;
    }
}

void 
RC__string_append(RC__String *self, const char *str) {
    RC__ASSERT(self != NULL);

    if (str == NULL) return;

    RC__string_append_len(self, str, strlen(str));
}

void 
RC__string_append_len(RC__String *self, const char *str, size_t len) {
    RC__ASSERT(self != NULL);
    reserve(self, len);
    memcpy(self->str + self->length, str, len);
    self->length += len;
    self->str[self->length] = 0;
}

void 
RC__string_append_c(RC__String *self, char c) {
    RC__ASSERT(self != NULL);
    reserve(self, 1);
    self->str[self->length++] = c;
    self->str[self->length]   = 0;
}

void 
RC__string_append_vprintf(RC__String *self, const char *fmt, va_list ap) {
    char *mallocstr;
    int len;
    len = vasprintf(&mallocstr, fmt, ap);
    RC__string_append_len(self, mallocstr, len);
    free(mallocstr);
}

void 
RC__string_append_printf(RC__String *self, const char *fmt, ...) {
    va_list ap;

    RC__ASSERT(self != NULL);
    RC__ASSERT(fmt != NULL);

    va_start(ap, fmt);
    RC__string_append_vprintf(self, fmt, ap);
    va_end(ap);
}

void
RC__string_destroy(RC__String *self) {
    if (!self) return;
    RC__free(self->alloc, self->str);
    self->str = NULL;
}
