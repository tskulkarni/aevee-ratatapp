#include "raat_base.h"
#include "rc_status.h"

#include <uv.h>

extern const char * RAAT__log_status_to_string(RC__Status status);
extern const char * RAAT__discovery_status_to_string(RC__Status status);
extern const char * RAAT__info_status_to_string(RC__Status status);
extern const char * RAAT__device_status_to_string(RC__Status status);
extern const char * RAAT__server_status_to_string(RC__Status status);
extern const char * RAAT__client_status_to_string(RC__Status status);
extern const char * RAAT__session_status_to_string(RC__Status status);
extern const char * RAAT__output_plugin_status_to_string(RC__Status status);
extern const char * RAAT__volume_plugin_status_to_string(RC__Status status);
extern const char * RAAT__source_selection_plugin_status_to_string(RC__Status status);
extern const char * RAAT__transport_plugin_status_to_string(RC__Status status);

static uv_once_t  status_init_once      = UV_ONCE_INIT;

static void static_init_cb(void) {
    RC__status_register(RAAT__LOG_STATUS_BASE, RAAT__LOG_STATUS_MAX, RAAT__log_status_to_string);
    RC__status_register(RAAT__DISCOVERY_STATUS_BASE, RAAT__DISCOVERY_STATUS_MAX, RAAT__discovery_status_to_string);
    RC__status_register(RAAT__INFO_STATUS_BASE, RAAT__INFO_STATUS_MAX, RAAT__info_status_to_string);
    RC__status_register(RAAT__CLIENT_STATUS_BASE, RAAT__CLIENT_STATUS_MAX, RAAT__client_status_to_string);
    RC__status_register(RAAT__SESSION_STATUS_BASE, RAAT__SESSION_STATUS_MAX, RAAT__session_status_to_string);
    RC__status_register(RAAT__DEVICE_STATUS_BASE, RAAT__DEVICE_STATUS_MAX, RAAT__device_status_to_string);
    RC__status_register(RAAT__SERVER_STATUS_BASE, RAAT__SERVER_STATUS_MAX, RAAT__server_status_to_string);
    RC__status_register(RAAT__OUTPUT_PLUGIN_STATUS_BASE, RAAT__OUTPUT_PLUGIN_STATUS_MAX, RAAT__output_plugin_status_to_string);
    RC__status_register(RAAT__VOLUME_PLUGIN_STATUS_BASE, RAAT__VOLUME_PLUGIN_STATUS_MAX, RAAT__volume_plugin_status_to_string);
    RC__status_register(RAAT__SOURCE_SELECTION_PLUGIN_STATUS_BASE, RAAT__SOURCE_SELECTION_PLUGIN_STATUS_MAX, RAAT__source_selection_plugin_status_to_string);
    RC__status_register(RAAT__TRANSPORT_PLUGIN_STATUS_BASE, RAAT__TRANSPORT_PLUGIN_STATUS_MAX, RAAT__transport_plugin_status_to_string);
}

void RAAT__static_init(void) {
    uv_once(&status_init_once, static_init_cb);
}
