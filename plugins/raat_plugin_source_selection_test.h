//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_PLUGIN_SOURCE_SELECTION_NULL_H
#define INCLUDED_RAAT_PLUGIN_SOURCE_SELECTION_NULL_H

#include "rc_base.h"
#include "rc_allocator.h"
#include "rc_status.h"
#include "raat_fwd.h"
#include "raat_device.h"
#include "raat_plugin_source_selection.h"
#include "raat_plugin_output.h"

#include <jansson.h>

#ifdef __cplusplus
extern "C" {
#endif

RC__Status
RAAT__test_source_selection_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__SourceSelectionPlugin **out_source_selection); 
void
RAAT__test_source_selection_plugin_delete(RAAT__SourceSelectionPlugin *out_source_selection);

#ifdef __cplusplus
}
#endif

#endif
