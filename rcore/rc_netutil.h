//
// The contents of this file are subject to RC SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RC_NETUTIL_H
#define INCLUDED_RC_NETUTIL_H

#include "rc_base.h"
#include "rc_allocator.h"
#include "rc_status.h"

#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

RC__Status
RC__get_arp_ips(RC__Allocator *alloc, uint32_t **out_addrs, int *out_count);

#define RC__MAX_ADDR_LEN (128)

void 
RC__sockaddr_to_string(const void *sockaddr, char *buf/*[RC__MAX_ADDR_LEN]*/);

typedef void (*RC__NetworkStatusCallback)(void*);

typedef struct RC__NetworkStatus RC__NetworkStatus;

RC__NetworkStatus *
RC__networkstatus_begin_watch(RC__Allocator *alloc, uv_loop_t *loop, RC__NetworkStatusCallback cb, void *userdata);

void
RC__networkstatus_end_watch(RC__NetworkStatus *self);

#ifdef __cplusplus
}
#endif

#endif
