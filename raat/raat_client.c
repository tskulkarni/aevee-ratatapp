//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_client.h"
#include "rc_netutil.h"
#include "raat_base.h"
#include "rc_list.h"

#include <stdlib.h>
#include <string.h>

#define RAAT__CURRENT_LOG (self->log)

#define RAAT__MAX_MESSAGE_LEN (4096*1024)               // max 4mb message size

typedef enum {
    READ_HEADER,
    READ_BODY,
} ClientReadState;


struct RAAT__Client_s {
    RC__Allocator                    *alloc;
    RAAT__Log                        *log;
    uv_loop_t                        *loop;

    RAAT__ClientState                 state;
    uv_tcp_t                          sock;
    char                              displayaddr[RC__MAX_ADDR_LEN];   // printable server address

    RC__List                          pending_requests;

    // state when connecting
    uv_connect_t                      connect;
    RAAT__ClientConnectCallback       connect_cb;
    void                             *connect_userdata;

    int64_t                           last_active_ms;

    ClientReadState                   readstate;
    int                               off;                // offset into header or body
    int                               left;               // bytes remaining until next state
    uint8_t                           header[8];          // header storage
    uint32_t                          msgtype;
    uint32_t                          bodylen;
    uint8_t                          *body;

    int32_t                           rid;

    // keepalive state
    uv_timer_t                        keepalive_timer;
    int64_t                          keepalive_timeout_ms;

    // disconnected callback
    RAAT__ClientDisconnectedCallback  disconnected_cb;
    void                             *disconnected_userdata;

    // message callback
    RAAT__ClientMessageCallback       message_cb;
    void *                            message_userdata;
};

RC_API RC__Status
RAAT__client_new                 (RC__Allocator *alloc, RAAT__Log *log, uv_loop_t *loop, RAAT__Client **out_self) 
{
    RAAT__Client *self;

    RC__ASSERT(alloc != NULL);
    RC__ASSERT(log != NULL);
    RC__ASSERT(loop != NULL);
    RC__ASSERT(out_self != NULL);
    *out_self = NULL;

    self = RC__new0(alloc, RAAT__Client, 1);
    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;

    self->alloc = RC__allocator_default(alloc);
    self->log   = log;
    self->loop  = loop;

    self->state = RAAT__CLIENT_DISCONNECTED;

    RC__list_init(&self->pending_requests, self->alloc);

    *out_self = self;

    return RC__STATUS_SUCCESS;
}

typedef struct {
    uint32_t                      rid;
    RAAT__ClientResponseCallback  cb;
    void                         *userdata;
} PendingRequest;

static void destroy_request(RAAT__Client *self, PendingRequest *req) {
    req->cb(self, RAAT__CANCELED, NULL, req->userdata);
    RC__free(self->alloc, req);
}

static void destroy_request_cb(void *data, void *userdata) {
    RAAT__Client   *self = userdata;
    PendingRequest *req  = data;
    destroy_request(self, req);
}

static void close_connection(RAAT__Client *self) {
    if (self->state == RAAT__CLIENT_DISCONNECTED) return;

    uv_close((uv_handle_t*)&self->sock, NULL);
    if (self->state == RAAT__CLIENT_CONNECTED)  {
        uv_close((uv_handle_t*)&self->keepalive_timer, NULL);
    }
    RC__list_foreach_destroy(&self->pending_requests, destroy_request_cb, self);
    self->state = RAAT__CLIENT_DISCONNECTED;
    if (self->disconnected_cb) self->disconnected_cb(self, self->disconnected_userdata);
}

RC_API void 
RAAT__client_delete             (RAAT__Client   *self) {
    if (!self) return;

    close_connection(self);

    RC__free(self->alloc, self->body);
    RC__list_destroy(&self->pending_requests);
    RC__free(self->alloc, self);
}

typedef struct {
    RAAT__Client *client;
    uv_buf_t buf;
    uv_write_t write;
    // buffer contents follow immediately in same malloc block
} WriteState;

static void write_message_cb(uv_write_t *req, int status) {
    WriteState   *state  = req->data;
    RAAT__Client *self   = state->client;

    //RAAT__TRACE("[client] write %d bytes\n", (int)state->buf.len);

    if (status) {
        RAAT__WARNING("[client] failed to write to %s: %s. Closing connection.", self->displayaddr, uv_strerror(status));
        close_connection(self);
    }

    RC__free(self->alloc, state);
}

static void write_message(RAAT__Client *self, uint32_t msgtype, uint8_t *body, int bodylen) {
    int rc;
    char *msgbuf;
    WriteState *state = RC__alloc0(self->alloc, 8 + bodylen + sizeof(WriteState));
    if (state == NULL) {
        RAAT__WARNING("[client] failed to write to %s: allocation failed. Closing connection.", self->displayaddr);
        close_connection(self);
        return;
    }

    msgbuf = ((char*)(state)) + sizeof(*state);

    *(uint32_t*)(msgbuf+0) = htonl(bodylen+8);
    *(uint32_t*)(msgbuf+4) = htonl(msgtype);
    memcpy(msgbuf + 8, body, bodylen);

    state->client = self;
    state->buf.len  = bodylen+8;
    state->buf.base = msgbuf;

    //RAAT__TRACE("writing message len=%d msgtype=%x", state->buf.len, msgtype);

    state->write.data = state;
    rc = uv_write(&state->write, (uv_stream_t*)&self->sock, &state->buf, 1, write_message_cb);
    if (rc) {
        RAAT__ERROR("error writing message: %s", uv_strerror(rc));
    }
}

static void keepalive_timer_cb(uv_timer_t *handle) {
    RAAT__Client       *self = handle->data;

    int64_t now_ms = RC__now_us() / 1000;

    if (now_ms - self->last_active_ms > self->keepalive_timeout_ms) {
        RAAT__WARNING("[client] client timed out after %dms (timeout=%d)", (int)(now_ms - self->last_active_ms), (int)(self->keepalive_timeout_ms));
        close_connection(self);
        return;
    }

    write_message(self, RAAT__LL_KEEPALIVE, NULL, 0);
}

static void ev_response(RAAT__Client *self, uint32_t rid, uint8_t flags, uint8_t *body, uint32_t bodylen) {
    RC__ListIter it;
    for (it = RC__list_begin(&self->pending_requests); it != RC__list_end(&self->pending_requests); it = RC__listiter_next(it)) {
        PendingRequest *req = (PendingRequest*)RC__listiter_data(it);
        if (req->rid == rid) {
            json_error_t error;
            RAAT__TRACE("[client] GOT [%d] %.*s", rid, bodylen, body);
            json_t *json  = json_loadb((const char*)body, (size_t)bodylen, 0, &error);
            if (json) {
                req->cb(self, RAAT__INVALID_JSON | flags, json, req->userdata);
            } else {
                req->cb(self, flags, NULL, req->userdata);
                json_decref(json);
                RAAT__ERROR("[client] JSON Parse Error: %s at %d:%d", error.text, error.line, error.column);
                RAAT__ERROR("[client] In %.*s", bodylen, body);
            }
            if (flags & RAAT__FINAL_RESPONSE)
                RC__list_remove(&self->pending_requests, it);
            return;
        }
    }
}
RC_API void
RAAT__client_set_message_callback      (RAAT__Client   *self, RAAT__ClientMessageCallback cb, void *userdata) {
    RC__ASSERT(self != NULL);
    self->message_cb       = cb;
    self->message_userdata = userdata;
}

static void ev_ll_message(RAAT__Client *self, uint32_t msgtype, uint8_t *body, uint32_t bodylen) {
    //RAAT__TRACE("got ll message type=0x%x bodylen=%d", msgtype, bodylen);

    if ((msgtype & 0x80000000) == 0) {
        if (self->message_cb) {
            self->message_cb(self, msgtype, body, bodylen, self->message_userdata);
        }
        return;
    }

    switch (msgtype) {
        case RAAT__LL_RESPONSE: {      
            uint32_t rid = ntohl(*(int32_t*)body);
            uint8_t flags = body[5];
            ev_response(self, rid, flags, body + 5, bodylen - 5);
        } break;

        case RAAT__LL_KEEPALIVE: 
            // nothing to do.
            break;

        default:
            RAAT__TRACE("[client] got unknown ll message type %x", msgtype);
            break;
    }
}

static void ev_client_read(RAAT__Client *self, uint8_t *buf, int count) {
    int tocopy;

    self->last_active_ms = RC__now_us() / 1000;

    /*
    RAAT__TRACE("got %d bytes", count);
    for (int i = 0; i < count; i++) {
        if (i % 2 == 0 && i) printf(" ");
        printf("%02x", buf[i]);
    }
    printf("\n");
    */

    while (count > 0) {
        switch (self->readstate) {
            case READ_HEADER:
                tocopy = RC__min(self->left, count);
                memcpy(self->header + self->off, buf, tocopy);
                self->off  += tocopy;
                self->left -= tocopy;
                buf          += tocopy;
                count        -= tocopy;

                if (self->left == 0) {        // done reading header
                    uint32_t *hdr       = (uint32_t*)self->header;

                    self->bodylen = ntohl(hdr[0]) - 8;
                    self->msgtype = ntohl(hdr[1]);

                    if (self->bodylen > RAAT__MAX_MESSAGE_LEN) {
                        RAAT__ERROR("[client] message length (%d) exceeds max length (%d). killing client %s", self->bodylen, RAAT__MAX_MESSAGE_LEN, self->displayaddr);
                        close_connection(self);
                        return;
                    }

                    if (self->bodylen == 0) {
                        ev_ll_message(self, self->msgtype, NULL, 0);
                        self->body = NULL;
                        self->left = 8;
                        self->off  = 0;
                    } else {
                        self->off    = 0;
                        self->left = self->bodylen;
                        self->body = RC__alloc(self->alloc, self->bodylen);
                        if (self->body == NULL) {
                            RAAT__ERROR("[client] allocation failed. killing self %s", self->displayaddr);
                            close_connection(self);
                            return;
                        }
                        self->readstate = READ_BODY;
                    }
                }
                break;

            case READ_BODY:
                tocopy = RC__min(self->left, count);
                memcpy(self->body + self->off, buf, tocopy);
                self->off  += tocopy;
                self->left -= tocopy;
                buf        += tocopy;
                count      -= tocopy;

                if (self->left == 0) {        // done reading body
                    ev_ll_message(self, self->msgtype, self->body, self->bodylen);
                    RC__free(self->alloc, self->body);
                    self->body = NULL;
                    self->left = 8;
                    self->off  = 0;
                    self->readstate = READ_HEADER;
                }
                break;
        }
    }
}

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    RAAT__Client       *self = stream->data;

    if (nread == UV__EOF) {
        RAAT__TRACE("[client] read: eof");
        close_connection(self);
    } else if (nread < 0) {
        RAAT__TRACE("[client] read error: %s", uv_strerror(nread));
        close_connection(self);
    } else {
        ev_client_read(self, (uint8_t*)buf->base, (int)nread);
    }

    RC__free(self->alloc, buf->base);
}

static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    RAAT__Client       *self = (RAAT__Client*)handle->data;
    buf->base = RC__alloc(self->alloc, suggested_size);
    buf->len  = (int)suggested_size;
}

RC_API RAAT__ClientState
RAAT__client_get_state           (RAAT__Client *self) {
    RC__ASSERT(self != NULL);
    return self->state;
}

RC_API RC__Status
RAAT__client_disconnect          (RAAT__Client   *self) {
    RC__ASSERT(self != NULL);

    if (self->state == RAAT__CLIENT_DISCONNECTED) {
        return RAAT__CLIENT_STATUS_INVALID_STATE;
    } else if (self->state == RAAT__CLIENT_CONNECTING) {
        uv_cancel((uv_req_t*)&self->connect);
        return RC__STATUS_SUCCESS;
    } else if (self->state == RAAT__CLIENT_CONNECTED) {
        close_connection(self);
        return RC__STATUS_SUCCESS;
    }

    RC__ASSERT(0);
	return RC__STATUS_UNEXPECTED_ERROR;		// for windows
}

typedef struct {
    RAAT__Client *self;
    uv_connect_t connect;
} ConnectState;

static void connect_cb(uv_connect_t *connect, int connectrc) {
    RAAT__Client *self = connect->data;
    int rc;

    if (connectrc == UV_ECANCELED) {
        RAAT__INFO("[client] connection canceled");
        close_connection(self);
        self->connect_cb(self, RAAT__CLIENT_STATUS_CANCELED, connectrc, self->connect_userdata);
    } else if (connectrc) {
        RAAT__INFO("[client] connection failed: %s", uv_strerror(connectrc));
        close_connection(self);
        self->connect_cb(self, RAAT__CLIENT_STATUS_NETWORK_ERROR, connectrc, self->connect_userdata);
    } else {
        RAAT__INFO("[client] connected to %s", self->displayaddr);
        // begin 
        self->sock.data = self;
        self->readstate = READ_HEADER;
        self->body      = NULL;
        self->left      = 8;
        self->off       = 0;
        rc = uv_read_start((uv_stream_t*)&self->sock, alloc_cb, read_cb);
        if (rc) {
            self->state = RAAT__CLIENT_DISCONNECTED;
            RAAT__ERROR("[server] client failed: %s", uv_strerror(rc));
            self->connect_cb(self, RAAT__CLIENT_STATUS_NETWORK_ERROR, rc, self->connect_userdata);
            return;
        }

        rc = uv_timer_init(self->loop, &self->keepalive_timer);
        if (rc) {
            RAAT__ERROR("[client] uv_timer_init failed: %s", uv_strerror(rc));
            self->connect_cb(self, RC__STATUS_UNEXPECTED_ERROR, rc, self->connect_userdata);
        }

        // send RAAT__LL_BEGIN_KEEPALIVE message
        uint32_t body[2];
        self->state = RAAT__CLIENT_CONNECTED;
        body[0] = htonl(RAAT__LL_DEFAULT_KEEPALIVE);
        body[1] = htonl(RAAT__LL_DEFAULT_KEEPALIVE_TIMEOUT);
        self->keepalive_timer.data = self;
        self->keepalive_timeout_ms = RAAT__LL_DEFAULT_KEEPALIVE_TIMEOUT;
        self->last_active_ms = RC__now_us() / 1000;
        uv_timer_start(&self->keepalive_timer, keepalive_timer_cb, RAAT__LL_DEFAULT_KEEPALIVE, RAAT__LL_DEFAULT_KEEPALIVE);
        write_message(self, RAAT__LL_BEGIN_KEEPALIVE, (uint8_t*)body, 8);

        // notify the user that we're connected
        self->connect_cb(self, RC__STATUS_SUCCESS, 0, self->connect_userdata);
    }

    self->connect_cb       = NULL;
    self->connect_userdata = NULL;
}

RC_API RC__Status
RAAT__client_connect             (RAAT__Client   *self, const struct sockaddr * addr, RAAT__ClientConnectCallback cb, void *userdata) {
    int rc;

    RC__ASSERT(self != NULL);
    RC__ASSERT(addr != NULL);
    RC__ASSERT(cb != NULL);

    if (self->state != RAAT__CLIENT_DISCONNECTED) {
        return RAAT__CLIENT_STATUS_INVALID_STATE;
    }

    rc = uv_tcp_init(self->loop, &self->sock);
    if (rc) {
        RAAT__ERROR("[client] uv_tcp_init failed: %s", uv_strerror(rc));
        return RC__STATUS_UNEXPECTED_ERROR;
    }

    self->state = RAAT__CLIENT_CONNECTING;

    self->connect_cb = cb;
    self->connect_userdata = userdata;
    self->connect.data = self;

    RC__sockaddr_to_string(addr, self->displayaddr);

    RAAT__INFO("[client] Connecting to %s", self->displayaddr);

    rc = uv_tcp_connect(&self->connect, &self->sock, addr, connect_cb);
    if (rc) {
        RAAT__ERROR("[client] uv_tcp_connect failed: %s", uv_strerror(rc));
        return RAAT__CLIENT_STATUS_NETWORK_ERROR;
    }

    return RC__STATUS_SUCCESS;
}

RC_API RC__Status
RAAT__client_request_forget      (RAAT__Client   *self, const json_t *json) {
    return RAAT__client_request(self, json, NULL, NULL);
}

RC_API RC__Status
RAAT__client_request             (RAAT__Client   *self, const json_t *json, RAAT__ClientResponseCallback cb, void *userdata) {
    uint8_t* body;
    uint32_t rid;
    size_t json_len;
    char *json_str;

    RC__ASSERT(self != NULL);
    RC__ASSERT(json != NULL);

    if (self->state != RAAT__CLIENT_CONNECTED) return RAAT__CLIENT_STATUS_INVALID_STATE;

    json_str = json_dumps(json, 0);
    json_len = strlen(json_str);
    body = RC__alloc(self->alloc, 4 + (int)json_len);
    if (body == NULL) {
        free(json_str);
        return RC__STATUS_OUT_OF_MEMORY;
    }
    memcpy(body+4, json_str, json_len);

    if (cb) {
        PendingRequest *pending = RC__new0(self->alloc, PendingRequest, 1);
        if (pending == NULL) {
            RC__free(self->alloc, body);
            return RC__STATUS_OUT_OF_MEMORY;
        }
        if (self->rid == 0xfffffffe) self->rid = 0;             // avoid 0xffffffff and 0
        rid = pending->rid = ++self->rid;
        pending->cb       = cb;
        pending->userdata = userdata;
        RC__list_push(&self->pending_requests, pending);
    } else {
        rid = 0xffffffff;
    }

    RAAT__TRACE("[client] SENT [%d] %.*s", rid, json_len, json_str);
    free(json_str);

    (*(int32_t*)body) = htonl(rid);

    write_message(self, RAAT__LL_REQUEST, body, 4 + (int)json_len);

    return RC__STATUS_SUCCESS;
}

RC_API void
RAAT__client_set_disconnected_callback (RAAT__Client   *self, RAAT__ClientDisconnectedCallback cb, void *userdata) {
    RC__ASSERT(self != NULL);
    self->disconnected_cb       = cb;
    self->disconnected_userdata = userdata;
}

const char * RAAT__client_status_to_string(RC__Status status) {
    RC__ASSERT(status >= RAAT__CLIENT_STATUS_BASE && status <= RAAT__CLIENT_STATUS_MAX);

    switch (status) {
        case RAAT__CLIENT_STATUS_NETWORK_ERROR:        return "RAAT__CLIENT_STATUS_NETWORK_ERROR";
        case RAAT__CLIENT_STATUS_INVALID_STATE:        return "RAAT__CLIENT_STATUS_INVALID_STATE";
        case RAAT__CLIENT_STATUS_CANCELED:             return "RAAT__CLIENT_STATUS_CANCELED";
        default: RC__ASSERT(0); return NULL;
    }
    return NULL;
}
