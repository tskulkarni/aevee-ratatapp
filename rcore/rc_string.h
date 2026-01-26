//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RC_STRING_H
#define INCLUDED_RC_STRING_H

#include "rc_base.h"
#include "rc_allocator.h"

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    RC__Allocator *alloc;
    char *str;
    size_t capacity;
    size_t length;
} RC__String;

void
RC__string_init(RC__String *self, RC__Allocator *alloc);

void 
RC__string_append(RC__String *self, const char *str);

void 
RC__string_append_len(RC__String *self, const char *str, size_t len);

void 
RC__string_append_c(RC__String *self, char c);

void 
RC__string_append_vprintf(RC__String *self, const char *fmt, va_list ap);

void 
RC__string_append_printf(RC__String *self, const char *fmt, ...);

void
RC__string_destroy(RC__String *self);


#ifdef __cplusplus
}
#endif

#endif
