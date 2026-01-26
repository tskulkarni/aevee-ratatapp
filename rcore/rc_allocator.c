//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "rc_allocator.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

void *RC__allocator_resize0(RC__Allocator *self, void *ptr, size_t old_n_bytes, size_t new_n_bytes)
{
    void *ret;

    if (new_n_bytes == 0) {
        RC__free(self, ptr);
        return NULL;
    }

    ret = RC__alloc(self, new_n_bytes);
    if (ret == NULL) {
        RC__free(self, ptr);
        return NULL;
    }

    memset(ret, 0, new_n_bytes);
    memcpy(ret, ptr, old_n_bytes);
    RC__free(self, ptr);
    return ret;
}

void *RC__allocator_resize(RC__Allocator *self, void *ptr, size_t old_n_bytes, size_t new_n_bytes)
{
    void *ret;

    if (new_n_bytes == 0) {
        RC__free(self, ptr);
        return NULL;
    }

    ret = RC__alloc(self, new_n_bytes);
    if (ret == NULL) {
        RC__free(self, ptr);
        return NULL;
    }

    memcpy(ret, ptr, old_n_bytes);
    RC__free(self, ptr);
    return ret;
}

void *RC__allocator_alloc(RC__Allocator *self, size_t n_bytes)
{
    void *ret= (*self->alloc)(n_bytes);
    return ret;
}

void *RC__allocator_alloc0(RC__Allocator *self, size_t n_bytes)
{
    void *ret = RC__allocator_alloc(self, n_bytes);
    memset(ret, 0, n_bytes);
    return ret;
}

void RC__allocator_free(RC__Allocator *self, void *ptr)
{
    (*self->free)(ptr);
}

void RC__allocator_init(RC__Allocator *self)
{
    self->free  = NULL;
    self->alloc = NULL;
}

void RC__allocator_set_alloc(RC__Allocator *self, RC__AllocCallback alloc)
{
    self->alloc = alloc;
}

void RC__allocator_set_free(RC__Allocator *self, RC__FreeCallback free)
{
    self->free = free;
}

void RC__allocator_destroy(RC__Allocator *self)
{
    (void)self;
}

RC__pathchar *RC__allocator_pathdup(RC__Allocator *self, const RC__pathchar *str)
{
    return (char *)RC__allocator_memdup(self, (void*)str, (RC__pathlen(str) + 1) * sizeof(RC__pathchar));
}

char *RC__allocator_strdup(RC__Allocator *self, const char *str)
{
    if (!str) return NULL;
    size_t len = strlen(str);
    return (char *)RC__allocator_memdup(self, (void*)str, len+1);
}

void *RC__allocator_memdup(RC__Allocator *self, const void *ptr, size_t n_bytes)
{
    void *out = RC__allocator_alloc(self, n_bytes);
    if (out)
        memcpy(out, ptr, n_bytes);
    return out;
}

char *RC__allocator_sprintf(RC__Allocator *self, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    int len;
    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf)-1, fmt, ap);
    if (len < (int)sizeof(buf)-1)
    {
        va_end(ap);
        return (char *)RC__allocator_memdup(self, buf, len+1);
    }
    else
    {
        /* do it again */
        char *realbuf = RC__allocator_alloc(self, len + 1);
        va_start(ap, fmt);
        vsnprintf(realbuf, len + 1, fmt, ap);
        va_end(ap);
        return realbuf;
    }
}

static void *RC__allocator_malloc_impl(size_t n_bytes)
{
    return malloc(n_bytes);
}

static void RC__allocator_free_impl(void *ptr)
{
    free(ptr);
}

RC__Allocator RC__malloc_allocator = { RC__allocator_malloc_impl, RC__allocator_free_impl };

