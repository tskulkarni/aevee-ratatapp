//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_server.h"
#include "rc_netutil.h"
#include "raat_base.h"
#include "raat_session.h"
#include "rc_list.h"

#include <jansson.h>
#include <string.h>

#include <stdlib.h>

#define RAAT__CURRENT_LOG (self->log)

struct RAAT__Server_s {
    RC__Allocator *alloc;
    RAAT__Info    *info;
    RAAT__Device  *device;
    RAAT__Log     *log;
    uv_loop_t     *loop;
    int            bound_port;
    bool           is_running;
    uv_tcp_t      *listener;

    RC__List       clients;

    uv_mutex_t     lock;
    uv_async_t     async_post;
    RC__List       post_handlers;
};

typedef enum {
    READ_HEADER,
    READ_BODY,
} ClientReadState;

typedef struct {
    RAAT__Server           *server;
    uv_tcp_t               *sock;
    char                    displayaddr[RC__MAX_ADDR_LEN];   // printable client address
    struct sockaddr_storage sockaddr;

    int64_t                 last_active_ms;

    ClientReadState         readstate;
    int                     off;                // offset into header or body
    int                     left;               // bytes remaining until next state
    uint8_t                 header[8];          // header storage
    uint32_t                msgtype;
    uint32_t                bodylen;
    uint8_t                *body;

    RAAT__Session          *session;
    bool                    is_destroying;

    // keepalive state
    bool                    keepalive;
    uv_timer_t             *keepalive_timer;
    int64_t                 keepalive_timeout_ms;
} Client;

typedef struct {
    RAAT__ServerCallback   cb;
    void                  *userdata;
} PostHandler;

static void cb_post(uv_async_t *handle) {
    RAAT__Server *self = handle->data;

    while (true) {
        PostHandler *ph = NULL;

        uv_mutex_lock(&self->lock);
        RC__ListIter it = RC__list_begin(&self->post_handlers);
        if (it != RC__list_end(&self->post_handlers)) {
            ph = (PostHandler*)RC__listiter_data(it);
            RC__list_remove(&self->post_handlers, it);
        }
        uv_mutex_unlock(&self->lock);

        if (ph) {
            ph->cb(self, ph->userdata);
            RC__free(self->alloc, ph);
        } else {
            break;
        }
    }
}

RC_API RC__Status
RAAT__server_post                (RAAT__Server *self, RAAT__ServerCallback cb, void *userdata) {
    PostHandler *ph  = RC__new0(self->alloc, PostHandler, 1);
    ph->cb       = cb;
    ph->userdata = userdata;
    uv_mutex_lock(&self->lock);
    RC__list_push(&self->post_handlers, ph);
    uv_mutex_unlock(&self->lock);
    uv_async_send(&self->async_post);
    return RC__STATUS_SUCCESS;
}

RC_API RC__Status
RAAT__server_new                 (RC__Allocator *alloc, RAAT__Device *device, uv_loop_t *loop, RAAT__Server **out_self) 
{
    RAAT__Server *self;

    RC__ASSERT(out_self != NULL);
    *out_self = NULL;

    self = RC__new0(alloc, RAAT__Server, 1);
    if (self == NULL) {
        return RC__STATUS_OUT_OF_MEMORY;
    }

    self->alloc  = RC__allocator_default(alloc);
    self->log    = RAAT__device_get_log(device);
    self->device = device;
    self->loop  = loop;

    RC__list_init(&self->clients, self->alloc);

    uv_mutex_init(&self->lock);
    RC__list_init(&self->post_handlers, self->alloc);

    *out_self = self;

    return RC__STATUS_SUCCESS;
}

static void handle_close_helper_cb(uv_handle_t *handle) {
    RC__free((RC__Allocator*)handle->data, handle);
}

static void handle_close_helper(RC__Allocator *alloc, void *vhandle) {
    uv_handle_t *handle = vhandle;
    handle->data = alloc;
    uv_close(handle, handle_close_helper_cb);
}

typedef struct {
    RAAT__Server         *self;
    Client               *client;
    uv_async_t            async;
} FinishDestroyClientCallbackState;

static void finish_destroy_client_callback_close_cb(uv_handle_t *handle) {
    FinishDestroyClientCallbackState *state  = handle->data;
    RAAT__Server                     *self   = state->self;
    RC__free(self->alloc, state);
}

static void finish_destroy_client_sock_close_cb(uv_handle_t *handle) {
    Client                           *client = handle->data;
    RAAT__Server                     *self   = client->server;
    RC__free(self->alloc, client->body);
    RC__free(self->alloc, client->sock);
    RC__free(self->alloc, client);
}

static void finish_destroy_client_callback_async_cb(uv_async_t *async) {
    FinishDestroyClientCallbackState *state  = async->data;
    //RAAT__Server                *self   = state->self;
    Client                      *client = state->client;

    uv_close((uv_handle_t*)client->sock, finish_destroy_client_sock_close_cb);
    uv_close((uv_handle_t*)&state->async, finish_destroy_client_callback_close_cb);
}

static void destroy_client(RAAT__Server *self, Client *client) {
    if (client->is_destroying) return;
    client->is_destroying = true;

    RAAT__TRACE("[server] [%s] destroying client", client->displayaddr);

    uv_read_stop((uv_stream_t*)client->sock);

    RAAT__session_delete(client->session);
    RC__list_remove_by_data(&self->clients, client);

    handle_close_helper(self->alloc, client->keepalive_timer);

    // post the rest of the work. This way if session_delete posts anything into our message queue, this comes later
    FinishDestroyClientCallbackState *state = RC__new0(self->alloc, FinishDestroyClientCallbackState, 1);
    uv_async_init(self->loop, &state->async, finish_destroy_client_callback_async_cb);
    state->client     = client;
    state->self       = self;
    state->async.data = state;
    uv_async_send(&state->async);
}

RC_API void 
RAAT__server_delete             (RAAT__Server   *self) 
{
    if (!self) return;
    RC__list_destroy(&self->clients);
    uv_mutex_destroy(&self->lock);
    RC__free(self->alloc, self);
}

RC_API RC__Status
RAAT__server_get_bound_port      (RAAT__Server   *self, int *out_port) {
    RC__ASSERT(self     != NULL);
    RC__ASSERT(out_port != NULL);
    if (self->bound_port == 0) {
        return RAAT__SERVER_GET_BOUND_PORT_STATUS_NOT_STARTED;
    }
    *out_port = self->bound_port;
    return RC__STATUS_SUCCESS;
}

typedef struct {
    Client *client;
    uv_buf_t buf;
    uv_write_t write;
    // buffer contents follow immediately in same malloc block
} WriteState;

static void write_message_cb(uv_write_t *req, int status) {
    WriteState   *state  = req->data;
    Client       *client = state->client;
    RAAT__Server *self   = state->client->server;

    if (status) {
        RAAT__WARNING("[server] failed to write to %s: %s. Closing connection.", state->client->displayaddr, uv_strerror(status));
        destroy_client(self, client);
    }

    RC__free(self->alloc, state);
}

static void write_message(RAAT__Server *self, Client *client, uint32_t msgtype, uint8_t *body, int bodylen) {
    char *msgbuf;
    WriteState *state = RC__alloc0(self->alloc, 8 + bodylen + sizeof(WriteState));
    if (state == NULL) {
        RAAT__WARNING("[server] failed to write to %s: allocation failed. Closing connection.", state->client->displayaddr);
        destroy_client(self, client);
        return;
    }

    msgbuf = ((char*)(state)) + sizeof(*state);

    *(uint32_t*)(msgbuf+0) = htonl(bodylen+8);
    *(uint32_t*)(msgbuf+4) = htonl(msgtype);
    memcpy(msgbuf + 8, body, bodylen);

    state->client = client;
    state->buf.len  = bodylen+8;
    state->buf.base = msgbuf;

    //RAAT__TRACE("writing message of len %d len=%x netlen=%x msgtype=%x netmsgtype=%x", state->buf.len, bodylen+8, htonl(bodylen+8), msgtype, htonl(msgtype));

    state->write.data = state;
    uv_write(&state->write, (uv_stream_t*)client->sock, &state->buf, 1, write_message_cb);
}

static void keepalive_timer_cb(uv_timer_t *handle) {
    Client       *client = handle->data;
    RAAT__Server *self   = client->server;

    int64_t now_ms = RC__now_us() / 1000;

    if (now_ms - client->last_active_ms > client->keepalive_timeout_ms) {
        RAAT__WARNING("[server] client timed out after %dms (timeout=%d)", (int)(now_ms - client->last_active_ms), (int)(client->keepalive_timeout_ms));
        destroy_client(self, client);
        return;
    }

    write_message(self, client, RAAT__LL_KEEPALIVE, NULL, 0);
}

static void respond(RAAT__Server *self, Client *client, uint32_t rid, bool is_final, bool quiet, json_t *response) {
    uint8_t *body;
    char *s;
    int len;

    // client didn't request a response, just drop it
    if (rid == 0xffffffff) {
        return;
    }

    s   = json_dumps(response, 0);
    len = strlen(s);

    body = RC__alloc(self->alloc, 5 + len);
    if (!body) {
        free(s);
        RAAT__ERROR("[server] allocation failed. killing client %s", client->displayaddr);
        destroy_client(self, client);
        return;
    }

    memcpy(body+5, s, len);
    if (!quiet) RAAT__TRACE("[server] [%s] SENT[LL] [%d]%s %.*s", client->displayaddr, rid, is_final ? "" : " [nonfinal]", len, s);
    free(s);

    (*(int32_t*)body) = htonl(rid);
    body[4] = is_final ? RAAT__FINAL_RESPONSE: 0;

    write_message(self, client, RAAT__LL_RESPONSE, body, len+5);
    RC__free(self->alloc, body);
}

static void log_iterate_cb(RAAT__LogEntry *entry, void *userdata) {
    json_t *entrylist = userdata;
    json_t *jsonentry = json_object();
    json_object_set_new(jsonentry, "seq", json_integer(entry->seq));
    json_object_set_new(jsonentry, "level", json_string(RAAT__LogLevelString[entry->level]));
    json_object_set_new(jsonentry, "time", json_integer(entry->time));
    json_object_set_new(jsonentry, "text", json_string(entry->message));
    json_array_append_new(entrylist, jsonentry);
}

static void info_cb(const char *key, const char *val, void *userdata) {
    json_t *obj = userdata;
    json_object_set_new(obj, key, json_string(val));
}

typedef struct {
    RAAT__Server *self;
    Client       *client;
    uint32_t      rid;
    uv_async_t    async;
    char         *error_message;
    RC__Status    status;
} RunScriptCallbackState;

static void run_script_close_cb(uv_handle_t *async) {
    RunScriptCallbackState *state         = async->data;
    RC__free(state->self->alloc, state->error_message);
    RC__free(state->self->alloc, state);
}

static void run_script_state_destroy(RunScriptCallbackState *state) {
    uv_close((uv_handle_t*)&state->async, run_script_close_cb);
}

// invoked in server thread
static void run_script_async_cb(uv_async_t *async) {
    RunScriptCallbackState *state         = async->data;
    RAAT__Server   *self          = state->self;
    json_t         *response      = json_object();

    if (RC__STATUS_IS_SUCCESS(state->status)) {
        json_object_set_new(response, "status",   json_string("Success"));         
    } else if (state->status == RAAT__SESSION_STATUS_INVALID_SCRIPT) {
        json_object_set_new(response, "status",   json_string("ScriptError"));         
        if (state->error_message) json_object_set_new(response, "message", json_string(state->error_message));         
    } else {
        json_object_set_new(response, "status",  json_string("UnexpectedError"));         
        if (state->error_message) json_object_set_new(response, "message", json_string(state->error_message));         
        json_object_set_new(response, "code",    json_string(RC__status_to_string(state->status)));
    }

    respond(self, state->client, state->rid, true, false, response);
    json_decref(response);
    run_script_state_destroy(state);
}

// invoked in random thread
static void run_script_cb(RAAT__Session *session, RC__Status status, const char *error_message, void *userdata) {
    RunScriptCallbackState *state = userdata;
    RAAT__Server *self = state->self;
    state->status = status;
    if (error_message) state->error_message = RC__allocator_strdup(self->alloc, error_message); 
    uv_async_send(&state->async);
}

static void ev_request(RAAT__Server *self, Client *client, uint32_t rid, uint8_t *body, uint32_t bodylen) {
    RC__Status status;
    json_error_t error;
    json_t *request  = json_loadb((const char*)body, (size_t)bodylen, 0, &error);
    json_t *response = json_object();
    bool defer_response = false;
    bool quiet = false;
    const char *reqname;

    // handle JSON parse error
    if (request == NULL) {
        json_object_set_new(response, "status",   json_string("InvalidJson"));
        json_object_set_new(response, "text",     json_string(error.text));
        json_object_set_new(response, "line",     json_integer(error.line));
        json_object_set_new(response, "column",   json_integer(error.column));
        json_object_set_new(response, "position", json_integer(error.position));
        goto done;
    }
    reqname = json_string_value(json_object_get(request, "request"));

    // don't log get_log or load_script
    bool is_quiet = !strcmp(reqname, "get_log")  || !strcmp(reqname, "load_script");
    if (reqname && !is_quiet)
        RAAT__TRACE("[server] [%s] GOT[LL] [%d] %.*s", client->displayaddr, rid, bodylen, body);

    if (!reqname) {
        json_object_set_new(response, "status",   json_string("InvalidRequest"));
        json_object_set_new(response, "text",     json_string("string field 'request' is missing"));

    } else if (!strcmp(reqname, "get_log")) {
        int min_seq = -1;
        json_t *min_seq_json = json_object_get(request, "min_seq");
        json_t *arr = json_array();
        if (min_seq_json) min_seq = json_number_value(min_seq_json);
        RAAT__log_iterate(self->log, min_seq, log_iterate_cb, arr);
        json_object_set_new(response, "status", json_string("Success"));
        json_object_set_new(response, "entries", arr);
        quiet = true;

    } else if (!strcmp(reqname, "clear_log")) {
        RAAT__log_clear(self->log);
        json_object_set_new(response, "status", json_string("Success"));

    } else if (!strcmp(reqname, "get_info")) {
        json_t *info = json_object();
        RAAT__info_foreach(RAAT__device_get_info(self->device), info_cb, info);
        json_object_set_new(response, "status", json_string("Success"));
        json_object_set_new(response, "info", info);

    } else if (!strcmp(reqname, "load_script")) {
        const char *script = json_string_value(json_object_get(request, "script"));
        json_t *j_module   = json_object_get(request, "module");
        json_t *j_name   = json_object_get(request, "name");
        const char *module = NULL;
        const char *name = "<unknown>";

        if (j_module != NULL) module = json_string_value(j_module);
        if (j_name   != NULL) name   = json_string_value(j_name);

        //const char *sha    = json_string_value(json_object_get(request, "sha256"));   XXX hash validation

        if (script == NULL) {
            json_object_set_new(response, "status",   json_string("NotFound"));         // correct, since we have no script cache
        } else {
            RAAT__Script *rscript;
            // XXX: validate sha256 hash
            RAAT__TRACE("[server] new script with name=%s module=%s", 
                    name == NULL ? "(null)" : name,
                    module == NULL ? "(null)" : module);
            status = RAAT__script_new(self->alloc, name, module, script, strlen(script), false, &rscript);
            if (!RC__STATUS_IS_SUCCESS(status)) {
                RAAT__ERROR("[server] [%s] RAAT__script_new failed: %s", client->displayaddr, RC__status_to_string(status));
                json_object_set_new(response, "status",   json_string("UnexpectedError"));         // correct, since we have no script cache
            } else {
                RunScriptCallbackState *state = RC__new0(self->alloc, RunScriptCallbackState, 1);
                state->self   = self;
                state->client = client;
                state->rid    = rid;
                defer_response = true;
                uv_async_init(self->loop, &state->async, run_script_async_cb);
                state->async.data = state;
                status = RAAT__session_run_script(client->session, rscript, run_script_cb, state);
                if (!RC__STATUS_IS_SUCCESS(status)) {
                    RAAT__ERROR("[server] [%s] RAAT__run_script failed: %s", client->displayaddr, RC__status_to_string(status));
                    run_script_state_destroy(state);
                }
            }
        }

    } else {
        json_object_set_new(response, "status",   json_string("NotImplemented"));
    }

done:
    if (!defer_response) respond(self, client, rid, true, quiet, response);
    json_decref(response);
    json_decref(request);
}

static bool ev_ll_message(RAAT__Server *self, Client *client, uint32_t msgtype, uint8_t *body, uint32_t bodylen) {
    //RAAT__TRACE("got ll message type=0x%x bodylen=%d", msgtype, bodylen);

    if ((msgtype & 0x80000000) == 0) {
        RC__Status status;
        RAAT__SessionMessage *message;
        status = RAAT__session_message_new(self->alloc, msgtype, body, bodylen, false, &message);
        if (!RC__STATUS_IS_SUCCESS(status)) {
            RAAT__ERROR("[server] [%s] RAAT__session_message_new failed: %s", RC__status_to_string(status));
            destroy_client(self, client);
            return false;
        }
        RAAT__session_process_message(client->session, message);
        return true;
    }

    switch (msgtype) {
        case RAAT__LL_REQUEST: {      
            uint32_t rid = ntohl(*(int32_t*)body);
            ev_request(self, client, rid, body + 4, bodylen - 4);
        } break;

        case RAAT__LL_KEEPALIVE:        
            // nothing to do.
            break;

        case RAAT__LL_BEGIN_KEEPALIVE: {      
            uint64_t interval;
            if (bodylen != 8) {
                RAAT__WARNING("got invalid LL_BEGIN_KEEPALIVE (bodylen = %d)", bodylen);
                destroy_client(self, client);
                return false;
            }
            if (client->keepalive) {
                uv_timer_stop(client->keepalive_timer);
            }
            client->keepalive            = true;
            interval                     = ntohl(*(uint32_t*)(body));
            client->keepalive_timeout_ms = ntohl(*(uint32_t*)(body+4));
            uv_timer_start(client->keepalive_timer, keepalive_timer_cb, interval, interval);
            break;
        }

        case RAAT__LL_END_KEEPALIVE: {      
            client->keepalive = false;
            uv_timer_stop(client->keepalive_timer);
            break;
        }

        default:
            RAAT__TRACE("[server] got unknown ll message type %x", msgtype);
            break;
    }

    return true;
}

static void ev_client_read(RAAT__Server *self, Client *client, uint8_t *buf, int count) {
    int tocopy;

    /*
    RAAT__TRACE("got %d bytes", count);
    for (int i = 0; i < count; i++) {
        if (i % 2 == 0 && i) printf(" ");
        printf("%02x", buf[i]);
    }
    printf("\n");
    */

    client->last_active_ms = RC__now_us() / 1000;

    while (count > 0) {
        /*
        const char *state;
        if (client->readstate == READ_HEADER) state = "READ_HEADER";
        if (client->readstate == READ_BODY)   state = "READ_BODY";
        RAAT__TRACE("state=%s, left=%d, off=%d", state, client->left, client->off);
        */

        switch (client->readstate) {
            case READ_HEADER:
                tocopy = RC__min(client->left, count);
                memcpy(client->header + client->off, buf, tocopy);
                client->off  += tocopy;
                client->left -= tocopy;
                buf          += tocopy;
                count        -= tocopy;

                if (client->left == 0) {        // done reading header
                    uint32_t *hdr       = (uint32_t*)client->header;
                    client->bodylen = ntohl(hdr[0]) - 8;
                    client->msgtype = ntohl(hdr[1]);

                    if (client->bodylen > RAAT__LL_MAX_MESSAGE_LEN) {
                        RAAT__ERROR("[server] message length (%d) exceeds max length (%d). killing client %s", client->bodylen, RAAT__LL_MAX_MESSAGE_LEN, client->displayaddr);
                        destroy_client(self, client);
                        return;
                    }

                    if (client->bodylen == 0) {
                        if (!ev_ll_message(self, client, client->msgtype, NULL, 0)) return;
                        client->body = NULL;
                        client->left = 8;
                        client->off  = 0;
                    } else {
                        client->off    = 0;
                        client->left = client->bodylen;
                        client->body = RC__alloc(self->alloc, client->bodylen);
                        if (client->body == NULL) {
                            RAAT__ERROR("[server] allocation failed. killing client %s", client->displayaddr);
                            destroy_client(self, client);
                            return;
                        }
                        client->readstate = READ_BODY;
                    }
                }
                break;

            case READ_BODY:
                tocopy = RC__min(client->left, count);
                memcpy(client->body + client->off, buf, tocopy);
                client->off  += tocopy;
                client->left -= tocopy;
                buf          += tocopy;
                count        -= tocopy;

                if (client->left == 0) {        // done reading body
                    if (!ev_ll_message(self, client, client->msgtype, client->body, client->bodylen)) return;
                    RC__free(self->alloc, client->body);
                    client->body = NULL;
                    client->left = 8;
                    client->off  = 0;
                    client->readstate = READ_HEADER;
                }
                break;
        }
    }
}

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    Client       *client = stream->data;
    RAAT__Server *self   = client->server;

    if (nread == UV__EOF) {
        RAAT__TRACE("[server] [%s] read: eof", client->displayaddr);
        destroy_client(self, client);
    } else if (nread < 0) {
        RAAT__TRACE("[server] [%s] read error: %s", client->displayaddr, uv_strerror(nread));
        destroy_client(self, client);
    } else {
        ev_client_read(self, client, (uint8_t*)buf->base, (int)nread);
    }

    RC__free(self->alloc, buf->base);
}

static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    Client       *client = (Client*)handle->data;
    RAAT__Server *self   = client->server;
    buf->base = RC__alloc(self->alloc, suggested_size);
    buf->len  = suggested_size;
}

typedef struct {
    Client               *client;
    RAAT__SessionMessage *message;
} SessionMessageCallbackState;

static void session_message_callback_async_cb(RAAT__Server *self, void *userdata) {
    SessionMessageCallbackState *state  = userdata;
    write_message(self, state->client, state->message->message_type, state->message->data, state->message->length);
    RAAT__session_message_delete(state->message);
    RC__free(self->alloc, state);
}

static void session_message_callback(RAAT__Session *session, RAAT__SessionMessage *message, void *userdata) {
    Client                      *client = userdata;
    RAAT__Server                *self   = client->server;

    SessionMessageCallbackState *state = RC__new0(self->alloc, SessionMessageCallbackState, 1);
    state->client     = client;
    state->message    = message;
    RAAT__server_post(self, session_message_callback_async_cb, state);
}

static void session_failure_callback_async_cb(RAAT__Server *self, void *userdata) {
    Client *client = userdata;
    destroy_client(self, client);
}

static void session_failure_callback(RAAT__Session *session, void *userdata) {
    Client                      *client = userdata;
    RAAT__Server                *self   = client->server;
    RAAT__server_post(self, session_failure_callback_async_cb, client);
}

static void connection_cb(uv_stream_t *server, int rc) {
    RAAT__Server *self = (RAAT__Server*)server->data;
    RC__Status status;
    int sockaddr_len = sizeof(struct sockaddr_storage);

    Client *client = RC__new0(self->alloc, Client, 1);
    client->server = self;

    client->readstate  = READ_HEADER;
    client->left   = 8;
    client->off    = 0;

    client->keepalive_timer = RC__new0(self->alloc, uv_timer_t, 1);
    uv_timer_init(self->loop, client->keepalive_timer);
    client->keepalive_timer->data = client;

    client->sock = RC__new0(self->alloc, uv_tcp_t, 1);

    rc = uv_tcp_init(self->loop, client->sock);
    if (rc) {
        RAAT__ERROR("[server] uv_tcp_init failed: %s", uv_strerror(rc));
        RC__free(self->alloc, client);
        return;
    }
    client->sock->data = client;

    rc = uv_accept(server, (uv_stream_t*)client->sock);
    if (rc) {
        RAAT__ERROR("[server] uv_accept failed: %s", uv_strerror(rc));
        uv_close((uv_handle_t*)client->sock, NULL);
        RC__free(self->alloc, client);
        return;
    }

    rc = uv_tcp_nodelay(client->sock, 0);
    if (rc) {
        RAAT__WARNING("[server] uv_tcp_nodelay failed: %s", uv_strerror(rc));
    }

    rc = uv_tcp_getpeername(client->sock, (struct sockaddr*)&client->sockaddr, &sockaddr_len);
    if (rc) {
        RAAT__ERROR("[server] uv_tcp_getpeername failed: %s", uv_strerror(rc));
        handle_close_helper(self->alloc, client->sock);
        RC__free(self->alloc, client);
        return;
    }

    RC__sockaddr_to_string(&client->sockaddr, client->displayaddr);

    RAAT__TRACE("[server] [%s] accepted connection", client->displayaddr);

    status = RAAT__session_new(self->alloc, self->device, &client->sockaddr, &client->session);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        RAAT__ERROR("[server] RAAT__session_new failed: %s", RC__status_to_string(status));
        handle_close_helper(self->alloc, client->sock);
        RC__free(self->alloc, client);
        return;
    }

    status = RAAT__session_set_failure_callback(client->session, session_failure_callback, client);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        RAAT__ERROR("[server] RC__session_set_failure_callback failed: %s", RC__status_to_string(status));
        handle_close_helper(self->alloc, client->sock);
        RAAT__session_delete(client->session);
        RC__free(self->alloc, client);
        return;
    }

    status = RAAT__session_set_message_callback(client->session, session_message_callback, client);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        RAAT__ERROR("[server] RC__session_set_message_callback failed: %s", RC__status_to_string(status));
        handle_close_helper(self->alloc, client->sock);
        RAAT__session_delete(client->session);
        RC__free(self->alloc, client);
        return;
    }

    status = RAAT__session_start(client->session);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        RAAT__ERROR("[server] RC__session_start failed: %s", RC__status_to_string(status));
        handle_close_helper(self->alloc, client->sock);
        RAAT__session_delete(client->session);
        RC__free(self->alloc, client);
        return;
    }

    rc = uv_read_start((uv_stream_t*)client->sock, alloc_cb, read_cb);
    if (rc) {
        RAAT__ERROR("[server] uv_read_start failed: %s", uv_strerror(rc));
        handle_close_helper(self->alloc, client->sock);
        RAAT__session_delete(client->session);
        RC__free(self->alloc, client);
        return;
    }

    RC__list_push(&self->clients, client);
}

static void listener_close_cb(uv_handle_t *handle) {
    RAAT__Server *self = handle->data;
    RC__free(self->alloc, handle);
}

RC_API RC__Status
RAAT__server_start               (RAAT__Server   *self) {
    int rc;
    struct sockaddr_in addr = {0,};
    struct sockaddr_in realaddr;
    int realaddr_size = sizeof(realaddr);

    addr.sin_family      = AF_INET;
    addr.sin_port        = 0; //htons(9200);
    addr.sin_addr.s_addr = 0;

    RC__ASSERT(self != NULL);

    if (self->is_running) return RAAT__SERVER_START_STATUS_ALREADY_RUNNING;

    uv_async_init(self->loop, &self->async_post, cb_post);
    self->async_post.data = self;

    self->listener = RC__new0(self->alloc, uv_tcp_t, 1);
    rc = uv_tcp_init(self->loop, self->listener);
    if (rc) {
        RAAT__ERROR("[server] uv_tcp_init failed: %s", uv_strerror(rc));
        return RC__STATUS_UNEXPECTED_ERROR;
    }
    self->listener->data = self;

    rc = uv_tcp_bind(self->listener, (const struct sockaddr*)&addr, 0);
    if (rc) {
        RAAT__ERROR("[server] uv_tcp_bind failed: %s", uv_strerror(rc));

        uv_close((uv_handle_t*)self->listener, listener_close_cb);
        return RAAT__SERVER_STATUS_NETWORK_ERROR;
    }

    rc = uv_listen((uv_stream_t*)self->listener, 10, connection_cb);
    if (rc) {
        RAAT__ERROR("[server] uv_listen failed: %s", uv_strerror(rc));
        uv_close((uv_handle_t*)self->listener, listener_close_cb);
        return RAAT__SERVER_STATUS_NETWORK_ERROR;
    }

    rc = uv_tcp_getsockname(self->listener, (struct sockaddr*)&realaddr, &realaddr_size);
    if (rc) {
        RAAT__ERROR("[server] uv_tcp_getsockname failed: %s", uv_strerror(rc));
        uv_close((uv_handle_t*)self->listener, listener_close_cb);
        return RAAT__SERVER_STATUS_NETWORK_ERROR;
    }

    RAAT__INFO("[server] listening on port %d", ntohs(realaddr.sin_port));
    self->bound_port = ntohs(realaddr.sin_port);

    self->is_running = true;

    return RC__STATUS_SUCCESS;
}

RC_API RC__Status
RAAT__server_rebind              (RAAT__Server   *self) {
    int rc;

    RC__ASSERT(self != NULL);

    if (!self->is_running) return RAAT__SERVER_REBIND_STATUS_NOT_RUNNING;
    uv_close((uv_handle_t*)self->listener, listener_close_cb);

    struct sockaddr_in addr = {0,};
    struct sockaddr_in realaddr;
    int realaddr_size = sizeof(realaddr);

    addr.sin_family      = AF_INET;
    addr.sin_port        = 0; //htons(9200);
    addr.sin_addr.s_addr = 0;

    self->listener = RC__new0(self->alloc, uv_tcp_t, 1);
    rc = uv_tcp_init(self->loop, self->listener);
    if (rc) {
        RAAT__ERROR("[server] uv_tcp_init failed: %s", uv_strerror(rc));
        return RC__STATUS_UNEXPECTED_ERROR;
    }
    self->listener->data = self;

    rc = uv_tcp_bind(self->listener, (const struct sockaddr*)&addr, 0);
    if (rc) {
        RAAT__ERROR("[server] uv_tcp_bind failed: %s", uv_strerror(rc));

        uv_close((uv_handle_t*)self->listener, listener_close_cb);
        return RAAT__SERVER_STATUS_NETWORK_ERROR;
    }

    rc = uv_listen((uv_stream_t*)self->listener, 10, connection_cb);
    if (rc) {
        RAAT__ERROR("[server] uv_listen failed: %s", uv_strerror(rc));
        uv_close((uv_handle_t*)self->listener, listener_close_cb);
        return RAAT__SERVER_STATUS_NETWORK_ERROR;
    }

    rc = uv_tcp_getsockname(self->listener, (struct sockaddr*)&realaddr, &realaddr_size);
    if (rc) {
        RAAT__ERROR("[server] uv_tcp_getsockname failed: %s", uv_strerror(rc));
        uv_close((uv_handle_t*)self->listener, listener_close_cb);
        return RAAT__SERVER_STATUS_NETWORK_ERROR;
    }

    RAAT__INFO("[server] listening on port %d", ntohs(realaddr.sin_port));
    self->bound_port = ntohs(realaddr.sin_port);

    return RC__STATUS_SUCCESS;
}

RC_API RC__Status
RAAT__server_stop                (RAAT__Server   *self) {
    RC__ASSERT(self != NULL);

    if (!self->is_running) return RAAT__SERVER_STOP_STATUS_NOT_RUNNING;

    RC__ListIter it;
    RC__ListIter next;
    for (it = RC__list_begin(&self->clients); it != RC__list_end(&self->clients); it = next) {
        Client *client = (Client*)RC__listiter_data(it);
		next = RC__listiter_next(it);
        destroy_client(self, client);
    }

    uv_close((uv_handle_t*)self->listener, listener_close_cb);
    uv_close((uv_handle_t*)&self->async_post, NULL);
    self->bound_port = 0;
    self->is_running = false;

    return RC__STATUS_SUCCESS;
}

const char * RAAT__server_status_to_string(RC__Status status) {
    RC__ASSERT(status >= RAAT__SERVER_STATUS_BASE && status <= RAAT__SERVER_STATUS_MAX);

    switch (status) {
        case RAAT__SERVER_GET_BOUND_PORT_STATUS_NOT_STARTED: return "RAAT__SERVER_GET_BOUND_PORT_STATUS_NOT_STARTED";
        case RAAT__SERVER_START_STATUS_ALREADY_RUNNING:      return "RAAT__SERVER_START_STATUS_ALREADY_RUNNING";
        case RAAT__SERVER_STOP_STATUS_NOT_RUNNING:           return "RAAT__SERVER_STOP_STATUS_NOT_RUNNING";
        case RAAT__SERVER_REBIND_STATUS_NOT_RUNNING:         return "RAAT__SERVER_REBIND_STATUS_NOT_RUNNING";
        case RAAT__SERVER_STATUS_NETWORK_ERROR:              return "RAAT__SERVER_STATUS_NETWORK_ERROR";
        default: RC__ASSERT(0); return NULL;
    }
    return NULL;
}
