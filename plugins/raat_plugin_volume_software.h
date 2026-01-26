//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_PLUGIN_VOLUME_SOFTWARE_H
#define INCLUDED_RAAT_PLUGIN_VOLUME_SOFTWARE_H

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

/** \defgroup raat_plugin_volume_software Software Volume Plugin
 *
 *  \brief
 *  This is an implementation of #RAAT__VolumePlugin plugin that uses DSP to perform volume attenuation of PCM signals. See #RAAT__VolumePlugin for more information on volume plugins in general.
 *
 *  This plugin uses noise-shaped (TPDF) dither to maintain transparency. 
 *
 *  DSD volume attenuation is not supported this time, but the plugin interface is designed in a way that allows for it to be supported in the future. 
 *
 *  For best results, output plugins integrating with software volume controls should expand the bit-depth of the PCM data to the 
 *  largest size supported by the DAC to maintain as much headroom as possible when reducing volume. 
 *
 *  If your device has a more suitable implementation of volume DSP that you'd rather use, consider duplicating this plugin implementation 
 *  and replacing the DSP in the softvol_process function with your own.
 *
 *  #RAAT__software_volume_plugin_new takes a JSON object with configuration information. These are the supported parameters:
 *
 *  <pre>
 *      { 
 *          // optional, maximum attenuation that will be applied to the signal (in dB). Defaults to -60.
 *          "max_attenuation": -60,            
 *      }
 *  </pre>
 *
 *  @{
 */

/**
 * Instantiate the software volume plugin.
 *
 * \param alloc the allocator to use for this plugin
 * \param device the #RAAT__Device instance that this plugin will be attached to
 * \param config the configuration data for this plugin
 * \param out_volume an output parameter that will receive the instantiated plugin.
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status
RAAT__software_volume_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__VolumePlugin **out_volume);

/**
 * Delete the software volume plugin
 *
 * \param volume the plugin pointer to delete
 */
void
RAAT__software_volume_plugin_delete(RAAT__VolumePlugin *volume);

/** $} */

#ifdef __cplusplus
}
#endif

#endif
