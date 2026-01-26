//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_PLUGIN_VOLUME_H
#define INCLUDED_RAAT_PLUGIN_VOLUME_H

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

/** \defgroup raat_plugin_volume Volume Plugin
 *
 *  \brief
 *  The volume plugin provides an integration point for volume and mute controls on a device. See #RAAT__VolumePlugin.
 *
 *  @{
 */

typedef enum {
    RAAT__VOLUME_TYPE_NONE                = 0,
    RAAT__VOLUME_TYPE_NUMBER              = 1,
    RAAT__VOLUME_TYPE_DB                  = 2,
    RAAT__VOLUME_TYPE_INCREMENTAL         = 3,
    RAAT__VOLUME_TYPE_HIDDEN_NUMBER       = 4,
} RAAT__VolumeType;

typedef enum {
    RAAT__VOLUME_INCREMENT_UP,
    RAAT__VOLUME_INCREMENT_DOWN,
} RAAT__VolumeIncrement;

typedef struct {
    RAAT__VolumeType    volume_type;

    // vvv with RAAT__VOLUME_TYPE_NONE or RAAT__VOLUME_TYPE_INCREMENTAL, the remaining items in this data structure are not used. 
    // Please initialize them to zero. vvv
    
    double              min_volume;
    double              max_volume;
    double              volume_value;
    bool                mute_value;

    // Min step size, primarily for decibel based volume controls
    // 
    // Ignored if 0
    // 
    double              volume_step;         

    // Minimum and Maximum volume measured in decibels. 
    //
    // This can tell us how much dynamic range is in a RAAT__VOLUME_TYPE_NUMBER scale
    //
    // If db_min_volume == db_max_volume == 0, these fields are ignored (for backwards compatibility)
    //
    double              db_min_volume;          // leave as default value (0.0) if not supported. Pre 1.0.9 implementations don't send this.
    double              db_max_volume;          
} RAAT__VolumeState;

// status codes
enum {
    RAAT__VOLUME_PLUGIN_STATUS_DEVICE_OPEN_FAILED          = RAAT__VOLUME_PLUGIN_STATUS_BASE + 0,
    RAAT__VOLUME_PLUGIN_STATUS_DEVICE_INIT_FAILED          = RAAT__VOLUME_PLUGIN_STATUS_BASE + 1,
    RAAT__VOLUME_PLUGIN_STATUS_INVALID_CONFIG              = RAAT__VOLUME_PLUGIN_STATUS_BASE + 2,
    RAAT__VOLUME_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED        = RAAT__OUTPUT_PLUGIN_STATUS_BASE + 3,
    RAAT__VOLUME_PLUGIN_STATUS_OUTPUT_PLUGIN_NOT_FOUND     = RAAT__OUTPUT_PLUGIN_STATUS_BASE + 4,
    RAAT__VOLUME_PLUGIN_STATUS_OUTPUT_PLUGIN_NOT_SUPPORTED = RAAT__OUTPUT_PLUGIN_STATUS_BASE + 5,
    RAAT__VOLUME_PLUGIN_STATUS_VOLUME_NOT_SUPPORTED        = RAAT__OUTPUT_PLUGIN_STATUS_BASE + 6,
};


/** Callback used to reflect volume state changes 
 *
 * \param cb_userdata opaque pointer-sized value passed to add_state_listener.
 * \param state The current state of the volume plugin.
 */
typedef void (*RAAT__VolumeStateCallback)(void *cb_userdata, RAAT__VolumeState *state);

/**
 * A virtual-function table that can be used to implement convenience switching.
 *
 * Implementing this interface is a requirement for devices that support volume adjustment. The goal is to expose the
 * highest quality hardware volume control possible to Roon so that it can be controlled remotely.
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
     * \param self the volume plugin instance
     * \param out_info pointer to a json_t that will receive the info object. This must be a JSON object.
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*get_info)(void *self, json_t **out_info);

    /**
     * Register a state listener with the volume plugin.
     *
     * Use the #RAAT__VolumeStateListeners helper structure + its associated functions to implement this function (see sample code).
     *
     * \param self the volume plugin
     * \param cb a callback that will receive state changes emitted by this volume plugin
     * \param cb_userdata an opaque pointer-sized value that will be passed to <tt>cb</tt>
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*add_state_listener)(void *self, RAAT__VolumeStateCallback cb, void *cb_userdata);

    /**
     * Unregister a state listener with the volume plugin.
     *
     * \param self the volume plugin
     * \param cb the callback to be removed
     * \param cb_userdata the cb_userdata that goes with the callback
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*remove_state_listener)(void *self, RAAT__VolumeStateCallback cb, void *cb_userdata);

    /**
     * Get the current state of the volume plugin
     *
     * \param self the volume plugin
     * \param out_state output parameter that will receive the current volume state.
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*get_state)(void *self, RAAT__VolumeState *out_state);

    /**
     * Change the volume setting.
     *
     * This is used for RAAT__VOLUME_TYPE_NUMBER and RAAT__VOLUME_TYPE_DB
     *
     * If this results in a state change, the plugin must notify any state listeners.
     *
     * \param self the volume plugin
     * \param volume_value the new volume value
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*set_volume)(void *self, double volume_value);

    /**
     * Increment the volume
     *
     * This is used only for RAAT__VOLUME_TYPE_INCREMENTAL
     *
     * \param self the volume plugin
     * \param increment how to increment the volume
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*increment_volume)(void *self, RAAT__VolumeIncrement increment);

    /**
     * Change the mute setting
     *
     * If this results in a state change, the plugin must notify any state listeners.
     *
     * \param self the volume plugin
     * \param mute_value the new mute value
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*set_mute)(void *self, bool mute_value);

    /**
     * Change the mute setting
     *
     * This is used only for RAAT__VOLUME_TYPE_INCREMENTAL
     *
     * If this results in a state change, the plugin must notify any state listeners.
     *
     * \param self the volume plugin
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*toggle_mute)(void *self);
} RAAT__VolumePlugin;

/*
 * Helper for managing volume state listeners
 */
typedef struct {
    RC__Allocator      *alloc;
    uv_mutex_t          lock;
    RC__List            listeners;
} RAAT__VolumeStateListeners;

/**
 * Initialize a collection of volume state listener callbacks
 *
 * \param self a pointer to a #RAAT__VolumeStateListeners 
 * \param alloc memory allocator to use for keeping track of the list of listeners
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status 
RAAT__volume_state_listeners_init               (RAAT__VolumeStateListeners    *self, 
                                                 RC__Allocator                 *alloc);

/**
 * Add a handler to a #RAAT__VolumeStateListeners callback list
 *
 * \param self a pointer to a #RAAT__VolumeStateListeners 
 * \param cb the callback to add
 * \param userdata an opaque pointer-sized value that will be passed to the callback
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status 
RAAT__volume_state_listeners_add                (RAAT__VolumeStateListeners    *self, 
                                                 RAAT__VolumeStateCallback      cb, 
                                                 void                          *userdata);

/**
 * Remove a handler from a #RAAT__VolumeStateListeners callback list
 *
 * \param self a pointer to a #RAAT__VolumeStateListeners 
 * \param cb the callback to remove
 * \param userdata the userdata to remove
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status 
RAAT__volume_state_listeners_remove             (RAAT__VolumeStateListeners    *self, 
                                                 RAAT__VolumeStateCallback      cb, 
                                                 void                          *userdata);

/**
 * Invoke each callback in a list of volume state callbacks
 *
 * \param self a pointer to a #RAAT__VolumeStateListeners 
 * \param state the current state of the volume plugin
 */
RC__Status 
RAAT__volume_state_listeners_invoke             (RAAT__VolumeStateListeners    *self, 
                                                 RAAT__VolumeState             *state);

/**
 * Destroy a set of volume state callbacks
 *
 * \param self a pointer to a #RAAT__VolumeStateListeners 
 */
void 
RAAT__volume_state_listeners_destroy            (RAAT__VolumeStateListeners    *self);

 /** @} */

#ifdef __cplusplus
}
#endif

#endif
