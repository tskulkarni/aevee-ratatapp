//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_WATCH_PLUGIN_ALSA_H
#define INCLUDED_RAAT_WATCH_PLUGIN_ALSA_H

#include "rc_base.h"
#include "rc_allocator.h"
#include "rc_status.h"
#include "raat_fwd.h"
#include "raat_device.h"
#include "raat_plugin_watch.h"

#include <jansson.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup raat_plugin_watch_alsa ALSA Watch Plugin
 *
 *  \brief
 *  This is an implementation of an watch plugin based on ALSA. See #RAAT__WatchPlugin for more information on watch plugins in general.
 *
 *  #RAAT__alsa_watch_plugin_new takes a JSON object with configuration information. These are the supported parameters:
 *
 *  <pre>
 *      { 
 *          // required. ALSA card id for the watch
 *          "device":            "hw:CARD=P20" | "hw:0",
 *
 *          // if lost_action is none, then the watch plugin will simply trace events to the log. 
 *          // If lost_action is exit, then the watch plugin will exit(0)
 *          "lost_action":       "exit" | "none"                
 *      }
 *  </pre>
 *
 *  @{
 */

/** Instantiate an ALSA watch plugin
 *
 * \param alloc the allocator to use. Use RC__ALLOCATOR_DEFAULT.
 * \param device the #RAAT__Device that this plugin is going to be associated with
 * \param config a JSON configuration object.
 * \param out_watch an watch parameter that will receive the instantiated plugin
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status 
RAAT__alsa_watch_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__WatchPlugin **out_watch);

/** Delete an ALSA watch plugin 
 *
 * \param watch the plugin instance, as returned by #RAAT__alsa_watch_plugin_new
 */
void
RAAT__alsa_watch_plugin_delete(RAAT__WatchPlugin *watch);

 /** @} */

#ifdef __cplusplus
}
#endif

#endif
