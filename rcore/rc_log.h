// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RC_LOG_H
#define INCLUDED_RC_LOG_H

#include "rc_base.h"
#include "rc_allocator.h"
#include "rc_status.h"

#include <stdlib.h>
#include <stdarg.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup rc_log Logging
 *
 * \internal
 *
 *  \brief
 *  This module implements a process-wide logging sub-system.
 *
 *  NOTE: RAAT does not use this logging system. See \ref raat_log for RAAT's logging mechanism
 *
 *  @{
 */

/** Represents the log level associated with a message */
typedef enum {
    RC__LOG_LEVEL_CRITICAL    = 0,
    /**< Critical */

    RC__LOG_LEVEL_ERROR,
    /**< Error */

    RC__LOG_LEVEL_WARNING,
    /**< Warning */

    RC__LOG_LEVEL_INFO,
    /**< Info */

    RC__LOG_LEVEL_TRACE,
    /**< Trace */

    RC__LOG_LEVEL_DEBUG,
    /**< Debug */

} RC__LogLevel;

typedef struct {
    int              seq;
    RC__LogLevel     level;
    int64_t          time;              // in microseconds since unix epoch
    const char      *message;
} RC__LogEntry;

/** Callback that receives a log entry */
typedef void (*RC__LogEntryCallback)(RC__LogEntry *entry, void *userdata);

/** Maps a #RC__LogLevel to a C string.
 *  
 * Using a #RC__LogLevel as the index to this array
 * will give the string equivalent.  The contents should not be modified.
 * 
 */
extern RC_API const char * const RC__LogLevelString[];

/**
 * Add a log callback
 * 
 * This is useful if you want to do something else with log messages, like re-directing them to stderr or to a system log file.
 *
 * \param cb a #RC__LogEntryCallback to invoke for each log message.
 * \param userdata user data to be passed to cb
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RC__log_add_callback(RC__LogEntryCallback cb, void *userdata);

/**
 * Remove a log callback
 *
 * \param cb a #RC__LogEntryCallback 
 * \param userdata user data that was passed in #RC__log_add_callback
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RC__log_remove_callback(RC__LogEntryCallback cb, void *userdata);

/** Write a message to a log.
 *
 * This function is thread-safe
 *
 * \param level the log level
 * \param fmt a format string supported by the C standard library on the current platform
 * \param ... additional arguments for the format string
 */
void RC__log_writef                   (RC__LogLevel                 level, 
                                       const char                    *fmt, 
                                       ...);

void RC__log_writev                   (RC__LogLevel                 level, 
                                       const char                    *fmt, 
                                       va_list ap);

/** Macro that writes a critical message 
 *
 * \param args printf style arguments
 */
#define RC__CRITICAL(...) RC__log_writef(RC__LOG_LEVEL_CRITICAL, "" __VA_ARGS__)

/** Macro that writes a error message 
 *
 * \param args printf style arguments
 */
#define RC__ERROR(...) RC__log_writef(RC__LOG_LEVEL_ERROR, "" __VA_ARGS__)

/** Macro that writes a warning message 
 *
 * \param args printf style arguments
 */
#define RC__WARNING(...) RC__log_writef(RC__LOG_LEVEL_WARNING, "" __VA_ARGS__)

/** Macro that writes a info message 
 *
 * \param args printf style arguments
 */
#define RC__INFO(...) RC__log_writef(RC__LOG_LEVEL_INFO, "" __VA_ARGS__)

/** Macro that writes a trace message 
 *
 * \param args printf style arguments
 */
#define RC__TRACE(...) RC__log_writef(RC__LOG_LEVEL_TRACE, "" __VA_ARGS__)

/** Macro that writes a debug message 
 *
 * \param args printf style arguments
 */
#define RC__DEBUG(...) RC__log_writef(RC__LOG_LEVEL_DEBUG, "" __VA_ARGS__)

 /** @} */

#ifdef __cplusplus
}
#endif

#endif
