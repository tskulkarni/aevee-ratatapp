//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_OUTPUT_PLUGIN_ALSA_H
#define INCLUDED_RAAT_OUTPUT_PLUGIN_ALSA_H

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

/** \defgroup raat_plugin_output_alsa ALSA Output Plugin
 *
 *  \brief
 *  This is an implementation of an output plugin based on ALSA. See #RAAT__OutputPlugin for more information on output plugins in general.
 *
 *  #RAAT__alsa_output_plugin_new takes a JSON object with configuration information. These are the supported parameters:
 *
 *  <pre>
 *      { 
 *          // required. ALSA device id for the output
 *          "device":                   "hw:CARD=P20,DEV=0", 
 *
 *          // optional, required if the output supports DSD
 *          "dsd_mode":                 "native" | "dop" | "dcs" | "native_or_dop" | "native_or_dcs" | "none",       
 *
 *          // optional, required if if the output supports DSD
 *          "max_dsd_rate":             64 |128 | 256,       
 *
 *          // optional, ALSA buffer duration in seconds. If not provided, defaults to 0.04s
 *          "buffer_duration":          0.04,                
 *
 *          // optional, tells RAAT to play a pad of silence at the beginning of each stream. If not provided, defaults to 0.1.
 *          "resync_delay":             0.5,                 
 *
 *          // optional, tells RAAT to not grab the USB device IDs on the output. This will hide the USB device connected to the ALSA output.
 *          "disable_usb_hw_info":      true,
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
 *
 *          // optional, and only for special situations where an ALSA device with fixed format support that can't report accurately on its own format capabilities at RAAT startup.
 *          // This is NOT to be used in conjunction with external USB devices. If you're not sure whether using this property is appropriate, please talk to Roon first.
 *          //
 *          // Some rules:
 *          // - All devices must support at least 44100/16/1, 44100/16/2, 48000/16/1, 48000/16/2
 *          // - If you support a format in STEREO you must also support it in MONO
 *          //
 *          "supported_formats": [
 *              { "sample_type": "pcm", "sample_rate": 44100, "bits_per_sample": 16, "channels": 1 },
 *              { "sample_type": "pcm", "sample_rate": 48000, "bits_per_sample": 16, "channels": 1 },
 *              ...
 *              { "sample_type": "pcm", "sample_rate": 44100, "bits_per_sample": 16, "channels": 2 },
 *              { "sample_type": "pcm", "sample_rate": 48000, "bits_per_sample": 16, "channels": 2 },
 *              ...
 *              { "sample_type": "dsd", "sample_rate": 2822400, "bits_per_sample": 1, "channels": 1 },
 *              { "sample_type": "dsd", "sample_rate": 5644800, "bits_per_sample": 1, "channels": 1 },
 *              ...
 *              { "sample_type": "dsd", "sample_rate": 2822400, "bits_per_sample": 1, "channels": 2 },
 *              { "sample_type": "dsd", "sample_rate": 5644800, "bits_per_sample": 1, "channels": 2 },
 *              ...
 *          ],
 *
 *          // optional, and only for special situations where RAAT must be allowed to start up with no ALSA device present.
 *          // This is NOT to be used in conjunction with external USB devices. If you're not sure whether using this property is appropriate, please talk to Roon first.
 *          "skip_startup_device_check": true
 *
 *          // optional, adjust synchronization for each sample rate to compensate for hardware characteristics. Offsets are
 *          // measured in nanoseconds. Values larger than zero correspond to increased output delay. 
 *          //
 *          // the first match in the list wins, so if you have MQA formats to account for, make sure they come first
 *          "sync_offset_ns": [
 *              { "sample_rate": 44100, "offset": 100, "sample_subtype": "mqa" },
 *              { "sample_rate": 88200, "offset": 200, "sample_subtype": "mqa_core" },
 *              { "sample_rate": 48000, "offset": 300, "sample_subtype": "mqa" },
 *              { "sample_rate": 96000, "offset": 400, "sample_subtype": "mqa_core" }
 *              { "sample_rate": 44100, "offset": 100 },
 *              { "sample_rate": 48000, "offset": 200 },
 *              { "sample_rate": 88200, "offset": 300 },
 *              { "sample_rate": 96000, "offset": 400 },
 *              { "sample_rate": 192000, "offset": 500 }
 *          ]
 *
 *          OR 
 *
 *          "sync_offset_ns": 12345             // use this variant if all sample rates require the same adjustment
 *
 *  </pre>
 *
 *  @{
 */

/** Instantiate an ALSA output plugin
 *
 * \param alloc the allocator to use. Use RC__ALLOCATOR_DEFAULT.
 * \param device the #RAAT__Device that this plugin is going to be associated with
 * \param config a JSON configuration object.
 * \param out_output an output parameter that will receive the instantiated plugin
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status 
RAAT__alsa_output_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__OutputPlugin **out_output);

/**
 * Update the signal path at runtime. Useful for situations where the signal path can change in real-time
 * and re-starting RAAT is not appropriate.
 *
 * The JSON object should be a list that follows follow the format specified above.
 */
RC__Status 
RAAT__alsa_output_plugin_update_signal_path(RAAT__OutputPlugin *output, json_t *signal_path);

/** Delete an ALSA output plugin 
 *
 * \param output the plugin instance, as returned by #RAAT__alsa_output_plugin_new
 */
void
RAAT__alsa_output_plugin_delete(RAAT__OutputPlugin *output);

 /** @} */

#ifdef __cplusplus
}
#endif

#endif
