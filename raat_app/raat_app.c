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
#include "rc_guid.h"
#include "raat_base.h"
#include "raat_plugin_volume.h"
#include "raat_plugin_output.h"
#include "raat_plugin_watch.h"
#include "raat_plugin_transport.h"
#include "raat_plugin_source_selection.h"

#include "raat_plugin_volume_dummy.h"
#include "raat_plugin_volume_incremental.h"
#include "raat_plugin_volume_software.h"
#include "raat_plugin_volume_null.h"
#include "raat_plugin_volume_software.h"
#include "raat_plugin_output_null.h"
#include "raat_plugin_output_capture.h"
#include "raat_plugin_source_selection_test.h"
#include "raat_plugin_transport_test.h"

#if defined(PLATFORM_LINUX) && defined(HAVE_ALSA)
#include "raat_plugin_volume_alsa.h"
#include "raat_plugin_output_alsa.h"
#include "raat_plugin_watch_alsa.h"
#endif

#if defined(PLATFORM_MACOSX)
#include "raat_plugin_volume_coreaudio.h"
#include "raat_plugin_output_coreaudio.h"
#endif

#if defined(PLATFORM_WINDOWS)
#include "raat_plugin_output_asio.h"
#include "raat_plugin_output_wasapi.h"
#include "raat_plugin_volume_wasapi.h"
#endif

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <jansson.h>

#define RAAT__CURRENT_LOG log

// global -- mcmurray
extern RAAT__OutputPlugin *g_output_plugin_ptr;

void log_to_stderr(RAAT__LogEntry *entry, void *userdata) {
    fprintf(stderr, "[%07d] [t%lld] %4.3f %s %s\n", entry->seq, (long long)uv_thread_self(), (double)entry->time / 1000000.0, RAAT__LogLevelString[entry->level], entry->message);
	fflush(stderr);
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
    fprintf(stderr, "USAGE: raat_app <configfile>\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Where <configfile> looks like:\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "{\n");
    fprintf(stderr, "    \"vendor\":      \"Roon Labs LLC\",\n");
    fprintf(stderr, "    \"vendor_model\":\"Roon Labs Model\",\n");
    fprintf(stderr, "    \"model\":       \"Model Name\",\n");
    fprintf(stderr, "    \"serial\":      \"Serial Number\",                                      (optional)\n");
    fprintf(stderr, "    \"config_url\":  \"http://__SELF__:8080/path/to/config/ui\",             (optional)\n");
    fprintf(stderr, "    \"auto_name\":   \"User-defined device name (if applicable)\",           (optional)\n");
    fprintf(stderr, "    \"version\":     \"Version Number\",\n");
    fprintf(stderr, "    \"output_name\": \"Output Name\",\n");
    fprintf(stderr, "    \"unique_id\":   \"8f4322e8-4507-4b21-b93d-e808d283cb6a\",\n");
    fprintf(stderr, "    \"output\":     { \"type\": \"coreaudio|alsa|null|capture\", ...}\n");
    fprintf(stderr, "    \"volume\":     { \"type\": \"coreaudio|alsa|null|capture\", \"optional\": true|false, ...}\n");
    fprintf(stderr, "}\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "See the reference documentation for the plugins for a full description of their parameters.");
    fprintf(stderr, "\n");
}

int main(int argc, char **argv) {
    RAAT__Device *device;
    RAAT__Log    *log;
    RAAT__Info   *info;
    RC__Status    status;

    if (argc != 2) {
        usage();
        exit(1);
    }

#if defined(PLATFORM_LINUX)
    signal(SIGPIPE, SIG_IGN);
#endif

    RAAT__static_init();

    const char *configpath = argv[1];
    json_error_t error;
    json_t *config = json_load_file(configpath, 0, &error);
    if (!config) {
        fail("Invalid config %s:%d: %s", configpath, error.line, error.text);
    }

    const char *unique_id      = json_string_value(json_object_get(config, "unique_id"));
    const char *vendor         = json_string_value(json_object_get(config, "vendor"));
    const char *vendor_model   = json_string_value(json_object_get(config, "vendor_model"));
    const char *model          = json_string_value(json_object_get(config, "model"));
    const char *output_name    = json_string_value(json_object_get(config, "output_name"));
    const char *serial         = json_string_value(json_object_get(config, "serial"));
    const char *config_url     = json_string_value(json_object_get(config, "config_url"));
    const char *auto_name      = json_string_value(json_object_get(config, "auto_name"));
    const char *version        = json_string_value(json_object_get(config, "version"));

    if (unique_id   == NULL) fail("Invalid config %s: missing unique_id", configpath);
    if (vendor      == NULL) fail("Invalid config %s: missing vendor", configpath);
    if (model       == NULL) fail("Invalid config %s: missing model", configpath);
    if (version     == NULL) fail("Invalid config %s: missing version", configpath);

    status = RAAT__log_new(RC__ALLOCATOR_DEFAULT, RAAT__LOG_DEFAULT_SIZE, &log);
    if (!RC__STATUS_IS_SUCCESS(status)) { 
        rcfail(status, "failure initializing log");
        return 1;
    }

    RAAT__log_add_callback(log, log_to_stderr, NULL);

    status = RAAT__device_new(RC__ALLOCATOR_DEFAULT, log, &device);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        rcfail(status, "failure initializing log");
        return 1;
    }

    RAAT__OutputPlugin    *output_plugin           = NULL;
    RAAT__VolumePlugin    *volume_plugin           = NULL;
    RAAT__SourceSelectionPlugin       *source_selection_plugin = NULL;
    RAAT__TransportPlugin *transport_plugin        = NULL;
    RAAT__WatchPlugin     *watch_plugin            = NULL;
    (void)watch_plugin;
    bool optional = false;

    // set up output plugin
    json_t *output = json_object_get(config, "output");
    if (output != NULL) {
        const char *type = json_string_value(json_object_get(output, "type"));
        if (type == NULL) fail("Invalid config %s: output requires a type", configpath);
        if (!strcmp(type, "null")) {
            status = RAAT__null_output_plugin_new(RC__ALLOCATOR_DEFAULT, device, output, &output_plugin);
            if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to initialize null output"); }

        } else if (!strcmp(type, "capture")) {
            status = RAAT__capture_output_plugin_new(RC__ALLOCATOR_DEFAULT, device, output, &output_plugin);
            if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to initialize capture output"); }

#ifdef PLATFORM_MACOSX
        } else if (!strcmp(type, "coreaudio")) {
            status = RAAT__coreaudio_output_plugin_new(RC__ALLOCATOR_DEFAULT, device, output, &output_plugin);
            if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to initialize coreaudio output"); }
#endif
#if defined(PLATFORM_LINUX) && defined(HAVE_ALSA)
        } else if (!strcmp(type, "alsa")) {
            status = RAAT__alsa_output_plugin_new(RC__ALLOCATOR_DEFAULT, device, output, &output_plugin);
            if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to initialize alsa output"); }
            g_output_plugin_ptr = output_plugin;
#endif
#ifdef PLATFORM_WINDOWS
        } else if (!strcmp(type, "wasapi")) {
            status = RAAT__wasapi_output_plugin_new(RC__ALLOCATOR_DEFAULT, device, output, &output_plugin);
            if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to initialize wasapi output"); }

        } else if (!strcmp(type, "asio")) {
            status = RAAT__asio_output_plugin_new(RC__ALLOCATOR_DEFAULT, device, output, &output_plugin);
            if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to initialize asio output"); }
#endif
        } else {
            if (type == NULL) fail("Invalid config %s: unknown output type '%s'", type);
        }
    }

    if (output_plugin != NULL) {
        RAAT__device_set_output_plugin(device, output_plugin);
    }

    // set up volume plugin
    json_t *volume = json_object_get(config, "volume");
    optional = json_is_true(json_object_get(volume, "optional"));
    if (volume != NULL) {
        const char *type = json_string_value(json_object_get(volume, "type"));
        if (type == NULL) fail("Invalid config %s: volume requires a type", configpath);
        if (!strcmp(type, "null")) {
            status = RAAT__null_volume_plugin_new(RC__ALLOCATOR_DEFAULT, device, volume, &volume_plugin);
            if (!optional && !RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to initialize null volume"); }

        } else if (!strcmp(type, "bricasti")) {
            status = RAAT__dummy_volume_plugin_new(RC__ALLOCATOR_DEFAULT, device, volume, &volume_plugin);
            if (!optional && !RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to initialize dummy volume"); }

        } else if (!strcmp(type, "incremental")) {
            status = RAAT__incremental_volume_plugin_new(RC__ALLOCATOR_DEFAULT, device, volume, &volume_plugin);
            if (!optional && !RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to initialize incremental volume"); }

        } else if (!strcmp(type, "software")) {
            status = RAAT__software_volume_plugin_new(RC__ALLOCATOR_DEFAULT, device, volume, &volume_plugin);
            if (!optional && !RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to initialize software volume"); }

#ifdef PLATFORM_MACOSX
        } else if (!strcmp(type, "coreaudio")) {
            status = RAAT__coreaudio_volume_plugin_new(RC__ALLOCATOR_DEFAULT, device, volume, &volume_plugin);
            if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to initialize coreaudio volume"); }
#endif
#if defined(PLATFORM_LINUX) && defined(HAVE_ALSA)
        } else if (!strcmp(type, "alsa")) {
            status = RAAT__alsa_volume_plugin_new(RC__ALLOCATOR_DEFAULT, device, volume, &volume_plugin);
            if (!optional && !RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to initialize alsa volume"); }
#endif
#ifdef PLATFORM_WINDOWS
        } else if (!strcmp(type, "wasapi")) {
            status = RAAT__wasapi_volume_plugin_new(RC__ALLOCATOR_DEFAULT, device, volume, &volume_plugin);
            if (!optional && !RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to initialize wasapi volume"); }
#endif
        } else {
            if (type == NULL) fail("Invalid config %s: unknown volume type '%s'", type);
        }
    }

    if (volume_plugin != NULL) {
        RAAT__device_set_volume_plugin(device, volume_plugin);
    }

    // set up source_selection plugin
    json_t *source_selection = json_object_get(config, "source_selection");
    if (source_selection != NULL) {
        const char *type = json_string_value(json_object_get(source_selection, "type"));
        if (type == NULL) fail("Invalid config %s: source_selection requires a type", configpath);
        if (!strcmp(type, "test")) {
            status = RAAT__test_source_selection_plugin_new(RC__ALLOCATOR_DEFAULT, device, source_selection, &source_selection_plugin);
            if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to initialize test source_selection"); }

        } else {
            if (type == NULL) fail("Invalid config %s: unknown source_selection type '%s'", type);
        }
    }

    if (source_selection_plugin != NULL) {
        RAAT__device_set_source_selection_plugin(device, source_selection_plugin);
    }

    // set up transport plugin
    json_t *transport = json_object_get(config, "transport");
    if (transport != NULL) {
        const char *type = json_string_value(json_object_get(transport, "type"));
        if (type == NULL) fail("Invalid config %s: transport requires a type", configpath);
        if (!strcmp(type, "test")) {
            status = RAAT__test_transport_plugin_new(RC__ALLOCATOR_DEFAULT, device, transport, &transport_plugin);
            if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to initialize test transport"); }

        } else {
            if (type == NULL) fail("Invalid config %s: unknown transport type '%s'", type);
        }
    }

    if (transport_plugin != NULL) {
        RAAT__device_set_transport_plugin(device, transport_plugin);
    }

    // set up watch plugin
    json_t *watch = json_object_get(config, "watch");
    if (watch != NULL) {
        const char *type = json_string_value(json_object_get(watch, "type"));
        if (type == NULL) fail("Invalid config %s: watch requires a type", configpath);
        if (!strcmp(type, "none")) {
#if defined(PLATFORM_LINUX) && defined(HAVE_ALSA)
        } else if (!strcmp(type, "alsa")) {
            status = RAAT__alsa_watch_plugin_new(RC__ALLOCATOR_DEFAULT, device, watch, &watch_plugin);
            if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to initialize alsa watch"); }
#endif

        } else {
            if (type == NULL) fail("Invalid config %s: unknown watch type '%s'", type);
        }
    }

    // get pointers to the implementation bits
    info   = RAAT__device_get_info(device);

    status = RAAT__info_set(info, RAAT__INFO_KEY_UNIQUE_ID,   unique_id);
    if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to set uniqueid"); }
    status = RAAT__info_set(info, RAAT__INFO_KEY_VENDOR,      vendor);
    if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to set vendor"); }
    if (vendor_model != NULL) {
        status = RAAT__info_set(info, RAAT__INFO_KEY_VENDOR_MODEL,  vendor_model);
        if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to set vendor_model"); }
    }
    if (output_name != NULL) {
        status = RAAT__info_set(info, RAAT__INFO_KEY_OUTPUT_NAME,  output_name);
        if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to set output name"); }
    }
    if (serial != NULL) {
        status = RAAT__info_set(info, RAAT__INFO_KEY_SERIAL,      serial);
        if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to set serial"); }
    }
    if (config_url != NULL) {
        status = RAAT__info_set(info, RAAT__INFO_KEY_CONFIG_URL,      config_url);
        if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to set config_url"); }
    }
    if (auto_name != NULL) {
        status = RAAT__info_set(info, RAAT__INFO_KEY_AUTO_NAME,      auto_name);
        if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to set auto_name"); }
    }
    status = RAAT__info_set(info, RAAT__INFO_KEY_MODEL,       model);
    if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to set model"); }
    status = RAAT__info_set(info, RAAT__INFO_KEY_VERSION,     version);
    if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "failed to set version"); }

    status = RAAT__device_run(device);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        rcfail(status, "failure running");
        return 1;
    }

    json_decref(config);

    return 0;
}
