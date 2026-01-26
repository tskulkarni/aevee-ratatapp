//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//

#ifndef INCLUDED_RAAT_DEVICE_H
#define INCLUDED_RAAT_DEVICE_H

#include "rc_base.h"
#include "rc_allocator.h"
#include "rc_status.h"
#include "raat_fwd.h"
#include "raat_log.h"
#include "raat_server.h"
#include "raat_discovery.h"
#include "raat_info.h"
#include "raat_plugin_output.h"
#include "raat_plugin_volume.h"
#include "raat_plugin_source_selection.h"
#include "raat_plugin_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup raat_device Device
 *
 *  \brief
 *  This is the high-level interface for RAAT Device implementors.
 *
 *  @{
 */

/** Maximum length for device vendor strings */
#define RAAT__DEVICE_MAX_VENDOR_LEN (128)

/** Maximum length for device name strings */
#define RAAT__DEVICE_MAX_NAME_LEN (128)

/** Maximum length for device model strings */
#define RAAT__DEVICE_MAX_MODEL_LEN (128)

/** Maximum length for device serial strings */
#define RAAT__DEVICE_MAX_SERIAL_LEN (128)

/** Maximum length for device version strings */
#define RAAT__DEVICE_MAX_VERSION_LEN (128)

enum {
    RAAT__DEVICE_RUN_STATUS_ALREADY_RUNNING             = RAAT__DEVICE_STATUS_BASE,
    /**< Device is already running */

    RAAT__DEVICE_RUN_STATUS_PRECONDITION_NOT_MET        = RAAT__DEVICE_STATUS_BASE + 2,
    /**< A precondition was not met, which prevented the device from running. Check the logs for more information */

    RAAT__DEVICE_STOP_STATUS_NOT_RUNNING                = RAAT__DEVICE_STATUS_BASE + 3,
    /**< Device was not running, so no action was taken */
};

/**
 * Callback that contains a list of client types that are currently connected to this device.
 *
 * This is represented as a list of strings. 
 *
 * Currently defined values are "Roon" and "RoonEssentials". Other client types may exist in the future.
 */
typedef void (*RAAT__ClientTypesCallback)(void *userdata, int n_client_types, char **client_types);

/** Create and initialize a RAAT device instance.
 *
 * In most integrations, exactly one device will be instantiated. 
 *
 * \param alloc the allocator to use, or NULL (See \ref rc_allocator for more information)
 * \param log a #RAAT__Log instance that the device will use for logging
 * \param out_self Out parameter that will receive a pointer to the #RAAT__Device. This will be set to NULL in failure cases.
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__device_new                 (RC__Allocator *alloc, RAAT__Log *log, RAAT__Device **out_self);


/** Disable the usual discovery mechanism
 *
 * This is used for RAATServer/RoonBridge, since it handles discovery differently.
 *
 * \param self a non-NULL #RAAT__Device instance 
 */
RC_API void
RAAT__device_disable_discovery   (RAAT__Device   *self);

/** Get the log for this device
 *
 * \param self a non-NULL #RAAT__Device instance 
 * \retval the #RAAT__Log instace used by this device.
 */
RC_API RAAT__Log *
RAAT__device_get_log             (RAAT__Device   *self);

/** Get the server for this device
 *
 * \param self a non-NULL #RAAT__Device instance 
 * \retval the #RAAT__Server instace used by this device.
 */
RC_API RAAT__Server *
RAAT__device_get_server          (RAAT__Device   *self);

/** Get the info dictionary for this device
 *
 * \param self a non-NULL #RAAT__Device instance 
 * \retval the #RAAT__Info instace used by this device.
 */
RC_API RAAT__Info *
RAAT__device_get_info            (RAAT__Device   *self);

/** Get the discovery for this device
 *
 * \param self a non-NULL #RAAT__Device instance 
 * \retval the #RAAT__Discovery instace used by this device.
 */
RC_API RAAT__Discovery *
RAAT__device_get_discovery            (RAAT__Device   *self);

/**
 * Set the source selection plugin for this device
 * 
 * \param self a non-NULL #RAAT__Device instance 
 * \param plugin the source selection plugin, or NULL
 */
RC_API void
RAAT__device_set_source_selection_plugin   (RAAT__Device *self, RAAT__SourceSelectionPlugin *plugin);

/**
 * Set the transport plugin for this device
 * 
 * \param self a non-NULL #RAAT__Device instance 
 * \param plugin the transport plugin, or NULL
 */
RC_API void
RAAT__device_set_transport_plugin   (RAAT__Device *self, RAAT__TransportPlugin *plugin);

/**
 * Set the output plugin for this device
 *
 * \param self a non-NULL #RAAT__Device instance 
 * \param plugin the output plugin, or NULL
 */
RC_API void
RAAT__device_set_output_plugin   (RAAT__Device *self, RAAT__OutputPlugin *plugin);

/**
 * Set the volume plugin for this device
 *
 * \param self a non-NULL #RAAT__Device instance 
 * \param plugin the volume plugin, or NULL
 *
 * \retval the volume plugin, or NULL if none exists
 */
RC_API void
RAAT__device_set_volume_plugin   (RAAT__Device *self, RAAT__VolumePlugin *plugin);

/**
 * Get the output plugin for this device
 *
 * \param self a non-NULL #RAAT__Device instance 
 *
 * \retval the output plugin, or NULL if none exists
 */
RC_API RAAT__OutputPlugin *
RAAT__device_get_output_plugin   (RAAT__Device *self);

/**
 * Get the transport plugin for this device
 *
 * \param self a non-NULL #RAAT__Device instance 
 *
 * \retval the transport plugin, or NULL if none exists
 */
RC_API RAAT__TransportPlugin *
RAAT__device_get_transport_plugin   (RAAT__Device *self);

/**
 * Get the source selection plugin for this device
 *
 * \param self a non-NULL #RAAT__Device instance 
 *
 * \retval the source selection plugin, or NULL if none exists
 */
RC_API RAAT__SourceSelectionPlugin *
RAAT__device_get_source_selection_plugin   (RAAT__Device *self);

/**
 * Get the volume plugin for this device
 *
 * \param self a non-NULL #RAAT__Device instance 
 *
 * \retval the volume plugin, or NULL if none exists
 */
RC_API RAAT__VolumePlugin *
RAAT__device_get_volume_plugin   (RAAT__Device *self);

/**
 * Run a server for this RAAT device. This function is long running, and will not return until the
 * device has been stopped
 *
 * This function is thread-safe.
 *
 * \param self a non-NULL #RAAT__Device instance 
 * \retval A status code indicating whether the run was successful.
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__device_run                 (RAAT__Device *self);

/** \internal 
 *
 * Sets up the device without starting the event loop. For example, if you intend to fork a second thread and run
 * the event loop in the background.
 */
RC_API RC__Status
RAAT__device_run_phase0          (RAAT__Device *self);

/** \internal 
 *
 * Runs the event loop. You must call RAAT__device_run_phase0 first.
 */
RC_API RC__Status
RAAT__device_run_phase1          (RAAT__Device *self);

/** Stop a running device
 *
 * This function assumes that the device is currently running.
 *
 * \param self a non-NULL #RAAT__Device instance
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__device_stop               (RAAT__Device *self);


/** Rebind listen sockets
 *
 * \param self a non-NULL #RAAT__Device instance
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__device_rebind               (RAAT__Device *self);

/**
 * Register a function that will receive information about the current set of 
 * clients connected to this RAAT__Device
 */
RC_API RC__Status
RAAT__device_set_client_types_callback   (RAAT__Device               *self, 
                                          RAAT__ClientTypesCallback   cb,
                                          void                       *userdata);

/**
 * Delete a RAAT device instance.
 *
 * \param self a #RAAT__Device instance, or NULL.
 */
RC_API void 
RAAT__device_delete             (RAAT__Device   *self);

/** \internal */ 
RC_API void
RAAT__device_notify_client_type(RAAT__Device *self, const char *client_type, bool connected);

 /** @} */

#ifdef __cplusplus
}
#endif

#endif
