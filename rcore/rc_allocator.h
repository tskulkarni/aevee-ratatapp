//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RC_ALLOCATOR_H
#define INCLUDED_RC_ALLOCATOR_H

#include <stdlib.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup rc_allocator Memory Allocation
 *
 *  \brief
 *  This module provides an abstraction over memory allocation. Most RAAT functionality is 
 *  built on top of this allocator system.
 *
 *  @{
 */

#include "rc_base.h"


/** A constant that can be used to grab the default allocator. Implemented on top of malloc() and free() */
#define RC__ALLOCATOR_DEFAULT (RC__allocator_malloc())

/** The allocation implementation */
typedef void *(*RC__AllocCallback)(size_t n_bytes);
/** The free implementation */
typedef void (*RC__FreeCallback)(void *ptr);

/** Data structure that represents an allocator */
typedef struct RC__allocator {
    RC__AllocCallback alloc;
    RC__FreeCallback  free;
} RC__Allocator;

/** \internal */
void RC__allocator_init(RC__Allocator *self);
/** \internal */
void RC__allocator_destroy(RC__Allocator *self); 
/** \internal */
void RC__allocator_set_alloc(RC__Allocator *self, RC__AllocCallback alloc);
/** \internal */
void RC__allocator_set_free(RC__Allocator *self, RC__FreeCallback free);

/** Resize a memory region 
 *
 * \param self the allocator
 * \param ptr a pointer previously returned from an allocation function
 * \param old_n_bytes the number of bytes of data that should be preserved, starting from ptr
 * \param new_n_bytes the number of bytes of the newly allocated region
 *
 * \retval the newly allocated memory or NULL if memory allocation failed
 * */
void *RC__allocator_resize(RC__Allocator *self, void *ptr, size_t old_n_bytes, size_t new_n_bytes);

/** Resize a memory region, zeroing any newly allocated bytes 
 *
 * \param self the allocator
 * \param ptr a pointer previously returned from an allocation function
 * \param old_n_bytes the number of bytes of data that should be preserved, starting from ptr
 * \param new_n_bytes the number of bytes of the newly allocated region
 *
 * \retval the newly allocated memory or NULL if memory allocation failed
 */
void *RC__allocator_resize0(RC__Allocator *self, void *ptr, size_t old_n_bytes, size_t new_n_bytes);

/** Allocate memory 
 *
 * \param self the allocator
 * \param n_bytes the number of bytes to allocate
 *
 * \retval the newly allocated memory or NULL if memory allocation failed
 */
void *RC__allocator_alloc(RC__Allocator *self, size_t n_bytes);

/** Allocate zeroed memory 
 *
 * \param self the allocator
 * \param n_bytes the number of bytes to allocate
 *
 * \retval the newly allocated memory or NULL if memory allocation failed
 * */
void *RC__allocator_alloc0(RC__Allocator *self, size_t n_bytes);
/** Free previously allocated memory 
 *
 * \param self the allocator
 * \param ptr the memory to free, or NULL
 * */
void RC__allocator_free(RC__Allocator *self, void *ptr);

/** 
 * Duplicate a null terminated string using an allocator
 *
 * \param self the allocator
 * \param s the string to duplicate
 *
 * \retval the duplicated string, or NULL if allocation failed
 */
char *RC__allocator_strdup(RC__Allocator *self, const char *s);

/** 
 * Duplicate a null terminated path string (RC__pathchar*) using an allocator
 *
 * \param self the allocator
 * \param s the string to duplicate
 *
 * \retval the duplicated string, or NULL if allocation failed
 */
RC__pathchar *RC__allocator_pathdup(RC__Allocator *self, const RC__pathchar *s);

/**
 * Duplicate a memory region using an allocator
 *
 * \param self the allocator
 * \param ptr pointer to the beginning of the memory region 
 * \param n_bytes the number of bytes to duplicate
 *
 * \retval the duplicated memory region, or NULL if allocation failed
 */
void *RC__allocator_memdup(RC__Allocator *self, const void *ptr, size_t n_bytes);

/**
 * Use printf-style formatting to populate a newly allocated memory region
 *
 * \param self the allocator
 * \param fmt format string
 * \param ... printf style arguments
 *
 * \retval a NULL-terminated C-string formed from the fmt and ... arguments, or NULL if allocation failed
 */
char *RC__allocator_sprintf(RC__Allocator *self, const char *fmt, ...);

/**
 * Returns a pointer to the malloc allocator, an RC__Allocator implemented on top of the standard C library's malloc and free functions.
 */
static inline RC__Allocator *RC__allocator_malloc();

/** \internal */
static inline RC__Allocator *RC__allocator_default(RC__Allocator *allocator) {
    if (allocator == NULL)
        return RC__allocator_malloc();
    else
        return allocator;
}

/** \internal */
static inline RC__Allocator *RC__allocator_malloc() {
    extern RC__Allocator RC__malloc_allocator;
    return &RC__malloc_allocator;
}

/** Helper macro for allocating memory based on a C type
 *
 * \param alloc the allocator
 * \param typ   a C type
 * \param cnt   the number of elements to allocate
 *
 * \retval a memory region of size sizeof(typ)*cnt or NULL if allocation failed
 */
#define RC__new(alloc, typ, cnt) \
    ((typ*)RC__allocator_alloc(alloc, sizeof(typ)*(cnt)))

/** Helper macro for allocating zeroed memory based on a C type
 *
 * \param alloc the allocator
 * \param typ   a C type
 * \param cnt   the number of elements to allocate
 *
 * \retval a memory region of size sizeof(typ)*cnt or NULL if allocation failed
 */
#define RC__new0(alloc, typ, cnt) \
    ((typ*)RC__allocator_alloc0(alloc, sizeof(typ)*(cnt)))

/** Shorthand for #RC__allocator_alloc
 */
#define RC__alloc(alloc, size) \
    (RC__allocator_alloc(alloc, size))

/** Shorthand for #RC__allocator_resize
 */
#define RC__resize(alloc, ptr, old_size, new_size) \
    (RC__allocator_resize(alloc, ptr, old_size, new_size))

/** Shorthand for #RC__allocator_resize0
 */
#define RC__resize0(alloc, ptr, old_size, new_size) \
    (RC__allocator_resize0(alloc, ptr, old_size, new_size))

/** Shorthand for #RC__allocator_alloc0
 */
#define RC__alloc0(alloc, size) \
    (RC__allocator_alloc0(alloc, size))

#define RC__free(alloc, ptr) (RC__allocator_free(alloc, (void*)ptr));

 /** @} */

#ifdef __cplusplus
}
#endif

#endif
