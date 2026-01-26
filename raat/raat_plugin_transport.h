//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_PLUGIN_TRANSPORT_H
#define INCLUDED_RAAT_PLUGIN_TRANSPORT_H

#include "rc_base.h"
#include "rc_allocator.h"
#include "rc_status.h"
#include "raat_log.h"
#include "rc_list.h"

#include <jansson.h>
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup raat_plugin_transport Transport Plugin
 *
 *  \brief
 *  The transport plugin lets RAAT integrate with physical transport controls and front-panel displays present on a hardware device.
 *
 *  @{
 */

/** Callback used to communicate transport controls back to Roon
 *
 * \param cb_userdata the opaque data provided in add_control_listener
 * \param control a control formatted in JSON
 *
 * Currently supported controls include:
 *
 * <pre>
 * { "button": "play" }
 * { "button": "pause" }
 * { "button": "playpause" }
 * { "button": "playpause" }
 * { "button": "next" }
 * { "button": "previous" }
 * { "button": "toggleshuffle" }
 * { "button": "toggleloop" }
 * { "seek": { "mode": "absolute", "seconds": seconds } }
 * { "seek": { "mode": "relative", "seconds": seconds } }
 * </pre>
 *
 * If your device can produce other controls, and you think integrating it into Roon makes sense, reach out 
 * to your contact at Roon to discuss.
 */
typedef void (*RAAT__TransportControlCallback)(void *cb_userdata, json_t *control);

/**
 * A virtual-function table that can be used to integrate with physical transport controls and front panel displays.
 *
 * Implementing this interface is required if your device has transport controls or a front-panel that is used 
 * to display now-playing displays in other situations.
 *
 * If you have any questions, please reach out to your Roon contact.
 */
typedef struct {
    /**
     * Return plugin-defined information to Roon.
     *
     * This information is expressed in a free-form JSON object.
     *
     * Configuration parameters and/or hardware information that might be useful for user display, troubleshooting, or to drive
     * app behavior should be exposed here. If in doubt about how to expose something, please reach to to your Roon contact.
     *
     * This information is immutable. Roon will get the info once after establishing a connection to a RAAT device, and then 
     * store it for the duration of the session. 
     *
     * \param self the output plugin instance
     * \param out_info pointer to a json_t that will receive the info object. This must be a JSON object.
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*get_info)(void *self, json_t **out_info);

    /**
     * Register a control listener with the transport plugin
     *
     * Use the #RAAT__TransportControlListeners helper structure + its associated functions to implement this function (see sample code).
     *
     * \param self the source selection plugin
     * \param cb a callback that will receive controls emitted by this transport plugin
     * \param cb_userdata an opaque pointer-sized value that will be passed to <tt>cb</tt>
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*add_control_listener)(void *self, RAAT__TransportControlCallback cb, void *cb_userdata);

    /**
     * Unregister a control listener with the transport plugin.
     *
     * \param self the transport plugin
     * \param cb the callback to be removed
     * \param cb_userdata the cb_userdata that goes with the callback
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*remove_control_listener)(void *self, RAAT__TransportControlCallback cb, void *cb_userdata);

    /**
     * Update current zone status + nowplaying information. 
     *
     * Implementing this function is optional. Leave this function pointer NULL if this device doesn't support status display.
     *
     * ### Status JSON format
     * <pre>
     * { 
     *     "loop":    "disabled" | "loop" | "loopone",
     *     "shuffle": true | false,
     *     "state":   "playing" | "loading" | "paused" | "stopped",
     *     "seek":    seek position | null,
     *
     *     "is_previous_allowed": true | false,        NOTE: is_*_allowed were introduced in Roon 1.3
     *     "is_next_allowed":     true | false,        NOTE: is_*_allowed were introduced in Roon 1.3
     *     "is_play_allowed":     true | false,        NOTE: is_*_allowed were introduced in Roon 1.3
     *     "is_pause_allowed":    true | false,        NOTE: is_*_allowed were introduced in Roon 1.3
     *     "is_seek_allowed":     true | false,        NOTE: is_*_allowed were introduced in Roon 1.3
     *
     *     "now_playing": {                                            NOTE: this field is be omitted if nothing is playing
     *         "one_line":      "text for single line displays",
     *
     *         "two_line_title":    "title for two line displays",
     *         "two_line_subtitle": "subtitle for two line displays"
     *
     *         "three_line_title":       "title for three line displays" | null,        NOTE three_line_* were introduced in Roon 1.2
     *         "three_line_subtitle":    "subtitle for three line displays" | null,
     *         "three_line_subsubtitle": "subsubtitle for three line displays" | null,
     *
     *         "length":        length | null,
     *
     *         "title":         NOTE: THIS IS DEPRECATED. DO NOT USE. YOU WILL FAIL CERTIFICATION.
     *         "album":         NOTE: THIS IS DEPRECATED. DO NOT USE. YOU WILL FAIL CERTIFICATION.
     *         "channel":       NOTE: THIS IS DEPRECATED. DO NOT USE. YOU WILL FAIL CERTIFICATION.
     *         "artist":        NOTE: THIS IS DEPRECATED. DO NOT USE. YOU WILL FAIL CERTIFICATION.
     *         "composer":      NOTE: THIS IS DEPRECATED. DO NOT USE. YOU WILL FAIL CERTIFICATION.
     *     }
     *     "stream_format": {                                          NOTE: this field is optional. please behave gracefully if it is not present. 
     *          "sample_type":     "dsd" | "pcm",                      NOTE: This information is for display purposes only. Do not allow it to influence audio playback.
     *          "sample_rate":     44100 | 48000 | ...,
     *          "bits_per_sample": 1 | 16 | 24 | 32,
     *          "channels":        1, 2, ...
     *     }
     * }           
     * </pre>
     *
     * \param self the transport plugin
     * \param status status information in JSON
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*update_status)(void *self, json_t *status);

    /**
     * Update artwork for the now-playing item.
     *
     * Implementing this function is optional. Leave this function pointer NULL if this device doesn't support artwork display.
     *
     * \param self the transport plugin
     * \param mime_type null-terminated string containing <tt>image/jpeg</tt>, <tt>image/png</tt>, or NULL if there is no image.
     * \param data pointer to image data, or NULL if there is no image
     * \param data_len number of bytes of image data
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*update_artwork)(void *self, const char *mime_type, void *data, size_t data_len);

} RAAT__TransportPlugin;

/**
 * Helper for managing transport control listeners
 */
typedef struct {
    RC__Allocator      *alloc;
    uv_mutex_t          lock;
    RC__List            listeners;
} RAAT__TransportControlListeners;

/**
 * Initialize a collection of transport control listener callbacks
 *
 * \param self a pointer to a #RAAT__TransportControlListeners 
 * \param alloc memory allocator to use for keeping track of the list of listeners
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status 
RAAT__transport_control_listeners_init               (RAAT__TransportControlListeners    *self, 
                                                      RC__Allocator                      *alloc);

/**
 * Add a handler to a #RAAT__TransportControlListeners callback list
 *
 * \param self a pointer to a #RAAT__TransportControlListeners 
 * \param cb the callback to add
 * \param userdata an opaque pointer-sized value that will be passed to the callback
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status 
RAAT__transport_control_listeners_add                (RAAT__TransportControlListeners    *self, 
                                                      RAAT__TransportControlCallback      cb, 
                                                      void                               *userdata);

/**
 * Add a handler to a #RAAT__TransportControlListeners callback list
 *
 * \param self a pointer to a #RAAT__TransportControlListeners 
 * \param cb the callback to add
 * \param userdata an opaque pointer-sized value that will be passed to the callback
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status 
RAAT__transport_control_listeners_remove             (RAAT__TransportControlListeners    *self, 
                                                      RAAT__TransportControlCallback      cb, 
                                                      void                               *userdata);

/**
 * Invoke each callback in a list of transport control callbacks
 *
 * \param self a pointer to a #RAAT__TransportControlListeners 
 * \param control a JSON message that will be passed to each callback
 */
RC__Status 
RAAT__transport_control_listeners_invoke             (RAAT__TransportControlListeners    *self, 
                                                      json_t                             *control);

/**
 * Destroy a set of transport control callbacks
 *
 * \param self a pointer to a #RAAT__TransportControlListeners 
 */
void 
RAAT__transport_control_listeners_destroy            (RAAT__TransportControlListeners    *self);

 /** @} */

#ifdef __cplusplus
}
#endif

#endif
