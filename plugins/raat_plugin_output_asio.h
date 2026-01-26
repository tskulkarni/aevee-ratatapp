//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_OUTPUT_PLUGIN_ASIO_H
#define INCLUDED_RAAT_OUTPUT_PLUGIN_ASIO_H

#include "rc_base.h"
#include "rc_allocator.h"
#include "rc_status.h"
#include "raat_fwd.h"
#include "raat_device.h"
#include "raat_plugin_output.h"

#include <jansson.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup raat_plugin_output_asio ASIO Output Plugin
 *
 *  \brief
 *  This is an implementation of an output plugin based on ASIO. See #RAAT__OutputPlugin for more information on output plugins in general.
 *
 *  #RAAT__asio_output_plugin_new takes a JSON object with configuration information. These are the supported parameters:
 *
 *  <pre>
 *      { 
 *          // required. ASIO device id for the output
 *          "device":                   "hw:CARD=P20,DEV=0", 
 *
 *          // optional, required if the output supports DSD
 *          "dsd_mode":                 "dop" | "dcs",       
 *
 *          // optional, required if if the output supports DSD
 *          "max_dsd_rate":             64 |128 | 256,       
 *
 *          // optional, ASIO buffer duration in seconds. If not provided, defaults to 0.04s
 *          "buffer_duration":          0.04,                
 *
 *          // optional, tells RAAT to play a pad of silence at the beginning of each stream. If not provided, defaults to 0.1.
 *          "resync_delay":             0.5,                 
 *
 *          // optional, allows for customization of the signal path. If not provided, a default implementation will be used.
 *          "signal_path": [                                 
 *               // you can add conditions to signal path elements if parts of your signal path are switched on and off based on stream parameters
 *               // supported fields are "sample_rate", "bits_per_sample", "channels". See the Signal Path documentation for more information 
 *               // on supported signal path elements
 *               { "quality": "high",     "type": "dsd_to_pcm",                 "from_sample_rate": 123, "to_sample_rate": 246 "condition": { "sample_rate": 44100 } }
 *               { "quality": "high",     "type": "pcm_sample_rate_conversion", "from_sample_rate": 123, "to_sample_rate": 246  }
 *               { "quality": "lossless", "type": "output",                     "method": "other" }
 *          ],
 *
 *          // optional. This is merged into the object in from the output plugin's get_info method
 *          "info": {                               
 *              ...
 *          }
 *  </pre>
 *
 *  @{
 */

/** Instantiate an ASIO output plugin
 *
 * \param alloc the allocator to use. Use RC__ALLOCATOR_DEFAULT.
 * \param device the #RAAT__Device that this plugin is going to be associated with
 * \param config a JSON configuration object.
 * \param out_output an output parameter that will receive the instantiated plugin
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status 
RAAT__asio_output_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__OutputPlugin **out_output);


typedef void (*RAAT__AsioOutputPluginLostCallback)(RAAT__OutputPlugin *output, void *userdata);

/** Set a lost callback
 */
void
RAAT__asio_output_plugin_set_lost_cb(RAAT__OutputPlugin *output, RAAT__AsioOutputPluginLostCallback cb, void *userdata);


/** Delete an ASIO output plugin 
 *
 * \param output the plugin instance, as returned by #RAAT__asio_output_plugin_new
 */
void
RAAT__asio_output_plugin_delete(RAAT__OutputPlugin *output);

 /** @} */

#ifdef __cplusplus
}
#endif

#endif
