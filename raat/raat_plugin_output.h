//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_PLUGIN_OUTPUT_H
#define INCLUDED_RAAT_PLUGIN_OUTPUT_H
#include "rc_base.h"
#include "rc_allocator.h"
#include "rc_status.h"
#include "rc_list.h"
#include "raat_log.h"
#include "raat_stream.h"

#include <jansson.h>
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup raat_plugin_output Output Plugin
 *
 *  \brief
 *  The output plugin is used to implement or customize audio output. See #RAAT__OutputPlugin for more information.
 *
 *  @{
 */

/** Output lost calback. Used by setup 
 *
 *  \param cb_userdata opaque pointer-sized value provided to setup.
 *  \param reason the reason why the output was lost. If present, a JSON object. See force_teardown for more information.
 */
typedef void (*RAAT__OutputLostCallback)(void *cb_userdata, /*optional*/json_t *reason);

/** Output setup calback. Used by setup 
 *
 *  \param cb_userdata opaque pointer-sized value provided to setup.
 *  \param status the status of the setup operation
 *  \param token if <tt>RC__STATUS_IS_SUCCESS(status)</tt>, then this contains the setup token that should be used in future output plugin function calls.
 */
typedef void (*RAAT__OutputSetupCallback)(void *cb_userdata, RC__Status status, int token);

// status codes
enum {
    RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED = RAAT__OUTPUT_PLUGIN_STATUS_BASE + 0,
    /**< Indicates that the #RAAT__StreamFormat is not supported */

    RAAT__OUTPUT_PLUGIN_STATUS_INVALID_TOKEN        = RAAT__OUTPUT_PLUGIN_STATUS_BASE + 1,
    /**< Indicates that an output plugin function was called with a token that does not currently own the plugin devuce */

    RAAT__OUTPUT_PLUGIN_STATUS_INVALID_STATE        = RAAT__OUTPUT_PLUGIN_STATUS_BASE + 2,
    /**< Indicates that an output plugin function was called when the plugin was in an invalid state */

    RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED   = RAAT__OUTPUT_PLUGIN_STATUS_BASE + 3,
    /**< Indicates that the output plugin failed to open a hardware device because it does not exist or is in use elsewhere */

    RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED   = RAAT__OUTPUT_PLUGIN_STATUS_BASE + 4,
    /**< Indicates that the output plugin failed to initialize an audio device for a reason other than format support */

    RAAT__OUTPUT_PLUGIN_STATUS_INVALID_CONFIG       = RAAT__OUTPUT_PLUGIN_STATUS_BASE + 5,
    /**< Indicates that the output plugin was created with an invalid configuration object */

    RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_IN_USE        = RAAT__OUTPUT_PLUGIN_STATUS_BASE + 6,
    /**< Indicates that the output plugin failed to initialize an audio device because it was in use elsewhere */
};


/** Represents an invalid setup token value */
#define RAAT__OUTPUT_TOKEN_INVALID ((int)(-1))

/** Callback type receives messages emitted by output plugin instances */
typedef void (*RAAT__OutputMessageCallback)(void *cb_userdata, json_t *message);


/**
 * Represents the interface between the output plugin and a software volume control implementation.
 *
 * Software volume control plugins use #set_software_volume to register an instance of this interface
 * with an output plugin.
 */
typedef struct {
    void *userdata;

    /**
     * Determine whether the software volume plugin supports a given stream format.
     *
     * When responding to queries about supported formats, the output plugin should filter out formats that are not supported
     * by the software volume control implementation to prevent the server from attempting to transmit those formats.
     *
     * In particular, since not all software volume controls will support DSD content, many such implementations will reject DSD formats.
     *
     * \retval RC__STATUS_SUCCESS if the format is supported, and RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED otherwise. 
     */
    RC__Status (*supports_format)(void *userdata, RAAT__StreamFormat *format);

    /*
     * Set up the software volume DSP.
     *
     * This should perform any memory allocations or pre-computation associated with the volume control.
     *
     * \param userdata the userdata value from the #RAAT__OutputSoftwareVolume structure
     * \param format the stream format to be used for software volume control. 
     * \param out_delay an output parameter that will receive the delay (in samples) imposed by this DSP.
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*setup)(void *userdata, RAAT__StreamFormat *format, int *out_delay);

    /*
     * Process a buffer of data
     *
     * \param userdata the userdata value from the #RAAT__OutputSoftwareVolume structure
     * \param extra_db is an extra gain adjustment to be summed with the volume control.
     * \param data sample data. DSD is interleaved, MSB. PCM is interleaved, packed, little endian.
     * \param nsamples number of sample periods represented in the #data.
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*process)(void *userdata, double extra_db, uint8_t *data, int nsamples);


    /*
     * Clean up any state allocated in #setup.
     *
     * \param userdata the userdata value from the #RAAT__OutputSoftwareVolume structure
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*teardown)(void *userdata);
} RAAT__OutputSoftwareVolume;


/**
 * A virtual-function table that represents the audio hardware associated with a RAAT device.
 *
 * Please discuss with your Roon contact before attempting to implement this interface from scratch to determine whether
 * that is the best course of action.
 *
 * See <tt>raat_output_plugin_null.c</tt> for a good example of a minimal output plugin implementation.
 *
 * ### Threading rules:
 *
 * - All of the functions in this interface will be called from multiple threads concurrently. Lock accordingly.
 * - Callback functions may be called from any thread.
 * 
 * ### The setup token:
 *
 * This interface provides a mechanism for controlling ownership of the audio hardware. This allows audio hardware
 * to be shared amongst multiple instances of the Roon app, and to be shared amongst multiple audio applications 
 * running on the same piece of hardware.
 *
 * Coordination between mutliple audio applications is primarily handled using the \ref raat_plugin_source_selection. See that 
 * document for more information on handling that use case.
 *
 * When a Roon application wants to take control of the audio hardware, it calls the setup function, providing the 
 * audio stream parameters. If someone else currently controls the audio hardware, they are forced to relinquish control, 
 * and informed that they have lost control of the hardware via their <tt>cb_lost</tt> callback.
 *
 * Once the hardware has been configured and is playing silence, <tt>cb_setup</tt> is called, informing the new owner of
 * the audio hardware that they are good to go. They are also given a token, which represents their ownership of the hardware.
 * The token is used on subsequent calls to the plugin. This ensures that only the current owner of the hardware can 
 * manipulate it.
 *
 * The token mechanism is necessary because control of audio hardware can be lost asynchronously. This means, it's possible for the Roon
 * application to attempt to manipulate the output plugin after it has lost control, but before it has been informed that control was
 * lost. The token provides a tidy solution to that race condition.
 *
 * It is crucial that function implementations check to make sure that tokens are current before taking action. 
 *
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
     * store it for the duration of the session. Consider using the message mechanism also in this plugin interface if you 
     * need to expose information that changes over time.
     *
     * Supported Fields:
     *
     * {
     *     "mqa_capabilities":                          [ "renderer", "decoder" ],      // This is required for devices which support MQA
     *     "refresh_supported_formats_before_playback": true,                           // This must be used used on devices who's format support can change on the fly
     *     "config":                                    { ... },                        // Reports  the configuration passed to the output plugin at startup
     *     "alsa_device":                               { ... },                        // Contains some opaque information specific to ALSA. The ALSA output plugin populates this for you, don't worry about it.
     * }
     *
     * \param self the output plugin instance
     * \param out_info pointer to a json_t that will receive the info object. This must be a JSON object.
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*get_info)(void *self, json_t **out_info);

    /**
     * List the native formats supported by the hardware. 
     *
     * If the user attempts to play an unsupported format, Roon will convert it to a supported format before sending the audio stream
     * to this device.
     *
     * out_formats is allocated using <tt>alloc</tt>, and must be freed by the caller
     *
     * \param self the output plugin
     * \param alloc an allocator that will be used to allocate the returned data
     * \param out_nformats output parameter that will receive the number of #RAAT__StreamFormat structures that was returned
     * \param out_formats output parameter that will receive <tt>*out_nformats</tt> #RAAT__StreamFormat structures.
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*get_supported_formats)(void *self, RC__Allocator *alloc, size_t *out_nformats, RAAT__StreamFormat/*?*/ **out_formats);

    /**
     * Configure the hardware to the specified format and begin playing silence. 
     *
     * Implementation Notes:
     *
     * - cb_setup must be called exactly once for each call to setup
     * - cb_setup must not be called until the clocking mechanism is able to provide good values in #get_local_time, or a failure has occurred
     * - cb_setup must be thread-safe--it can be called from multiple threads at the same time.
     * - cb_setup need not be re-entrant--neither cb_setup nor cb_lost callbacks will call back into setup.
     *
     * \param self the output plugin
     * \param format a #RAAT__StreamFormat structure that represents the format of the audio stream
     * \param cb_setup a callback that will be invoked once the stream is up and running
     * \param cb_setup_userdata an opaque pointer-sized value that will be passed to cb_setup when it is invoked
     * \param cb_lost a callback that will be invoked if this setup operation is terminated asynchronously
     * \param cb_lost_userdata an opaque pointer-sized value that will be passed to cb_lost when it is invoked
     */
    void (*setup)(void *self, RAAT__StreamFormat *format, RAAT__OutputSetupCallback cb_setup, void *cb_setup_userdata, RAAT__OutputLostCallback cb_lost, void *cb_lost_userdata);

    /**
     * If audio is playing, stop playing.
     *
     * \param self the output plugin
     * \param token a token received from the setup callback. See the detailed description for more information.
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*teardown)(void *self, int token);

    /**
     * Begin playback of content from streamtime at time.
     *
     * This function must check that <tt>token</tt> represents the currrent setup token for this output, and fail otherwise.
     *
     * \param self the output plugin
     * \param token a token received from the setup callback. See the detailed description for more information.
     * \param walltime the time to begin playback, in terms of the nanosecond resolution clock exposed by #get_local_time
     * \param streamtime the sample offset in the stream that corresponds to <tt>walltime</tt>. This will be passed along to #RAAT__stream_read.
     * \param stream the #RAAT__Stream instance that contains the audio data that we are playing.
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*start)(void *self, int token, int64_t walltime, int64_t streamtime, RAAT__Stream *stream);

    /**
     * Samples the current time, at nano-second resolution. It is important that this time be based on the same
     * clock source as the audio stream itself. Do not use the OS's real-time clock to implement this function unless
     * the audio hardware is actually slaved to that clock source.
     *
     * This function must check that <tt>token</tt> represents the currrent setup token for this output, and fail otherwise.
     *
     * \param self the output plugin
     * \param token a token received from the setup callback. See the detailed description for more information.
     * \param out_time output parameter that will receive the current time
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*get_local_time)(void *self, int token, int64_t *out_time);

    /**
     * Notifies the output about a remote master clock and its relationship to the local clock (as reported by <tt>get_local_time</tt>).
     *
     * This function must check that <tt>token</tt> represents the currrent setup token for this output, and fail otherwise.
     *
     * \param self the output plugin
     * \param token a token received from the setup callback. See the detailed description for more information.
     * \param clock_offset the offset between the remote clock and the local clock, measured recently
     * \param new_source true if the remote clock is different from the previous remote clock, or if this is the first piece of timing information
     *                   received from a remote clock. This causes the output to clear out any past information and start fresh.
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*set_remote_time)(void *self, int token, int64_t clock_offset, bool new_source);

    /**
     * Cease playback immediately and begin playing silence.
     *
     * This function must check that <tt>token</tt> represents the currrent setup token for this output, and fail otherwise.
     *
     * \param self the output plugin
     * \param token a token received from the setup callback. See the detailed description for more information.
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*stop)(void *self, int token);

    /**
     * Force a teardown, regardless of the current setup token.
     *
     * This can be used by other plugins (e.g. source selection) to gracefully take the audio device away from Roon. 
     *
     * Before this function returns, Roon will have relinquished its hold on the audio device.
     *
     * The reason argument is an extension point that allows information to bubble up to the UI. 
     *
     * Examples:
     *
     * <pre>
     *    { reason: "source_deselected" }
     *    { reason: "reboot" }
     *    { reason: "device_not_available" }
     *    { reason: "standby" }
     * </pre>
     *
     * If you think there's additional information that should be shown to the user, reach out to your contact 
     * at Roon to discuss.
     *
     * \param self the output plugin
     * \param reason a JSON object that describes why the output is being torn down.
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*force_teardown)(void *self, json_t *reason);
    
    /**
     * This function is optional, and should only be implemented if the output plugin is capable of applying 
     * a software volume adjustment. Hardware partners are not expected to implement this function.
     *
     * This function is called by a software volume plugin in order to apply a volume adjustment.
     *
     * \param self the output plugin
     * \param volume the volume control implementation
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*set_software_volume)(void *self, RAAT__OutputSoftwareVolume *volume);

    /**
     * Update the portion of the signal path related to software volume control.
     *
     * This function is called by a software volume plugin.
     *
     * \param self the output plugin
     * \param signal_path a JSON_ARRAY typed json_t* value or NULL if a software volume control is not part of the signal path at this time.
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*set_software_volume_signal_path)(void *self, json_t *signal_path);

    /**
     * This function is optional. It must be implemented if this output plugin supports receiving JSON messages, 
     *
     * The message mechanism allows for this interface to be extended with vendor or configuration specific functionality, since
     * JSON messages can be safely passed between Roon and the plugin without requiring that a protocol be defined up front.
     *
     * Currently Roon sends no messages. If you think that your plugin requires custom messages, reach out to your contact at 
     * Roon to discuss how to proceed.
     */
    RC__Status (*send_message)(void *self, json_t *message);

    /**
     * Register a message listener with the output plugin.
     *
     * This function is optional. It must be implemented if this output plugin supports sending JSON messages.
     *
     * Use the #RAAT__OutputMessageListeners helper structure + its associated functions to implement this function (see sample code).
     *
     * This method is used to implement the \ref signal_path feature. See that document for more information on the format of signal path messages.
     *
     * \param self the output plugin
     * \param cb a callback that will receive messages emitted by this output plugin
     * \param cb_userdata an opaque pointer-sized value that will be passed to <tt>cb</tt>
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*add_message_listener)(void *self, RAAT__OutputMessageCallback cb, void *cb_userdata);

    /**
     * Unregister a message listener with the output plugin.
     *
     * This function is optional. It must be implemented if this output plugin supports sending JSON messages.
     *
     * \param self the output plugin
     * \param cb the callback to be removed
     * \param cb_userdata the cb_userdata that goes with the callback
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*remove_message_listener)(void *self, RAAT__OutputMessageCallback cb, void *cb_userdata);

    /**
     * OPTIONAL: Samples the current ioutput delay, at nano-second resolution. 
     *
     * This function must check that <tt>token</tt> represents the currrent setup token for this output, and fail otherwise.
     *
     * \param self the output plugin
     * \param token a token received from the setup callback. See the detailed description for more information.
     * \param out_delay output parameter that will receive the current delay
     *
     * \retval The return status. (See \ref rc_status for more information)
     */
    RC__Status (*get_output_delay)(void *self, int token, int64_t *out_delay);

} RAAT__OutputPlugin;   

/**
 * Helper for managing a collection of output message listeners + invoking the listeners properly.
 */
typedef struct {
    RC__Allocator      *alloc;
    uv_mutex_t          lock;
    RC__List            listeners;
} RAAT__OutputMessageListeners;

/**
 * Initialize a collection of output message listener callbacks
 *
 * \param self a pointer to a #RAAT__OutputMessageListeners 
 * \param alloc memory allocator to use for keeping track of the list of listeners
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status 
RAAT__output_message_listeners_init               (RAAT__OutputMessageListeners    *self, 
                                                   RC__Allocator                   *alloc);

/**
 * Add a handler to a #RAAT__OutputMessageListeners callback list
 *
 * \param self a pointer to a #RAAT__OutputMessageListeners 
 * \param cb the callback to add
 * \param userdata an opaque pointer-sized value that will be passed to the callback
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status 
RAAT__output_message_listeners_add                (RAAT__OutputMessageListeners    *self, 
                                                   RAAT__OutputMessageCallback      cb, 
                                                   void                            *userdata);

/*
 * Remove a output message listener callback
 *
 * \param self a pointer to a #RAAT__OutputMessageListeners 
 * \param cb the callback to remove
 * \param userdata the userdata to remove
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC__Status 
RAAT__output_message_listeners_remove             (RAAT__OutputMessageListeners    *self, 
                                                   RAAT__OutputMessageCallback      cb, 
                                                   void                            *userdata);

/**
 * Invoke each callback in a list of output listener callbacks
 *
 * \param self a pointer to a #RAAT__OutputMessageListeners 
 * \param message a JSON message that will be passed to each callback
 */
RC__Status 
RAAT__output_message_listeners_invoke             (RAAT__OutputMessageListeners    *self, 
                                                   json_t                          *message);

/**
 * Destroy a set of output listener callbacks
 *
 * \param self a pointer to a #RAAT__OutputMessageListeners 
 */
void 
RAAT__output_message_listeners_destroy            (RAAT__OutputMessageListeners    *self);

 /** @} */

#ifdef __cplusplus
}
#endif

#endif
