//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_device.h" 

#include "raat_plugin_volume_dummy.h"
#include "raat_plugin_output_null.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

void log_to_stderr(RAAT__LogEntry *entry, void *userdata) {
    fprintf(stderr, "[%07d] [t%lld] %4.3f %s %s\n", entry->seq, (long long)uv_thread_self(), (double)entry->time / 1000000.0, RAAT__LogLevelString[entry->level], entry->message);
}

void rcfail(RC__Status status, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); 
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, ": %s\n", RC__status_to_string(status));
    exit(1);
}

void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); 
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    exit(1);
}

void usage(void) {
    fprintf(stderr, "USAGE: raat_null_sample");
    fprintf(stderr, "\n");
}

void client_types_cb(void *userdata, int n_client_types, char **client_types) {
    int i;
    fprintf(stderr, "Got %d client types:\n", n_client_types);
    for (i = 0; i < n_client_types; i++) {
        fprintf(stderr, "    %s\n", client_types[i]);
    }
}

int main(int argc, char **argv) {
    RAAT__Device *device;
    RAAT__Log    *log;
    RAAT__Info   *info;
    json_t       *config;
    RC__Status    status;


    //
    // Initialize the logger.
    //
    // RAAT__Log implements a 256kb rolling log in RAM. This can be queried using the raatool tool and the Roon 
    // application. 
    //
    status = RAAT__log_new(RC__ALLOCATOR_DEFAULT, RAAT__LOG_DEFAULT_SIZE, &log);
    if (!RC__STATUS_IS_SUCCESS(status)) { 
        rcfail(status, "failure initializing log");
        return 1;
    }

    //
    // (Optional) add a log callback that redirects log output to standard error, for convenience.
    //
    RAAT__log_add_callback(log, log_to_stderr, NULL);

    //
    // Create the RAAT__Device object. This is the toplevel object that implements the core
    // RAAT functionality
    //
    status = RAAT__device_new(RC__ALLOCATOR_DEFAULT, log, &device);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        rcfail(status, "failure initializing log");
        return 1;
    }

    //
    // Instantiate and configure an output plugin. 
    //
    // By convention, we pass in a json_t* with
    // configuration data for the plugin. This can be an empty json object (as in this example), 
    // or may configure many aspects of the plugin's behavior (for example, for the ALSA output 
    // plugin).
    //
    config = json_object();
    RAAT__OutputPlugin    *output_plugin;
    status = RAAT__null_output_plugin_new(RC__ALLOCATOR_DEFAULT, device, config, &output_plugin);
    if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to initialize null output"); }
    RAAT__device_set_output_plugin(device, output_plugin);
    json_decref(config);

    //
    // Instantiate and configure a volume plugin.
    //
    // The same process could be repeated for source selection and transport plugins if desired.
    //
    config = json_object();
    RAAT__VolumePlugin    *volume_plugin           = NULL;
    status = RAAT__dummy_volume_plugin_new(RC__ALLOCATOR_DEFAULT, device, config, &volume_plugin);
    if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to initialize dummy volume"); }
    RAAT__device_set_volume_plugin(device, volume_plugin);
    json_decref(config);

    //
    // The Info dictionary contains the basic static information about a RAAT device. It's 
    // conveyed to Roon as part of the discovery process. In order for a RAAT__Device to be 
    // valid, it must have certain fields set.
    //
    info   = RAAT__device_get_info(device);

    //
    // Unique ID (required field)
    //  
    // The unique ID is a guid in string form. It must be globally unique across
    // all devices in the world--Roon will become confused if two devices using the same
    // unique id appear on the same network.
    //
    // If you don't have a good opportunity to generate a guid during the manufacturing process,
    // or on first run (how RoonSpeakers apps do it internally), consider formatting the MAC 
    // address into a guid. For example:
    //
    // 01:02:03:04:05:06 could become 00000000-0000-0000-0000010203040506.
    //
    status = RAAT__info_set(info, RAAT__INFO_KEY_UNIQUE_ID, "7d81dd2e-a4ea-407a-a5c0-1a06c89b2776");
    if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to set uniqueid"); }

    //
    // Vendor (required field)
    // 
    // This string will be displayed to the user. It should describe the vendor or manufacturer
    // of the device.
    //  
    status = RAAT__info_set(info, RAAT__INFO_KEY_VENDOR,      "Roon Labs LLC");
    if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to set vendor"); }

    //
    // Model (required field)
    // 
    // This string will be displayed to the user. It should describe the model name of the device.
    //  
    status = RAAT__info_set(info, RAAT__INFO_KEY_MODEL,       "Null Device Sample");
    if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to set model"); }

    //
    // Output Name (optional field)
    // 
    // This field should only be used for products that expose multiple RAAT devices. It 
    // represents a user-facing name for the output, for example "Output 1" or "Analog Output".
    //
    // In the sample code, we are not setting this because this sample product only exposes one device.
    //  
    // status = RAAT__info_set(info, RAAT__INFO_KEY_OUTPUT_NAME,  "Analog Output");
    // if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to set name"); }
    //
    
    //
    // Auto Name (optional field)
    // 
    // This field determines the default name for the device when a Roon user first enables it.
    //
    // This should not be used to describe the device hardware (that is what vendor, model, and vendor_model are for).
    //
    // This should only be used in situations where the user has provided a name for the device via another app
    // or configuration interface. It helps smooth the process of getting the device into Roon.
    //
    // status = RAAT__info_set(info, RAAT__INFO_KEY_AUTO_Name,  "Living Room");
    // if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to set auto name"); }

    //
    // Config URL (optional field)
    // 
    // This field allows you to link to a web URL on your device that allows the user to perform
    // some sort of setup, configuration, firmware updates, etc.
    //
    // Devices should not attempt to guess their own IP address, instead, they should embed __SELF__.
    // The Roon server will replace __SELF__ with the IP address that it is using to communicate with 
    // the device.
    //
    // status = RAAT__info_set(info, RAAT__INFO_KEY_CONFIG_URL,  "http://__SELF__/config/raat");
    // if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to set config url"); }

    //
    // Serial Number (optional field)
    // 
    // Serial number for the device. This string is optional. For hardware devices that don't have 
    // a serial number or don't know their serial number, the MAC address is a good fallback.
    //
    status = RAAT__info_set(info, RAAT__INFO_KEY_SERIAL,      "123456ABCDEF");
    if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to set serial"); }

    //
    // Hardware/Firmware version (required field)
    // 
    // This string represents version information for the hardware and firmware. This will be displayed to the user
    // and included in Roon's logs so it's accessible to our support staff.
    //
    status = RAAT__info_set(info, RAAT__INFO_KEY_VERSION,     "Rev 2, Firmware XXX");
    if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to set version"); }

    //
    // Register for information about connected client types (optional, only if you plan to do something with it)
    //
    RAAT__device_set_client_types_callback(device, client_types_cb, log);

    //
    // Run the device. This function will not return unless a failure occurs or someone calls RAAT__device_stop.
    // 
    status = RAAT__device_run(device);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        rcfail(status, "failure running");
        return 1;
    }

    return 0;
}
