//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_PLUGIN_VOLUME_NULL_H
#define INCLUDED_RAAT_PLUGIN_VOLUME_NULL_H

#include "rc_base.h"
#include "rc_allocator.h"
#include "rc_status.h"
#include "raat_fwd.h"
#include "raat_device.h"
#include "raat_plugin_volume.h"

#include <jansson.h>

#ifdef __cplusplus
extern "C" {
#endif

RC__Status
RAAT__null_volume_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__VolumePlugin **out_volume);

void
RAAT__null_volume_plugin_delete(RAAT__VolumePlugin *out_volume);

#ifdef __cplusplus
}
#endif

#endif
