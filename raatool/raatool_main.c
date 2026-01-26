//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_discovery.h" 
#include "raat_log.h" 
#include "raat_info.h" 
#include "rc_guid.h"
#include "rc_dict.h"
#include "raat_client.h"
#include "raat_base.h"
#include "rc_netutil.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

#include <uv.h>
#include <jansson.h>

static RAAT__Log       *g_log;
static RAAT__Discovery *discovery;
static uv_loop_t        loop;
static RC__Allocator   *alloc;

static bool verbose;

typedef enum {
    CMD_LIST            = 1,
    CMD_LOGDUMP         = 2,
    CMD_LOGCAT          = 3,
    CMD_INFO            = 4,
    CMD_DISCOVERY       = 5,
    CMD_DISCOVERYQUERY  = 6,
    CMD_RUN             = 7,
    CMD_EXIT            = 8,
} Command;

static Command cmd;                             // the command that raatool was invoked with
static const char      *device_id;              // for CMD_LOGDUMP, CMD_LOGCAT, CMD_INFO
static RC__Dict         conns;                  // for CMD_LOGDUMP, CMD_LOGCAT, CMD_INFO
static RC__Dict         listed;                 // for CMD_LIST
static uv_timer_t       list_timeout;           // for CMD_LIST
static int              min_seq = -1;           // for CMD_LOGCAT
static uv_timer_t       logcat_timer;           // for CMD_LOGCAT
static const char      *script;                 // for CMD_RUN

#define RAAT__CURRENT_LOG g_log

void log_to_stderr(RAAT__LogEntry *entry, void *userdata) {
    fprintf(stderr, "[RAATOOL] [%07d] %4.3f %s %s\n", entry->seq, (double)entry->time / 1000000.0, RAAT__LogLevelString[entry->level], entry->message);
}

void usage() {
    fprintf(stderr, "USAGE raatool [globaloptions] <command> [options]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "GLOBAL OPTIONS\n");
    fprintf(stderr, "   --verbose,-v                     make responses more verbose");
    fprintf(stderr, "\n");
    fprintf(stderr, "COMMANDS\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "    list                            list devices on the local LAN\n");
    fprintf(stderr, "    version                         print out the verison of raatool that's running, then exit\n");
    fprintf(stderr, "    info <device id>                display the info dictionary for the specified device id\n");
    fprintf(stderr, "    logdump <device id>             dump log to stdout\n");
    fprintf(stderr, "    logcat <device id>              tail the log to stdout\n");
    fprintf(stderr, "    discovery                       dump discovery packets on the network passively\n");
    fprintf(stderr, "    discoveryquery                  send a query, then dump discovery packets on the network passively\n");
    fprintf(stderr, "    run <device id> <script>        run a lua script on the device, redirecting output back to raatool\n");
    fprintf(stderr, "    exit <device id>                exit the RAAT process on the device\n");
    fprintf(stderr, "\n");
}

void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); 
    vfprintf(stderr, fmt, ap);
    exit(1);
}

void uvfail(int status, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); 
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, ": %s\n", uv_strerror(status));
    exit(1);
}

void rcfail(RC__Status status, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); 
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, ": %s\n", RC__status_to_string(status));
    exit(1);
}

typedef struct {
    char *unique_id;
    char displayaddr[RC__MAX_ADDR_LEN];
    RAAT__Client *client;
} ClientConnection;

static RC__DictConfig conns_dict_config = {
    RC__dict_str_hash,
    RC__dict_str_equal,
    NULL,
    NULL,       // XXX: destroy conn (if needed)
};

static RC__DictConfig str_dict_config = {
    RC__dict_str_hash,
    RC__dict_str_equal,
    RC__dict_str_destroy,
    NULL
};

static void disconnect_cb(RAAT__Client *client, void *userdata) {
    ClientConnection *conn = userdata;
    RAAT__TRACE("[raatool] disconnected from %s", conn->displayaddr);
}

static void getinfo_cb(RAAT__Client *client, uint8_t flags, json_t *response, void *userdata) {
    const char *status;

    if (!response) {
        printf("failed to get info\n");
        exit(1);
    }

    status = json_string_value(json_object_get(response, "status"));
    if (status && !strcmp(status, "Success")) {
        json_t *info = json_object_get(response, "info");
        if (json_is_object(info)) {
            const char *key;
            json_t *value;
            json_object_foreach(info, key, value) {
                if (json_is_string(value)) {
                    printf("%s: %s\n", key, json_string_value(value));
                }
            }
        }
        exit(0);
    }

    printf("invalid info\n");
    exit(1);
}

static void getlog_cb(RAAT__Client *client, uint8_t flags, json_t *response, void *userdata) {
    const char *status;

    if (!response) return;

    status = json_string_value(json_object_get(response, "status"));
    if (status && !strcmp(status, "Success")) {
        json_t *entries = json_object_get(response, "entries");
        if (entries) {
            size_t index;
            json_t *value;
            json_array_foreach(entries, index, value) {
                if (json_is_object(value)) {
                    json_t *seq_val   = json_object_get(value, "seq");
                    json_t *level_val = json_object_get(value, "level");
                    json_t *time_val  = json_object_get(value, "time");
                    json_t *text_val  = json_object_get(value, "text");
                    int64_t time;
                    int seq,next_seq;
                    const char *text, *level;

                    if (!json_is_integer(seq_val) || !json_is_string(level_val) || !json_is_integer(time_val) || !json_is_string(text_val)) continue;

                    seq   = (int)json_integer_value(seq_val);
                    level = json_string_value(level_val);
                    time  = (int64_t)json_integer_value(time_val);
                    text  = json_string_value(text_val);

                    if (seq == INT_MAX) next_seq = 0;
                    else                next_seq = seq + 1;

                    if (next_seq > min_seq || (next_seq < (INT_MAX / 2) && min_seq > (INT_MAX / 2)))
                        min_seq = next_seq;

                    printf("[%07d] %4.3f %s %s\n", seq, (double)time / 1000000.0, level, text);
                }
            }
        }
    }

    if (cmd == CMD_LOGDUMP) exit(0);
}

void logcat_timer_cb(uv_timer_t* timer) {
    ClientConnection *conn = timer->data;
    json_t *req_json = json_object();
    json_object_set_new(req_json, "request", json_string("get_log"));
    if (min_seq != -1) {
        json_object_set_new(req_json, "min_seq", json_integer(min_seq));
    }
    RAAT__client_request(conn->client, req_json, getlog_cb, NULL);
    json_decref(req_json);
}

static void connect_cb(RAAT__Client *client, RC__Status status, int uvrc, void *userdata) {
    fprintf(stderr, "connect cb\n");
    ClientConnection *conn = userdata;
    if (status != RC__STATUS_SUCCESS) {
        fprintf(stderr, "Connection failed\n");
        exit(1);
    }

    if (cmd == CMD_LOGDUMP) {
        json_t *req_json = json_object();
        json_object_set_new(req_json, "request", json_string("get_log"));
        RAAT__client_request(client, req_json, getlog_cb, NULL);
        json_decref(req_json);
    }

    if (cmd == CMD_INFO) {
        json_t *req_json = json_object();
        json_object_set_new(req_json, "request", json_string("get_info"));
        RAAT__client_request(client, req_json, getinfo_cb, NULL);
        json_decref(req_json);
    }

    if (cmd == CMD_LOGCAT) {
        uv_timer_init(&loop, &logcat_timer);
        logcat_timer.data = conn;
        uv_timer_start(&logcat_timer, logcat_timer_cb, 0, 250);
    }

    if (cmd == CMD_RUN) {
        json_t *req_json = json_object();
        json_object_set_new(req_json, "request", json_string("load_script"));
        json_object_set_new(req_json, "script", json_string(script));
        json_object_set_new(req_json, "sha256", json_string("12345"));
        RAAT__client_request(client, req_json, getlog_cb, NULL);
        json_decref(req_json);
    }

    if (cmd == CMD_EXIT) {
        json_t *req_json = json_object();
        json_object_set_new(req_json, "request", json_string("load_script"));
        json_object_set_new(req_json, "script", json_string("os.exit(1)\n"));
        json_object_set_new(req_json, "sha256", json_string("12345"));
        RAAT__client_request(client, req_json, getlog_cb, NULL);
        json_decref(req_json);
    }
}

static void tcp_connect(RAAT__DiscoveryMessage *message, struct sockaddr_storage *addr) {
    RC__Status status;
    ClientConnection *conn;
    const char *unique_id = RAAT__discovery_message_get(message, "unique_id");

    if (RC__dict_lookup(&conns, (void*)unique_id)) return;       // already connected to this device

    conn = RC__new0(alloc, ClientConnection, 1);
    RC__ASSERT(conn != NULL);

    status = RAAT__client_new(alloc, g_log, &loop, &conn->client);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        RC__free(alloc, conn);
        RAAT__ERROR("[raatool] error creating client: %s", RC__status_to_string(status));
        return;
    }

    RC__sockaddr_to_string(addr, conn->displayaddr);

    {
        RC__String string;
        RC__string_init(&string, alloc);
        RAAT__discovery_message_append_to_string(message, &string);
        RAAT__INFO("[raatool] Connecting to device %s %s", string.str, conn->displayaddr);
        RC__string_destroy(&string);
    }

    RAAT__client_set_disconnected_callback(conn->client, disconnect_cb, conn);
    status = RAAT__client_connect(conn->client, (struct sockaddr*)addr, connect_cb, conn);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        RAAT__ERROR("[raatool] error initiating client connection: %s", RC__status_to_string(status));
        RAAT__client_delete(conn->client);
        RC__free(alloc, conn);
        return;
    }

    conn->unique_id = RC__allocator_strdup(alloc, unique_id);
    RC__dict_insert(&conns, conn->unique_id, conn);
}

static void ev_discoverymessage(RAAT__DiscoveryMessage *message) {
    const char *service_id_str       = RAAT__discovery_message_get(message, "service_id");
    const char *tcp_port_str         = RAAT__discovery_message_get(message, "tcp_port");
    const char *protocol_version_str = RAAT__discovery_message_get(message, "protocol_version");
    const char *unique_id_str        = RAAT__discovery_message_get(message, "unique_id");

    int tcp_port;
    int protocol_version;
    struct sockaddr_storage tcp_addr;
    RC__Guid raat_service_guid;
    RC__Guid msg_service_guid;
    char addr[RC__MAX_ADDR_LEN];

    tcp_addr = *RAAT__discovery_message_get_source_addr(message);
    RC__sockaddr_to_string(&tcp_addr, addr);

    if (verbose) {
        RC__String string;
        RC__string_init(&string, alloc);
        RAAT__discovery_message_append_to_string(message, &string);
        //RAAT__TRACE("[raatool] got discovery message from %s: %s", addr, string.str);
        RC__string_destroy(&string);
    }

    if (cmd == CMD_DISCOVERY || cmd == CMD_DISCOVERYQUERY) {
        RC__String string;
        RC__string_init(&string, alloc);
        RAAT__discovery_message_append_to_string(message, &string);
        printf("[%s] %s\n", addr, string.str);
        RC__string_destroy(&string);
        return;
    } 

    if (service_id_str == NULL || tcp_port_str == NULL || protocol_version_str == NULL || unique_id_str == NULL) return;

    if (RC__guid_init_string(&raat_service_guid, RAAT__SERVICE_GUID_STRING)) { return; }

    if (service_id_str == NULL || 0 != RC__guid_init_string(&msg_service_guid, service_id_str)) return;        // ignore devices without a parseable service id
    if (!RC__guid_equals(&msg_service_guid, &raat_service_guid)) return;                                       // ignore messages not for our service id

    tcp_port = atoi(tcp_port_str);
    if (tcp_port == 0) return;

    protocol_version = atoi(protocol_version_str);
    if (protocol_version == 0) return;

    if (protocol_version != RAAT__PROTOCOL_VERSION) return;

    if (tcp_addr.ss_family == AF_INET) {
        ((struct sockaddr_in*)&tcp_addr)->sin_port = htons((uint16_t)tcp_port);
    } else if (tcp_addr.ss_family == AF_INET6) {
        ((struct sockaddr_in6*)&tcp_addr)->sin6_port = htons((uint16_t)tcp_port);
    } else {
        return;
    }

    if (cmd == CMD_LIST) {
        const char *unique_id = RAAT__discovery_message_get(message, "unique_id");
        const char *vendor = RAAT__discovery_message_get(message, "vendor");
        const char *model = RAAT__discovery_message_get(message, "model");
        const char *serial = RAAT__discovery_message_get(message, "serial");

        if (serial == NULL) serial = "N/A";
        if (vendor == NULL || model == NULL || unique_id == NULL) return;

        if (!RC__dict_lookup(&listed, (void*)unique_id)) {
            RC__dict_insert(&listed, RC__allocator_strdup(alloc, unique_id), RC__INT_TO_POINTER(1));
            printf("%-20s %-20s %-15s %-20s %s\n", vendor, model, serial, addr, unique_id);
        }
    } else if (cmd == CMD_LOGCAT || cmd == CMD_LOGDUMP || cmd == CMD_INFO || cmd == CMD_RUN || cmd == CMD_EXIT) {
        const char *unique_id = RAAT__discovery_message_get(message, "unique_id");
        if (unique_id && !strcmp(unique_id, device_id)) {
            tcp_connect(message, &tcp_addr);
        }
    }
}

static void query_cb(RAAT__Discovery *discovery, RAAT__DiscoveryMessage *message, void *userdata)   { ev_discoverymessage(message); }
static void message_cb(RAAT__Discovery *discovery, RAAT__DiscoveryMessage *message, void *userdata) { ev_discoverymessage(message); }

static RC__Status query() {
    RC__Status status;
    RAAT__DiscoveryMessage *message;

    status = RAAT__discovery_message_new(alloc, &message);
    if (!RC__STATUS_IS_SUCCESS(status)) return status;

    // populate fields
    RAAT__discovery_message_set(message, "service_id", RAAT__SERVICE_GUID_STRING);
    status = RAAT__discovery_query(discovery, message, query_cb, NULL);

    RAAT__discovery_message_delete(message);
    return status;
}

static void list_timeout_cb(uv_timer_t *timer) {
    printf("-------------------------------------------------------------------------------------------------------------------\n");
    exit(0);
}

void skip_arg(int i, int *argc, char **argv) {
    *argc = *argc - 1;
    while (i < *argc)
        argv[i] = argv[i+1];
}

int main(int argc, char **argv) {
    RC__Status status;
    int rc;
    alloc = RC__allocator_default(NULL);
    int i;
    bool needs_query = false;

    RAAT__static_init();

    status = RAAT__log_new(alloc, RAAT__LOG_DEFAULT_SIZE, &g_log);
    if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "RAAT__log_new"); }

    rc = uv_loop_init(&loop);
    if (rc) { uvfail(rc, "uv_loop_init"); }

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '-') {
            const char *scopts = argv[i] + 1;
            while (*scopts) {
                switch (*scopts) {
                    case 'v': verbose = true; break;
                    default: fail("Unrecognized argument: %s", argv[i]);
                }
                scopts++;
            }
            skip_arg(i, &argc, argv);
        }
        if (!strcmp(argv[i], "--verbose")) {
            verbose = true;
            skip_arg(i, &argc, argv);
        }
    }

    if (verbose) {
        RAAT__log_add_callback(g_log, log_to_stderr, NULL);
    }

    if (argc == 1) {
        usage();
        exit(1);
    }

    if (!strcmp(argv[1], "logcat")) {
        if (argc != 3) fail("Invalid arguments to 'logcat'");
        cmd      = CMD_LOGCAT;
        device_id = argv[2];
        needs_query = true;

    } else if (!strcmp(argv[1], "logdump")) {
        if (argc != 3) fail("Invalid arguments to 'logdump'");
        cmd      = CMD_LOGDUMP;
        device_id = argv[2];
        needs_query = true;

    } else if (!strcmp(argv[1], "info")) {
        if (argc != 3) fail("Invalid arguments to 'info'");
        cmd      = CMD_INFO;
        device_id = argv[2];
        needs_query = true;

    } else if (!strcmp(argv[1], "discovery")) {
        if (argc != 2) fail("Invalid arguments to 'discovery'");
        cmd      = CMD_DISCOVERY;
        needs_query = false;

    } else if (!strcmp(argv[1], "discoveryquery")) {
        if (argc != 2) fail("Invalid arguments to 'discoveryquery'");
        cmd      = CMD_DISCOVERYQUERY;
        needs_query = true;

    } else if (!strcmp(argv[1], "run")) {
        const char *path;
        FILE *f;
        size_t file_len;

        if (argc != 4) fail("Invalid arguments to 'run'");
        cmd      = CMD_RUN;
        device_id = argv[2];
        path      = argv[3];

        f = fopen(path, "r");
        if (!f) {
            perror("error opening script");
            exit(2);
        }
        fseek(f, 0L, SEEK_END);
        file_len = ftell(f);
        fseek(f, 0L, SEEK_SET);

        script = RC__alloc(alloc, file_len + 1);
        if (1 != fread((void*)script, file_len, 1, f)) {
            perror("eror reading script");
            exit(3);
        }

        fclose(f);

        needs_query = true;

    } else if (!strcmp(argv[1], "exit")) {
        if (argc != 3) fail("Invalid arguments to 'exit'");
        cmd         = CMD_EXIT;
        device_id   = argv[2];
        needs_query = true;

    } else if (!strcmp(argv[1], "version")) {
        printf("RAAT v%s\n", RAAT__VERSION);
        printf("Copyright (C) 2015 Roon Labs LLC\n");
        exit(0);

    } else if (!strcmp(argv[1], "list")) {
        if (argc != 2) fail("Invalid arguments to 'list'");
        cmd = CMD_LIST;
        RC__dict_init(&listed, alloc, &str_dict_config);
        uv_timer_init(&loop, &list_timeout);
        uv_timer_start(&list_timeout, list_timeout_cb, 20000, 0);
        printf("%-20s %-20s %-15s %-20s %s\n", "Vendor", "Model", "Serial", "Address", "Id"); 
        printf("-------------------------------------------------------------------------------------------------------------------\n");
        needs_query = true;

    } else {
        fail("Invalid command: %s", argv[1]);
    }

    RC__dict_init(&conns, alloc, &conns_dict_config);

    status = RAAT__discovery_new(alloc, g_log, &loop, &discovery);
    if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "RAAT__discovery_new"); }

    status = RAAT__discovery_add_message_callback(discovery, message_cb, NULL);
    if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "RAAT__discovery_add_message_callback"); }

    status = RAAT__discovery_start(discovery);
    if (!RC__STATUS_IS_SUCCESS(status)) { rcfail(status, "RAAT__discovery_start"); }

    if (needs_query) {
        query();
    }

    rc = uv_run(&loop, UV_RUN_DEFAULT);
    if (rc) { rcfail(rc, "uv_run"); }

    return 0;
}


