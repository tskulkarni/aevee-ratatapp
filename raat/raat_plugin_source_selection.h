//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_PLUGIN_SOURCE_SELECTION_H
#define INCLUDED_RAAT_PLUGIN_SOURCE_SELECTION_H

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

/** \defgroup raat_plugin_source_selection Source Selection Plugin
 *
 *  \brief
 *  The source selection plugin implements convenience switching for devices with multiple audio sources. See #RAAT__SourceSelectionPlugin.
 *
 *  @{
 */

/** Represents the current state of the source selection plugin */
typedef enum {
    RAAT__SOURCE_SELECTION_STATUS_SELECTED,
    /**< Indicates that RAAT's source is selected */
    RAAT__SOURCE_SELECTION_STATUS_DESELECTED,
    /**< Indicates that RAAT's source is not selected */
    RAAT__SOURCE_SELECTION_STATUS_INDETERMINATE, 
    /**< Indicates that RAAT's source may or may not be selected. This should only be used on devices where source selection is unknowable under some circumstances. */
    RAAT__SOURCE_SELECTION_STATUS_STANDBY,           
    /**< Indicates that RAAT's source may or may not be selected. This should only be used on devices where source selection is unknowable under some circumstances. */
} RAAT__SourceSelectionStatus;

/** Represents state information for the source selection plugin */
typedef struct {
    RAAT__SourceSelectionStatus status;
} RAAT__SourceSelectionState;

// status codes
enum {
    RAAT__SOURCE_SELECTION_PLUGIN_STATUS_SOURCE_NOT_AVAILABLE   = RAAT__SOURCE_SELECTION_PLUGIN_STATUS_BASE + 0,
    RAAT__SOURCE_SELECTION_PLUGIN_STATUS_TIMEOUT                = RAAT__SOURCE_SELECTION_PLUGIN_STATUS_BASE + 1,
};

/** Callback used to reflect source selection state changes 
 *
 * \param cb_userdata opaque pointer-sized value passed to add_state_listener.
 * \param state The current state of the source selection plugin.
 */
typedef void (*RAAT__SourceSelectionStateCallback)(void *cb_userdata, RAAT__SourceSelectionState *state);

/** Callback used to communicate the status of the request_source function.
 *
 * \param cb_userdata opaque pointer-sized value passed to request_source.
 * \param status the status of the request_source operation
 * \param extra_info If source selection fails, this should contain JSON data that describes why it failed in more detail. This might be something like <tt>{ "reason": "airplay_in_use" }</tt>. 
 */
typedef void (*RAAT__SourceSelectionRequestSourceCallback)(void *cb_userdata, RC__Status status, json_t *extra_info);

/** Callback used to communicate the status of the request_standby function.
 *
 * \param cb_userdata opaque pointer-sized value passed to request_standby.
 * \param status the status of the request_standby operation
 * \param extra_info If source selection fails, this should contain JSON data that describes why it failed in more detail. This might be something like <tt>{ "reason": "airplay_in_use" }</tt>. 
 */
typedef void (*RAAT__SourceSelectionRequestStandbyCallback)(void *cb_userdata, RC__Status status, json_t *extra_info);

/**
 * A virtual-function table that can be used to implement convenience switching.
 *
 * When the user presses the "play" button in Roon, the device should be configured so that audio from Roon is heard.
 *
 * Implementing this interface is a requirement for devices that support multiple sources. This comes up for many device classes:
 *
 * - Players that might have other audio sources (iTunes, Spotify Connect, etc).
 * - Receivers that have audio inputs other than Roon
 * - DACs that have multiple inputs (USB, coax, aes/ebu, RAAT).
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
     * Register a state listener with the source selection plugin.
     *
     * Use the #RAAT__SourceSelectionStateListeners helper structure + its associated functions to implement this function (see sample code).
     *
     * \param self the source selection plugin
     * \param cb a callback that will receive state changes emitted by this source selection plugin
     * \param cb_userdata an opaque pointer-sized value that will be passed to <tt>cb</tt>
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*add_state_listener)(void *self, RAAT__SourceSelectionStateCallback cb, void *cb_userdata);

    /**
     * Unregister a state listener with the source selection plugin.
     *
     * \param self the source selection plugin
     * \param cb the callback to be removed
     * \param cb_userdata the cb_userdata that goes with the callback
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*remove_state_listener)(void *self, RAAT__SourceSelectionStateCallback cb, void *cb_userdata);

    /**
     * Get the current state of the sourceselection plugin
     *
     * \param self the source selection plugin
     * \param out_state output parameter that will receive the current source selection state.
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*get_state)(void *self, RAAT__SourceSelectionState *out_state);

    /**
     * Request that the source be switched to Roon.
     *
     * The status of this operation is reported via <tt>cb_result</tt>.
     *
     * Roon calls this as a result of user actions like "play now". The goal is to immediately switch the system
     * in a state where it's ready to do what the Roon user requested. If a different source is selected, the source
     * should be changed so that Roon can play. If the device is in standby, then the source should be taken out of 
     * standby.
     *
     * \param cb_result result callback for the source selection operation
     * \param cb_userdata opaque pointer-sized value that will be passed to cb_result
     */
    void (*request_source)(void *self, RAAT__SourceSelectionRequestSourceCallback cb_result, void *cb_userdata);

    /**
     * Request that the RAAT device enter standby mode
     *
     * The status of this operation is reported via <tt>cb_result</tt>.
     *
     * Note: this method is optional. Not all plugins support it.
     *
     * \param cb_result result callback for the source selection operation
     * \param cb_userdata opaque pointer-sized value that will be passed to cb_result
     */
    void (*request_standby)(void *self, RAAT__SourceSelectionRequestStandbyCallback cb_result, void *cb_userdata);

} RAAT__SourceSelectionPlugin;

/**
 * Helper for managing sourceselection state listeners
 */
typedef struct {
    RC__Allocator      *alloc;
    uv_mutex_t          lock;
    RC__List            listeners;
} RAAT__SourceSelectionStateListeners;

/**
 * Initialize a collection of source selection state listener callbacks
 *
 * \param self a pointer to a #RAAT__SourceSelectionStateListeners 
 * \param alloc memory allocator to use for keeping track of the list of listeners
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status 
RAAT__source_selection_state_listeners_init               (RAAT__SourceSelectionStateListeners    *self, 
                                                           RC__Allocator                          *alloc);

/**
 * Add a handler to a #RAAT__SourceSelectionStateListeners callback list
 *
 * \param self a pointer to a #RAAT__SourceSelectionStateListeners 
 * \param cb the callback to add
 * \param userdata an opaque pointer-sized value that will be passed to the callback
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status 
RAAT__source_selection_state_listeners_add                (RAAT__SourceSelectionStateListeners    *self, 
                                                           RAAT__SourceSelectionStateCallback      cb, 
                                                           void                                   *userdata);

/**
 * Remove a handler from a #RAAT__SourceSelectionStateListeners callback list
 *
 * \param self a pointer to a #RAAT__SourceSelectionStateListeners 
 * \param cb the callback to remove
 * \param userdata the userdata to remove
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status 
RAAT__source_selection_state_listeners_remove             (RAAT__SourceSelectionStateListeners    *self, 
                                                           RAAT__SourceSelectionStateCallback      cb, 
                                                           void                                   *userdata);

/**
 * Invoke each callback in a list of source selection state callbacks
 *
 * \param self a pointer to a #RAAT__SourceSelectionStateListeners 
 * \param state the current source selection state information
 */
RC__Status 
RAAT__source_selection_state_listeners_invoke             (RAAT__SourceSelectionStateListeners    *self, 
                                                           RAAT__SourceSelectionState             *state);

/**
 * Destroy a set of source selection state callbacks
 *
 * \param self a pointer to a #RAAT__SourceSelectionStateListeners 
 */
void 
RAAT__source_selection_state_listeners_destroy            (RAAT__SourceSelectionStateListeners    *self);

 /** @} */

#ifdef __cplusplus
}
#endif
#endif
