//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RC_BASE_H
#define INCLUDED_RC_BASE_H

// NOTE: these might need emulation on MSVC
#include <stdint.h>
#include <stdbool.h>

#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <time.h>


#if _WIN32
#include <winsock2.h>
#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#define alloca _alloca
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define sleep(x) Sleep(1000 * x)
#define RC__pathchar wchar_t
#define RC__pathlen wcslen
#define RC__pathsepc    L'\\'
#define RC__pathsep    L"\\"
#define RC__pathcat wcscat
#define RC__pathcat_cstring ERRROORRRRR
#define RC__pathunlink DeleteFileW
#define RC__PATHFORMAT "%ls"
static inline void RC__pathcat_cstring(wchar_t *path, const char *to_append) {
    wchar_t to_append_wide[32768];
    mbstowcs(to_append_wide, to_append, 32768);
    RC__pathcat(path, to_append_wide);
}

static inline FILE *RC__pathopen(const wchar_t *path, const char *mode) {
    wchar_t wmode[32] = {0,};
    mbstowcs(wmode, mode, 32);
    return _wfopen(path, wmode);
}
#else
#include <alloca.h>
#include <unistd.h>
#define RC__pathchar char
#define RC__pathlen  strlen
#define RC__pathsepc    '/'
#define RC__pathsep "/"
#define RC__pathcat strcat
#define RC__pathcat_cstring strcat
#define RC__pathopen fopen
#define RC__pathunlink unlink
#define RC__PATHFORMAT "%s"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup rc_base Base 
 *
 *  \brief
 *
 *  Utility functions
 *
 *  @{
 */

#if PLATFORM_WINDOWS || PLATFORM_MACOSX
#define RC__IS_LITTLE_ENDIAN (1)
#define inline __inline
#else
/** Macro that is true if the current platform is little-endian, and false otherwise */
#define RC__IS_LITTLE_ENDIAN (((union { unsigned x; unsigned char c; }){1}).c)
#endif

#ifdef PLATFORM_WINDOWS

#pragma warning (disable:4244)
int vasprintf(char ** ret, const char * format, va_list ap);
#define RC_API __declspec(dllexport)
#else
#define RC_API 

#endif

/** 
 * Suppress crash dialogs 
 */
RC_API void RC__suppress_crash_dialogs(void);

/** Determine the current system time, in microseconds
 *
 * This should use a high-resolution monotonic clock. 
 *
 * \retval clock value in microseconds.
 */
int64_t RC__now_us(void);

/** Determine the current system time, in nanoseconds
 *
 * This should use a high-resolution monotonic clock. 
 *
 * \retval clock value in nanoseconds.
 */
int64_t RC__now_ns(void);

/** Sleep for the specified number of microseconds
 *
 * \retval clock value in nanoseconds.
 */
void RC__usleep(int interval);

/** Macro to compute the maximum of two values */
#define RC__max(a,b)     (((a) > (b)) ? (a) : (b))
/** Macro to compute the minimum of two values */
#define RC__min(a,b)     (((a) < (b)) ? (a) : (b))

#define RC__ASSERT assert

#ifdef ARCH_X64
/** Pack an integer value into a pointer value */
#define RC__INT_TO_POINTER(i) ((void*)(long long)(i))
/** Unpack a pointer value into an integer value */
#define RC__POINTER_TO_INT(p) ((int)(long long)(p))
/** Pack an unsigned integer value into a pointer value */
#define RC__UINT_TO_POINTER(i) ((void*)(unsigned long long)(i))
/** Unpack a pointer value into an unsigned integer value */
#define RC__POINTER_TO_UINT(p) ((unsigned int)(unsigned long long)(p))
#else
/** Pack an integer value into a pointer value */
#define RC__INT_TO_POINTER(i) ((void*)(i))
/** Unpack a pointer value into an integer value */
#define RC__POINTER_TO_INT(p) ((int)(p))
/** Pack an unsigned integer value into a pointer value */
#define RC__UINT_TO_POINTER(i) ((void*)(i))
/** Unpack a pointer value into an unsigned integer value */
#define RC__POINTER_TO_UINT(p) ((unsigned int)(p))
#endif

/** Flip endianness of an array values 
 *
 * \param p pointer to the values
 * \param valsize size of each value in bytes
 * \param n number of values
 */
static inline void RC__flip_endian_array(void *p, size_t valsize, size_t n) {
    uint8_t *c = (uint8_t*)p;
    size_t i,j;
    for (j = 0; j < n; j++) {
        for (i = 0; i < valsize / 2; i++) {
            uint8_t tmp = c[i];
            c[i]        = c[valsize-i-1];
            c[valsize-i-1]    = tmp;
        }
        c += valsize;
    }
}

/** Flip endianness of a value 
 *
 * \param p pointer to the value
 * \param valsize size of the value in bytes
 *
 * */
static inline void RC__flip_endian(void *p, size_t valsize) {
    uint8_t *c = (uint8_t*)p;
    size_t i;
    for (i = 0; i < valsize / 2; i++) {
        uint8_t tmp = c[i];
        c[i]        = c[valsize-i-1];
        c[valsize-i-1]    = tmp;
    }
}

/** Convert an array of n native-endian values, valsize bytes each into big-endian 
 *
 * \param p pointer to the values
 * \param valsize size of each value in bytes
 * \param n number of values
 *
 */
static inline void RC__big_endian_array(void *p, size_t valsize, size_t n) {
    if (RC__IS_LITTLE_ENDIAN) RC__flip_endian_array(p, valsize, n);
}
/** Convert an array of n native-endian values, valsize bytes each into little-endian 
 *
 * \param p pointer to the values
 * \param valsize size of each value in bytes
 * \param n number of values
 *
 */
static inline void RC__little_endian_array(void *p, size_t valsize, size_t n) {
    if (!RC__IS_LITTLE_ENDIAN) RC__flip_endian_array(p, valsize, n);
}

/** Convert a native-endian value of size valsize bytes into big-endian 
 *
 * \param p pointer to the values
 * \param valsize size of the value in bytes
 *
 */
static inline void RC__big_endian(void *p, size_t valsize) {
    if (RC__IS_LITTLE_ENDIAN) RC__flip_endian(p, valsize);
}

/** Convert a native-endian value of size valsize bytes into little-endian 
 *
 * \param p pointer to the values
 * \param valsize size of the value in bytes
 *
 */
static inline void RC__little_endian(void *p, size_t valsize) {
    if (!RC__IS_LITTLE_ENDIAN) RC__flip_endian(p, valsize);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif
