//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//

#ifndef INCLUDED_RAAT_BASE_H
#define INCLUDED_RAAT_BASE_H

#include "raat_base.h"

/** \defgroup raat_base Constants
 *
 *  \brief
 *  This module contains shared constants relative to one or more other modules in the RAAT SDK
 *
 *  @{
 */

/** 
 * The current version of the RAAT SDK. 
 *
 * The version number is incremented by Roon as part of SDK releases, and should not be modified by partners
 * or integrators.
 */
#define RAAT__VERSION          "1.1.38"

/** The current version number for RAAT-LL protocol. Roon uses this version number to manage compatibility with RAAT
 * devices over time.
 */
#define RAAT__PROTOCOL_VERSION (3)

/** The service id for RAAT. This is used as part of the discovery subsystem.
 */
#define RAAT__SERVICE_GUID_STRING ("5e2042ad-9bc5-4508-be92-ff68f19bdc93")

/* Max message length for low-level protocol */
#define RAAT__LL_MAX_MESSAGE_LEN (4096*1024)               // max 4mb message size

/** message type for request messagesin the RAAT-LL protocol */
#define RAAT__LL_REQUEST         0x80000001

/** message type for RESPONSE messages in the RAAT-LL protocol */
#define RAAT__LL_RESPONSE        0x80000002

/** message type for KEEPALIVE messages in the RAAT-LL protocol */
#define RAAT__LL_KEEPALIVE       0x80000003

/** message type for BEGIN_KEEPALIVE messages in the RAAT-LL protocol */
#define RAAT__LL_BEGIN_KEEPALIVE 0x80000004

/** message type for END_KEEPALIVE messages in the RAAT-LL protocol */
#define RAAT__LL_END_KEEPALIVE   0x80000005

/** protocol flags used in \ref raat_client and \ref raat_server modules */
enum {
    RAAT__FINAL_RESPONSE        = 1,
    /**< Indicates a final response */

    RAAT__CANCELED              = 2,
    /**< Indicates a canceled request */

    RAAT__INVALID_JSON          = 4,
    /**< Indicates that a response contained invalid JSON */
};

/** 
 * Default keepalive interval in ms
 *
 * Keepalive timeouts in RAAT are driven by the client software. This establishes the defaults for 
 *  the \ref raat_client module. The defaults used in production are determined by the Roon application.
 */
#define RAAT__LL_DEFAULT_KEEPALIVE         (2000)

/** 
 * Default keepalive timeout in ms
 *
 * Keepalive timeouts in RAAT are driven by the client software. This establishes the defaults for 
 *  the \ref raat_client module. The defaults used in production are determined by the Roon application.
 */
#define RAAT__LL_DEFAULT_KEEPALIVE_TIMEOUT (10000)

void RAAT__static_init(void);

 /** @} */

#endif
