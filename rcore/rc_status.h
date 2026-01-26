//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RC_STATUS_H
#define INCLUDED_RC_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup rc_status Error Handling
 *
 * ## Generic Status Codes
 *
 * Generic status codes are reserved for highly generic situations like "success" and "out of memory". Any function can
 * return any generic status code at any time, unless otherwise documented.
 *
 * Generic status codes live in the 0-1000 range. You can check to see if a code is generic using the #RC__STATUS_IS_GENERIC method.
 *
 * ## Module-specific status codes.
 *
 * Each module may define its own status codes as well. These may be returned from functions in that module, and are prefixed
 * with <tt>RC__<MODULE>_STATUS_</tt> or <tt>RAAT__<MODULE>_STATUS_</tt>.
 *
 * Status codes are not allowed to have multiple meanings. This requires us to reserve status code ranges for each module. This 
 * is accomplished using RC__<MODULE>_STATUS_BASE constants, defined in rc_status.h
 *
 *  @{
 */

enum {
    RC__STATUS_SUCCESS                = 0,
    /**< Operation was successful */

    RC__STATUS_UNEXPECTED_ERROR       = 1,
    /**< Failed due to an unexpected error. Check the log. */

    RC__STATUS_OUT_OF_MEMORY          = 2,
    /**< A memory allocation failed */

    RC__STATUS_NOT_IMPLEMENTED        = 3,
    /**< not implemented */

    RC__STATUS_NOT_SUPPORTED          = 4,
    /**< not supported */

    RC__STATUS_INVALID_ARGUMENT       = 5,
    /**< invalid argument */

    RC__STATUS_CANCELED               = 6,
    /**< operation canceled */

    RC__STATUS_NOT_FOUND              = 7,
    /**< entity not found */

    RC__STATUS_GENERIC_MAX            = 999,
    /**< \internal Maximum value for generic statuses. */

    RAAT__LOG_STATUS_BASE             = 1000,
    /**< \internal establishes a status code range for the \ref raat_log module */
    RAAT__LOG_STATUS_MAX              = 1999,
    /**< \internal establishes a status code range for the \ref raat_log module */

    RAAT__DEVICE_STATUS_BASE          = 2000,
    /**< \internal establishes a status code range for the \ref raat_device module */
    RAAT__DEVICE_STATUS_MAX           = 2999,
    /**< \internal establishes a status code range for the \ref raat_device module */

    RAAT__DISCOVERY_STATUS_BASE       = 3000,
    /**< \internal establishes a status code range for the \ref raat_discovery module */
    RAAT__DISCOVERY_STATUS_MAX        = 3999,
    /**< \internal establishes a status code range for the \ref raat_discovery module */

    RAAT__SERVER_STATUS_BASE          = 4000,
    /**< \internal establishes a status code range for the \ref raat_server module */
    RAAT__SERVER_STATUS_MAX           = 4999,
    /**< \internal establishes a status code range for the \ref raat_server module */

    RAAT__INFO_STATUS_BASE            = 5000,
    /**< \internal establishes a status code range for the \ref raat_info module */
    RAAT__INFO_STATUS_MAX             = 5999,
    /**< \internal establishes a status code range for the \ref raat_info module */

    RAAT__CLIENT_STATUS_BASE          = 6000,
    /**< \internal establishes a status code range for the \ref raat_client module */
    RAAT__CLIENT_STATUS_MAX           = 6999,
    /**< \internal establishes a status code range for the \ref raat_client module */

    RAAT__SESSION_STATUS_BASE          = 7000,
    /**< \internal establishes a status code range for the \ref raat_session module */
    RAAT__SESSION_STATUS_MAX           = 7999,
    /**< \internal establishes a status code range for the \ref raat_session module */

    RAAT__OUTPUT_PLUGIN_STATUS_BASE          = 8000,
    /**< \internal establishes a status code range for the \ref raat_output_plugin module */
    RAAT__OUTPUT_PLUGIN_STATUS_MAX           = 8999,
    /**< \internal establishes a status code range for the \ref raat_output_plugin module */

    RAAT__VOLUME_PLUGIN_STATUS_BASE          = 9000,
    /**< \internal establishes a status code range for the \ref raat_volume module */
    RAAT__VOLUME_PLUGIN_STATUS_MAX           = 9999,
    /**< \internal establishes a status code range for the \ref raat_volume module */

    RAAT__SOURCE_SELECTION_PLUGIN_STATUS_BASE          = 10000,
    /**< \internal establishes a status code range for the \ref raat_source_selection module */
    RAAT__SOURCE_SELECTION_PLUGIN_STATUS_MAX           = 10999,
    /**< \internal establishes a status code range for the \ref raat_source_selection module */

    RAAT__TRANSPORT_PLUGIN_STATUS_BASE          = 11000,
    /**< \internal establishes a status code range for the \ref raat_transport module */
    RAAT__TRANSPORT_PLUGIN_STATUS_MAX           = 11999,
    /**< \internal establishes a status code range for the \ref raat_transport module */

    RNET__JSON_SERVER_STATUS_BASE          = 12000,
    /**< \internal establishes a status code range for the \ref rnet_jsonserver module */
    RNET__JSON_SERVER_STATUS_MAX            = 12999,
    /**< \internal establishes a status code range for the \ref rnet_jsonserver module */


    RC__USER_STATUS_BASE                 = 10000000,
    /**< \internal use for user-defined status ranges */

    /** @} */
};

/** The status type returned from RC methods that need to report success or failure */
typedef int RC__Status;

/** Determines whether a #RC__Status value represents success.
 *
 * \retval TRUE or FALSE, depending on whether the status was successful.
 */
#define RC__STATUS_IS_SUCCESS(status) ((status) == RC__STATUS_SUCCESS)

/** Determines whether a #RC__Status value is a generic status or a module-specific status.
 *
 * \retval TRUE or FALSE, depending on whether the status is generic.
 */
#define RC__STATUS_IS_GENERIC(status) ((status) < RC__STATUS_GENERIC_MAX)

/** Converts a status code to a string 
 *
 * \param status the status code to convert
 * \retval a string. Do not modify the contents of the string, or attempt to free it.
 *
 * */
const char *RC__status_to_string(RC__Status status);

typedef const char *(*RC__StatusToString)(RC__Status status);

void RC__status_register(int min, int max, RC__StatusToString fn);

/** @} */

#ifdef __cplusplus
}
#endif

#endif
