//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_PLUGIN_VOLUME_ALSA_H
#define INCLUDED_RAAT_PLUGIN_VOLUME_ALSA_H

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

/** \defgroup raat_plugin_volume_alsa ALSA Volume Plugin
 *
 *  This is an implementation of an volume plugin based on ALSA. See #RAAT__VolumePlugin for more information on volume plugins in general.
 *
 *  #RAAT__alsa_volume_plugin_new takes a JSON object with configuration information. These are the supported parameters:
 *
 *  If both are provided, then RAAT uses the exact ALSA mixer element specified by the index/name pair.
 *  If only index is provided, then RAAT uses the first playback volume control on the device that matches the index.
 *  If neither are provided, then RAAT uses the first playback volume control on the device, regardless of the index.
 *
 * <pre>
 *      { 
 *          // required. ALSA card id for the ALSA mixer control
 *          "device":                   "hw:CARD=P20", 
 *
 *          //
 *          // optional. ALSA device index for the ALSA mixer control. If omitted, RAAT will make an educated guess
 *          //
 *          "index":  0,
 *
 *          //
 *          // optional. Name of mixer element to manipulate. If omitted, RAAT will make an educated guess.
 *          //
 *          "name":  "PS Audio  Clock Selector",
 *
 *          //
 *          // optional. If mode is 'db' then volume scale will be in decibels instead of 0-100.
 *          //           Unlike 0-100, this doesn't work well for all devices, since some misreport 
 *          //           their characteristics.
 *          "mode": "db" | "number",
 *
 *          //
 *          // optional, for use with "db" mode. If ALSA exposes too wide a range for this device, this lets you constrain the range.
 *          //
 *          "db_min":  -100.0,
 *          "db_max":  0.0,
 *
 *          //
 *          // optional, for use with "db" mode. If your db supports step size smaller than 1.0, report it here to enable fractional behavior in the app
 *          // 
 *          "db_step": 0.5,
 *
 *          //
 *          // optional. This is merged into the object in from the volume plugin's get_info method
 *          //
 *          "info": {                               
 *              ...
 *          }
 *     }
 * </pre>
 *
 *  @{
 */

/** Instantiate an ALSA volume plugin
 *
 * \param alloc the allocator to use. Use RC__ALLOCATOR_DEFAULT.
 * \param device the #RAAT__Device that this plugin is going to be associated with
 * \param config a JSON configuration object.
 * \param out_volume an volume parameter that will receive the instantiated plugin
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status
RAAT__alsa_volume_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__VolumePlugin **out_volume);

/** Delete an ALSA volume plugin 
 *
 * \param volume the plugin instance, as returned by #RAAT__alsa_volume_plugin_new
 */
void
RAAT__alsa_volume_plugin_delete(RAAT__VolumePlugin *volume);

 /** @} */

#ifdef __cplusplus
}
#endif

#endif
