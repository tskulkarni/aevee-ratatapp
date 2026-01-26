// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_LOG_H
#define INCLUDED_RAAT_LOG_H

#include "rc_base.h"
#include "rc_allocator.h"
#include "rc_status.h"

#include <stdlib.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RAAT__LOG_MINIMUM_SIZE (256*1024)
#define RAAT__LOG_DEFAULT_SIZE (256*1024)

/** \defgroup raat_log Logging
 *
 *  \brief
 *  This module implements the logging sub-system.
 *
 *  The RAAT log is a 256kb in-memory circular buffer. It can be polled or monitored via the RAAT-LL API or \ref raatool.
 *
 *  @{
 */

/** The opaque structure definition for the RAAT__Log type
 */
typedef struct RAAT__Log_s RAAT__Log;

/** Represents the log level associated with a message */
typedef enum {
    RAAT__LOG_LEVEL_CRITICAL    = 0,
    /**< Critical */

    RAAT__LOG_LEVEL_ERROR,
    /**< Error */

    RAAT__LOG_LEVEL_WARNING,
    /**< Warning */

    RAAT__LOG_LEVEL_INFO,
    /**< Info */

    RAAT__LOG_LEVEL_TRACE,
    /**< Trace */

    RAAT__LOG_LEVEL_DEBUG,
    /**< Debug */

} RAAT__LogLevel;

typedef struct {
    int              seq;
    RAAT__LogLevel   level;
    int64_t          time;              // in microseconds since unix epoch
    const char      *message;
} RAAT__LogEntry;

/** Callback that receives a log entry */
typedef void (*RAAT__LogEntryCallback)(RAAT__LogEntry *entry, void *userdata);

/** Maps a #RAAT__LogLevel to a C string.
 *  
 * Using a #RAAT__LogLevel as the index to this array
 * will give the string equivalent.  The contents should not be modified.
 * 
 */
extern RC_API const char * const RAAT__LogLevelString[];

/** Create a log 
 *
 * \param alloc the allocator to use, or NULL (See \ref rc_allocator for more information)
 * \param log_size the amount of memory that the log should consume, in bytes.
 * \param out_self a non-NULL pointer that will receive the newly created RAAT__Log instance
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status 
RAAT__log_new               (RC__Allocator *alloc, size_t log_size, RAAT__Log **out_self);

/**
 * Delete a RAAT log instance.
 *
 * \param self a #RAAT__Log instance, or NULL.
 */
RC_API void 
RAAT__log_delete          (RAAT__Log *self);

/**
 * Add a log callback
 * 
 * This is useful if you want to do something else with log messages, like re-directing them to stderr or to a system log file.
 *
 * \param self a #RAAT__Log instance, or NULL.
 * \param cb a #RAAT__LogEntryCallback to invoke for each log message.
 * \param userdata user data to be passed to cb
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status
RAAT__log_add_callback(RAAT__Log *self, RAAT__LogEntryCallback cb, void *userdata);

/**
 * Remove a log callback
 *
 * \param self a #RAAT__Log instance, or NULL.
 * \param cb a #RAAT__LogEntryCallback 
 * \param userdata user data that was passed in #RAAT__log_add_callback
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status
RAAT__log_remove_callback(RAAT__Log *self, RAAT__LogEntryCallback cb, void *userdata);

/** Write a message to a log.
 *
 * Individual log messages that exceed 1024 bytes will be truncated.
 *
 * This function is thread-safe
 *
 * \param self a non-NULL #RAAT__Log instance 
 * \param level the log level
 * \param fmt a format string supported by the C standard library on the current platform
 * \param ... additional arguments for the format string
 */
void RAAT__log_writef                   (RAAT__Log                     *self, 
                                         RAAT__LogLevel                 level, 
                                         const char                    *fmt, 
                                         ...);

/** Iterate over log messages
 *
 * Note that while this iteration is occurring, the logging system is unavailable for logging or other use.
 * Methods calling this must not log from within the callback.
 *
 * This function is thread-safe
 *
 * \param self a non-NULL #RAAT__Log instance 
 * \param minimum_seq log entries with a sequence number below minimum_seq will not be returned. Use -1 to return all
 * \param cb a callback to invoke for each entry. DO NOT CALL OTHER RAAT__log_* functions FROM WITHIN THIS CALLBACK.
 * \param userdata extra data that's passed into the callback
 *
 */
void RAAT__log_iterate                  (RAAT__Log                     *self,
                                         int                            minimum_seq, 
                                         RAAT__LogEntryCallback         cb, 
                                         void                          *userdata);

/** Clear the log
 *
 * This function is thread-safe
 *
 * \param self a non-NULL #RAAT__Log instance 
 */
void RAAT__log_clear                    (RAAT__Log                     *self);


/** Macro that writes a critical message to RAAT__CURRENT_LOG. 
 *
 * You must define RAAT__CURRENT_LOG to point to a non-null #RAAT__Log* instance prior to using this macro.
 *
 * \param args printf style arguments
 */
#define RAAT__CRITICAL(...) RAAT__log_writef(RAAT__CURRENT_LOG, RAAT__LOG_LEVEL_CRITICAL, "" __VA_ARGS__)

/** Macro that writes a error message to RAAT__CURRENT_LOG. 
 *
 * You must define RAAT__CURRENT_LOG to point to a non-null #RAAT__Log* instance prior to using this macro.
 *
 * \param args printf style arguments
 */
#define RAAT__ERROR(...) RAAT__log_writef(RAAT__CURRENT_LOG, RAAT__LOG_LEVEL_ERROR, "" __VA_ARGS__)

/** Macro that writes a warning message to RAAT__CURRENT_LOG. 
 *
 * You must define RAAT__CURRENT_LOG to point to a non-null #RAAT__Log* instance prior to using this macro.
 *
 * \param args printf style arguments
 */
#define RAAT__WARNING(...) RAAT__log_writef(RAAT__CURRENT_LOG, RAAT__LOG_LEVEL_WARNING, "" __VA_ARGS__)

/** Macro that writes a info message to RAAT__CURRENT_LOG. 
 *
 * You must define RAAT__CURRENT_LOG to point to a non-null #RAAT__Log* instance prior to using this macro.
 *
 * \param args printf style arguments
 */
#define RAAT__INFO(...) RAAT__log_writef(RAAT__CURRENT_LOG, RAAT__LOG_LEVEL_INFO, "" __VA_ARGS__)

/** Macro that writes a trace message to RAAT__CURRENT_LOG. 
 *
 * You must define RAAT__CURRENT_LOG to point to a non-null #RAAT__Log* instance prior to using this macro.
 *
 * \param args printf style arguments
 */
#define RAAT__TRACE(...) RAAT__log_writef(RAAT__CURRENT_LOG, RAAT__LOG_LEVEL_TRACE, "" __VA_ARGS__)

/** Macro that writes a debug message to RAAT__CURRENT_LOG. 
 *
 * You must define RAAT__CURRENT_LOG to point to a non-null #RAAT__Log* instance prior to using this macro.
 *
 * \param args printf style arguments
 */
#define RAAT__DEBUG(...) RAAT__log_writef(RAAT__CURRENT_LOG, RAAT__LOG_LEVEL_DEBUG, "" __VA_ARGS__)

 /** @} */

#ifdef __cplusplus
}
#endif

#endif
