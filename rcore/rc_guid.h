//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RC_GUID_H
#define INCLUDED_RC_GUID_H


#include "rc_base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RC__GUID_STRLEN 36
#define RC__GUID_COMPACT_STRLEN 32

typedef struct {
    unsigned int   a;
    unsigned short b,c;
    unsigned char  d,e,f,g,h,i,j,k;
} RC__Guid;

/*
 * Functions
 */
void
RC__guid_init                                          (/* out */ RC__Guid *self,
                                                         int a,
                                                         short b,
                                                         short c,
                                                         unsigned char d,
                                                         unsigned char e,
                                                         unsigned char f,
                                                         unsigned char g,
                                                         unsigned char h,
                                                         unsigned char i,
                                                         unsigned char j,
                                                         unsigned char k);

void
RC__guid_init_random                                   (/* out */ RC__Guid *self);

void
RC__guid_init_bytes                                    (/* out */ RC__Guid *self,
                                                         void *buf /* 16 bytes of guid in network byte order */);

/* returns non-0 on error */
int
RC__guid_init_string                                   (/* out */ RC__Guid *self,
                                                         const char *str);

void
RC__guid_init_serial_string                            (RC__Guid *self,
                                                         const char *str);

void
RC__guid_tobytes                                      (RC__Guid *self,
                                                         /* out */ void *buf /* this is filled in with 16 bytes of guid in network byte order */);

void
RC__guid_tostring                                      (RC__Guid *self,
                                                         /* out */ char *buf/*[RC__GUID_STRLEN + 1]*/);

void
RC__guid_tostring_compact                              (RC__Guid *self,
                                                         /* out */ char *buf/*[RC__GUID_COMPACT_STRLEN + 1]*/);

void
RC__guid_serial_tostring                               (RC__Guid *self,
                                                         /* out */ char *buf/*[RC__GUID_STRLEN + 1]*/);

bool
RC__guid_equals                                        (RC__Guid *self,
                                                         RC__Guid *other);

#ifdef __cplusplus
}
#endif

#endif
