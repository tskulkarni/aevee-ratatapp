//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_INFO_H
#define INCLUDED_RAAT_INFO_H

#include "rc_base.h"
#include "rc_allocator.h"
#include "rc_status.h"
#include "raat_log.h"

#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup raat_info Info
 *
 * \brief
 *
 * A #RAAT__Info instance represents the info dictionary for a RAAT device. This is a set of string->string pairs that comprises the
 * basic device information.
 *
 * Keys may be up to #RAAT__INFO_MAX_KEY_LEN bytes long, including the terminating null character.
 * Values may be up to #RAAT__INFO_MAX_VALUE_LEN bytes long, including the terminating null character.
 *
 * Keys and values are UTF-8 encoded.
 *
 * Custom keys are allowed, but they must be prefixed with "c_" to indicate that they are custom. 
 * Known keys include #RAAT__INFO_KEY_VENDOR, #RAAT__INFO_KEY_OUTPUT_NAME, #RAAT__INFO_KEY_SERIAL, #RAAT__INFO_KEY_VERSION, #RAAT__INFO_KEY_MODEL, 
 * #RAAT__INFO_KEY_SERIAL, and #RAAT__INFO_KEY_UNIQUE_ID.
 *
 * Note that #RAAT__INFO_KEY_RAAT_VERSION and #RAAT__INFO_KEY_PROTOCOL_VERSION are populated automatically.
 *
 * Validation requirements:
 *
 * * #RAAT__INFO_KEY_VENDOR, #RAAT__INFO_KEY_MODEL, #RAAT__INFO_KEY_VERSION, and #RAAT__INFO_KEY_UNIQUE_ID are required
 * * If #RAAT__INFO_KEY_SERIAL is set, it must be non-empty 
 * * Any device/implementation specific keys must be prefixed with an underscore. "_" This indicates that they are custom keys. 
 *
 *  @{
 */

/** Maximum length for keys */
#define RAAT__INFO_MAX_KEY_LEN   (32)

/** Maximum length for values */
#define RAAT__INFO_MAX_VALUE_LEN (128) 

/** Constant for built-in key "vendor" */
#define    RAAT__INFO_KEY_VENDOR                ("vendor")
/** Constant for built-in key "output_name" */
#define    RAAT__INFO_KEY_OUTPUT_NAME           ("output_name")   
/** Constant for built-in key "serial" */
#define    RAAT__INFO_KEY_SERIAL                ("serial") 
/** Constant for built-in key "model" */
#define    RAAT__INFO_KEY_MODEL                 ("model")  
/** Constant for built-in key "vendor_model" */
#define    RAAT__INFO_KEY_VENDOR_MODEL          ("vendor_model")  
/** Constant for built-in key "version" */
#define    RAAT__INFO_KEY_VERSION               ("version")
/** Constant for built-in key "raat_version" */
#define    RAAT__INFO_KEY_RAAT_VERSION          ("raat_version")
/** Constant for built-in key "protocol_version" */
#define    RAAT__INFO_KEY_PROTOCOL_VERSION      ("protocol_version")
/** Constant for built-in key "unique_id" */
#define    RAAT__INFO_KEY_UNIQUE_ID             ("unique_id")
/** Constant for built-in key "unique_id" */
#define    RAAT__INFO_KEY_CONFIG_URL            ("config_url")
/** Constant for built-in key "auto_name" */
#define    RAAT__INFO_KEY_AUTO_NAME             ("auto_name")  

enum {
    RAAT__INFO_SET_STATUS_INVALID_VALUE         = RAAT__INFO_STATUS_BASE + 0,
    /**<  Indicates that a call to RAAT__info_set failed because the <tt>val</tt> parameter was invalid. */
      
    RAAT__INFO_SET_STATUS_INVALID_KEY           = RAAT__INFO_STATUS_BASE + 1,  
    /**<  Indicates that a call to RAAT__info_set failed because the <tt>key</tt> parameter was invalid. */
};


/** The opaque structure definition for the RAAT__Info type
 */
typedef struct RAAT__Info_s RAAT__Info;

/** Callback function used when iterating over the key/value pairs in a a #RAAT__Info instance 
 *
 * \param key null-terminated UTF-8 string containing the key
 * \param key null-terminated UTF-8 string containing the val
 * \param key null-terminated UTF-8 string containing the userdata provided to #RAAT__info_foreach
 *
 * */
typedef void (*RAAT__InfoForeachCallback)(const char *key, const char *val, void *userdata);

/** Create and initialize a #RAAT__Info instance.
 *
 * \param alloc the allocator to use, or NULL (See \ref rc_allocator for more information)
 * \param log a non-null #RAAT__Log instance that should be used when logging from this instance
 * \param out_self Out parameter that will receive a pointer to the #RAAT__Info. This will be set to NULL in failure cases.
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__info_new                 (RC__Allocator *alloc, RAAT__Log *log, RAAT__Info **out_self);

/**
 * Set a value in the info dictionary.
 *
 * Keys may be up to #RAAT__INFO_MAX_KEY_LEN bytes long, including the terminating null character.
 * Values may be up to #RAAT__INFO_MAX_VALUE_LEN bytes long, including the terminating null character.
 *
 * Keys and values are UTF-8 encoded.
 *
 * Custom keys are allowed, but they must be prefixed with "c_" to indicate that they are custom. 
 * Known keys include #RAAT__INFO_KEY_VENDOR, #RAAT__INFO_KEY_OUTPUT_NAME, #RAAT__INFO_KEY_SERIAL, #RAAT__INFO_KEY_VERSION, #RAAT__INFO_KEY_MODEL, 
 * and #RAAT__INFO_KEY_SERIAL.
 *
 * \param self a non-NULL #RAAT__Info instance 
 * \param key a null-terminated UTF-8 encoded C string that may be up to #RAAT__INFO_MAX_KEY_LEN bytes in size, including the terminating null byte.
 * \param val a null-terminated UTF-8 encoded C string that may be up to #RAAT__INFO_MAX_VALUE_LEN  bytes in size, including the terminating null byte.
 * \retval The return status. (See \ref rc_status for more information)
 *
 */
RC_API RC__Status
RAAT__info_set                 (RAAT__Info *self, const char *key, const char *val);

/**
 * Retrieve a value from the info dictionary.
 *
 * \param self a non-NULL #RAAT__Info instance
 * \param key a null-terminated UTF-8 encoded C string that may be up to #RAAT__INFO_MAX_KEY_LEN bytes in size, including the terminating null byte.
 * \param out_value an output buffer that is at least #RAAT__INFO_MAX_VALUE_LEN bytes in size. This will receive the value.
 * \retval true if the key was found in the dictionary, false otherwise.
 */
RC_API bool        
RAAT__info_try_get_value       (RAAT__Info *self, const char *key, char *out_value/*[RAAT__INFO_MAX_VALUE_LEN]*/);

/** Validate a RAAT__info to ensure that all required fields have been populated.
 *
 * \param self a non-NULL #RAAT__Info instance
 * \retval true if the contents of the info dictionary are valid, false otherwise.
 */
RC_API bool
RAAT__info_validate            (RAAT__Info *self);

/** Iterate over the key/value pairs in the info dictionary
 *
 * \param self a non-NULL #RAAT__Info instance
 * \param cb a callback that should be invoked for each value
 * \param userdata userdata for the callback
 */
RC_API void
RAAT__info_foreach             (RAAT__Info *self, RAAT__InfoForeachCallback cb, void *userdata);

/**
 * Delete a #RAAT__Info instance.
 *
 * \param self a #RAAT__Info instance, or NULL.
 */
RC_API void RAAT__info_delete            (RAAT__Info   *self);

 /** @} */

#ifdef __cplusplus
}
#endif

#endif
