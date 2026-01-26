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

#include "rc_base.h"
#include "rc_list.h"
#include "rc_dict.h"
#include "rc_guid.h"
#include "rc_allocator.h"
#include "rc_netutil.h"

#include <stdlib.h>
#include <string.h>

#define RAAT__DISCOVERY_PACKET_MAX_SIZE (4096)

#define RAAT__MULTICAST_ADDR ("239.255.90.90")
#define RAAT__DISCOVERY_PORT (9003)

#define RAAT__CURRENT_LOG (self->log)

struct RAAT__Discovery_s {
    RC__Allocator *alloc;
    RAAT__Log     *log;

    uv_loop_t     *loop;

    uv_udp_t      *unicast_recv;
    uv_udp_t      *unicast_send;

    bool           is_running;
    int            start_seq;

    RC__List       broadcast_ops;    // contains BroadcastOp
    RC__List       message_cbs;      // contains MessageThunk
    RC__List       pending_transactions;
    RC__List       multicast_recv_interfaces;        // contains uv_udp_t *
    RC__List       multicast_send_interfaces;        // contains uv_udp_t *

    uv_udp_t      *localhost_send_sock;         // NOTE: this is also  in the multicast_send_interfaces array, and is resource-managed there.
                                                //       this is just a shortcut for localhost rebroadcasts

    char recvbuf[RAAT__DISCOVERY_PACKET_MAX_SIZE];
};

typedef struct {
    RAAT__DiscoveryMessageCallback cb;
    void *userdata;
} MessageThunk;

typedef enum {
    QUERY_MESSAGE,
    RESPONSE_MESSAGE,
} DiscoveryMessageType;

/* dict config for RAAT__DiscoveryMessage::values */
static RC__DictConfig values_dict_config = {
    RC__dict_str_hash,
    RC__dict_str_equal,
    RC__dict_str_destroy,
    RC__dict_str_destroy
};

struct RAAT__DiscoveryMessage_s {
    RC__Allocator          *alloc;
    DiscoveryMessageType    type;
    struct sockaddr_storage source_addr;           // used for incoming query messages
    struct sockaddr_storage dest_addr;             // used for outgoing response messages
    RC__Guid                transactionid;         // used incoming query messages and outgoing response messages
    RC__Dict                values;                // key-value pairs
};

typedef enum {
    STATE_FAST_SEND,
    STATE_SLOW_SEND,
    STATE_SLOW_SEND_TIMER_WAIT,
} BroadcastOpState;

typedef struct {
    RAAT__Discovery                *discovery;
    RAAT__DiscoveryMessageCallback  cb;
    void                           *userdata;
    RC__Guid                        transactionid;
    uv_timer_t                      timer;              // transactions have a 10s expiration
} PendingTransaction;

typedef struct {
    RAAT__Discovery *discovery;

    BroadcastOpState state;

    char    buf[RAAT__DISCOVERY_PACKET_MAX_SIZE];
    int     len;

    int                     sends_active;
    bool                    timer_active;
    uv_timer_t              timer;

    int                     start_seq;

    RC__List                slow_ips;
    RC__List                fast_ips;
} BroadcastOp;

static void close_multicast(RAAT__Discovery *self);
static void close_unicast(RAAT__Discovery *self);

RC_API RC__Status 
RAAT__discovery_new                 (RC__Allocator *alloc, RAAT__Log *log, uv_loop_t *loop, RAAT__Discovery **out_self) {
    RAAT__Discovery *self;

    RC__ASSERT(out_self != NULL);
    *out_self = NULL;

    self = RC__new0(alloc, RAAT__Discovery, 1);
    if (self == NULL) {
        return RC__STATUS_OUT_OF_MEMORY;
    }

    self->alloc = RC__allocator_default(alloc);
    self->log   = log;
    self->loop  = loop;

    RC__list_init(&self->multicast_recv_interfaces, alloc);
    RC__list_init(&self->multicast_send_interfaces, alloc);
    RC__list_init(&self->broadcast_ops, alloc);
    RC__list_init(&self->message_cbs, alloc);
    RC__list_init(&self->pending_transactions, alloc);

    *out_self = self;

    return RC__STATUS_SUCCESS;
}

static void destroy_message_cb(void *data, void *userdata) {
    RC__Allocator   *alloc = (RC__Allocator*)userdata;
    MessageThunk *cb    = (MessageThunk*)data;

    RC__free(alloc, cb);
}

static void finish_destroy_broadcast_op(uv_handle_t *handle) {
    BroadcastOp     *op    = (BroadcastOp*)handle->data;
    //RAAT__Discovery *self = op->discovery;
    RC__free(op->discovery->alloc, op);
}

// XXX: make sure we kill any pending requests (timers, etc)
static void destroy_braodcast_op(void *data, void *userdata) {
    BroadcastOp   *op    = (BroadcastOp*)data;
    //RAAT__Discovery *self = op->discovery;

    RC__list_destroy(&op->fast_ips);
    RC__list_destroy(&op->slow_ips);

    uv_timer_stop(&op->timer);
    uv_close((uv_handle_t*)&op->timer, finish_destroy_broadcast_op);
}

static void destroy_pending_transaction_cb(void *data, void *userdata) {
    RC__Allocator *alloc = userdata;
    PendingTransaction *tx   = (PendingTransaction*)data;
    uv_close((uv_handle_t*)&tx->timer, NULL);
    RC__free(alloc, tx);
}

RC_API void
RAAT__discovery_delete           (RAAT__Discovery *self) {
    if (!self) return;

    RAAT__discovery_stop(self);

    close_multicast(self);

    RC__list_destroy(&self->multicast_recv_interfaces);
    RC__list_destroy(&self->multicast_send_interfaces);
    RC__list_destroy(&self->broadcast_ops);
    RC__list_foreach_destroy(&self->message_cbs, destroy_message_cb, self->alloc);
    RC__list_foreach_destroy(&self->pending_transactions, destroy_pending_transaction_cb, self->alloc);

    RC__free(self->alloc, self);
}

static void delete_sendop(BroadcastOp *op) {
    if (!op) return;
    RC__list_destroy(&op->slow_ips);
    RC__list_destroy(&op->fast_ips);
    uv_close((uv_handle_t*)&op->timer, NULL);
    RC__free(op->discovery->alloc, op);
}

static void broadcast_op_step(BroadcastOp *op);

static void broadcast_send_cb(uv_udp_send_t *req, int status) {
    BroadcastOp *op = (BroadcastOp*)req->data;
    RAAT__Discovery *self = op->discovery;
    op->sends_active -= 1;
    //RAAT__DEBUG("sends_active => %d", op->sends_active);
    if (status && status != UV_ECANCELED
            && status != UV_EHOSTDOWN && status != UV_EHOSTUNREACH) {         // these happen commonly...no reason to be noisy about it
        RAAT__WARNING("[discovery] got send failure: %s", uv_strerror(status));
    }
    if (op->sends_active == 0) {
        broadcast_op_step(op);
    }
    RC__free(self->alloc, req);
}

static void broadcast_timer_cb(uv_timer_t *timer) {
    BroadcastOp *op = (BroadcastOp*)timer->data;
    //RAAT__Discovery *self = op->discovery;
    op->timer_active = false;
    broadcast_op_step(op);
}

static void broadcast_op_step(BroadcastOp *op) {
    RAAT__Discovery *self = op->discovery;

    if (self->start_seq != op->start_seq) {
        if (op->sends_active > 0 || op->timer_active) {
            RAAT__DEBUG("[discovery] broadcast op canceled, deferring dispose until sends/timers are done");
        } else {
            RAAT__DEBUG("[discovery] broadcast op canceled, deleting it");
            RC__list_remove_by_data(&self->broadcast_ops, op);
            destroy_braodcast_op(op, self->alloc);
        }
        return;
    }

redispatch:
    switch (op->state) {
        case STATE_FAST_SEND: {
            uint32_t ip = RC__POINTER_TO_UINT(RC__list_shift(&op->fast_ips));
            if (ip) {
                struct sockaddr_in addr = {0,};
                char ipstrbuf[64];
                uv_buf_t buf;
                addr.sin_family      = AF_INET;
                addr.sin_addr.s_addr = ip;
                addr.sin_port        = htons(RAAT__DISCOVERY_PORT);

                buf.base = (char*)op->buf;
                buf.len  = op->len;

                uv_inet_ntop(AF_INET, &ip, ipstrbuf, sizeof(ipstrbuf));
                RC__ListIter it;
                for (it = RC__list_begin(&self->multicast_send_interfaces); it != RC__list_end(&self->multicast_send_interfaces); it = RC__listiter_next(it)) {
                    uv_udp_t *sock = (uv_udp_t*)RC__listiter_data(it);
                    uv_udp_send_t *udp_send = RC__new0(self->alloc, uv_udp_send_t, 1);
                    op->sends_active += 1;
                    udp_send->data = op;

                    uv_udp_send(udp_send, sock, &buf, 1, (struct sockaddr*)&addr, broadcast_send_cb);
                }

                // also send on the unisock
                uv_udp_send_t *udp_send = RC__new0(self->alloc, uv_udp_send_t, 1);
                op->sends_active += 1;
                udp_send->data = op;
                uv_udp_send(udp_send, self->unicast_send, &buf, 1, (struct sockaddr*)&addr, broadcast_send_cb);
                return;

            } else {
                op->state = STATE_SLOW_SEND_TIMER_WAIT;
                goto redispatch;
            }
        }

        case STATE_SLOW_SEND_TIMER_WAIT: {
             uv_timer_start(&op->timer, broadcast_timer_cb, 15, 0);
             op->timer_active = true;
             //RAAT__DEBUG("[discovery] slow send timer wait");
             op->state = STATE_SLOW_SEND;
             break;
         }

        case STATE_SLOW_SEND: {
            uint32_t ip = RC__POINTER_TO_UINT(RC__list_shift(&op->slow_ips));
            if (ip) {
                struct sockaddr_in addr = {0,};
                char ipstrbuf[64];
                uv_buf_t buf;
                addr.sin_family      = AF_INET;
                addr.sin_addr.s_addr = ip;
                addr.sin_port        = htons(RAAT__DISCOVERY_PORT);

                buf.base = (char*)op->buf;
                buf.len  = op->len;

                uv_inet_ntop(AF_INET, &ip, ipstrbuf, sizeof(ipstrbuf));
                //RAAT__DEBUG("[discovery] (slow) sending %d bytes to %s", op->len, ipstrbuf);
                op->state = STATE_SLOW_SEND_TIMER_WAIT;
                op->sends_active += 1;
                //RAAT__DEBUG("sends_active => %d", op->sends_active);
                uv_udp_send_t *udp_send = RC__new0(self->alloc, uv_udp_send_t, 1);
                udp_send->data = op;
                uv_udp_send(udp_send, self->unicast_send, &buf, 1, (struct sockaddr*)&addr, broadcast_send_cb);

                return;

            } else {
                // all done
                RAAT__DEBUG("[discovery] broadcast op is complete");
                RC__list_remove_by_data(&self->broadcast_ops, op);
                destroy_braodcast_op(op, self->alloc);
                return;
            }
       }

        default:
            RC__ASSERT(0);
            break;
    }
}

static RC__Status broadcast_message(RAAT__Discovery *self, void *message, int len) {
    RC__Status status;

    uv_interface_address_t *ifaces = NULL;
    int iface_count = 0;

    uint32_t *arp_ips = NULL;
    int arp_ip_count = 0;

    BroadcastOp *op = NULL;

    int i;

    RC__ASSERT(len < RAAT__DISCOVERY_PACKET_MAX_SIZE);

    op = RC__new0(self->alloc, BroadcastOp, 1);
    if (op == NULL) return RC__STATUS_OUT_OF_MEMORY;

    uv_timer_init(self->loop, &op->timer);
    op->timer.data = op;

    op->start_seq = self->start_seq;
    op->discovery = self;
    op->state = STATE_FAST_SEND;

    memcpy(op->buf, message, len);
    op->len = len;

    RC__list_init(&op->slow_ips, self->alloc);
    RC__list_init(&op->fast_ips, self->alloc);

    int rc = uv_interface_addresses(&ifaces, &iface_count);
    if (rc != 0) {
        status = RC__STATUS_UNEXPECTED_ERROR;
        goto fail;
    }

    struct sockaddr_in mcast_addr;
    uv_ip4_addr(RAAT__MULTICAST_ADDR, RAAT__DISCOVERY_PORT, &mcast_addr);
    RC__list_push(&op->fast_ips, RC__UINT_TO_POINTER(mcast_addr.sin_addr.s_addr));

    uint32_t loopback = 0x7f000001;
    RC__list_push(&op->fast_ips, RC__UINT_TO_POINTER(htonl(loopback)));

    status = RC__get_arp_ips(self->alloc, &arp_ips, &arp_ip_count);
    if (!RC__STATUS_IS_SUCCESS(status)) goto fail;

    for (i = 0; i < arp_ip_count; i++) {
        RC__list_push(&op->fast_ips, RC__UINT_TO_POINTER(arp_ips[i]));
    }
    RC__free(self->alloc, arp_ips); arp_ips = NULL;

    for (i = 0; i < iface_count; i++) {
        if (ifaces[i].address.address4.sin_family != AF_INET) continue;         // ipv4 only
        if (ifaces[i].is_internal)                            continue;         // skip loopback

        uint32_t ip   = ifaces[i].address.address4.sin_addr.s_addr;
        uint32_t mask = ifaces[i].netmask.netmask4.sin_addr.s_addr;

        uint32_t scanip;
        uint32_t network   = ip & mask;
        uint32_t broadcast = ip | (mask ^ 0xffffffffu);

        // send packet to broadcast address (fast)
        RC__list_push(&op->fast_ips, RC__UINT_TO_POINTER(broadcast));
    }
    uv_free_interface_addresses(ifaces, iface_count); ifaces = NULL;

    RC__list_push(&self->broadcast_ops, op);

    broadcast_op_step(op);

    return RC__STATUS_SUCCESS;

fail:
    if (ifaces)  uv_free_interface_addresses(ifaces, iface_count); 
    if (arp_ips) RC__free(self->alloc, arp_ips);
    if (op)  delete_sendop(op);
    return status;
}

static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    RAAT__Discovery *self = (RAAT__Discovery*)handle->data;
    buf->base = RC__alloc(self->alloc, RAAT__DISCOVERY_PACKET_MAX_SIZE);
    buf->len  = RAAT__DISCOVERY_PACKET_MAX_SIZE;
}

static void dispatch_message(RAAT__Discovery *self, RAAT__DiscoveryMessage *message) {
    if (message->type == QUERY_MESSAGE) {
        RC__ListIter it;
        // notify listeners that a query came in
        for (it = RC__list_begin(&self->message_cbs); it != RC__list_end(&self->message_cbs); it = RC__listiter_next(it)) {
            MessageThunk *mt = (MessageThunk*)RC__listiter_data(it);
            mt->cb(self, message, mt->userdata);
        }
    } 

    if (message->type == RESPONSE_MESSAGE) {
        RC__ListIter it;
        for (it = RC__list_begin(&self->pending_transactions); it != RC__list_end(&self->pending_transactions); it = RC__listiter_next(it)) {
            PendingTransaction *tx = (PendingTransaction*)RC__listiter_data(it);
            if (RC__guid_equals(&tx->transactionid, &message->transactionid)) {
                tx->cb(self, message, tx->userdata);
            }
        }
    }
}

typedef struct {
    char      *buf;
    int        len;
    RC__Status status;
} SerializeState;

static bool serialize_value(void *v_key, void *v_val, void *userdata) {
    SerializeState *state = (SerializeState*)userdata;
    const char *key = (const char*)v_key;
    const char *val = (const char*)v_val;

    size_t key_len = strlen(key);
    size_t val_len = val == NULL ? 0 : strlen(val);

    if (key_len > 255 || val_len > 65534) {
        state->status = RAAT__DISCOVERY_STATUS_INVALID_KEY_OR_VALUE;
        return true;
    }

    int space_needed = 3 + key_len + val_len;
    if (space_needed + state->len > RAAT__DISCOVERY_PACKET_MAX_SIZE) {
        state->status = RAAT__DISCOVERY_STATUS_PACKET_TOO_LARGE;
        return true;
    }

    // write key
    state->buf[state->len++] = (char)(uint8_t)key_len;
    memcpy(state->buf + state->len, key, key_len);
    state->len += key_len;

    // write val
    state->buf[state->len++] = (char)(uint8_t)((val_len & 0xff00) >> 8);
    state->buf[state->len++] = (char)(uint8_t)(val_len & 0xff);
    memcpy(state->buf + state->len, val, val_len);
    state->len += val_len;

    return false;
}

static RC__Status 
serialize_packet(RAAT__DiscoveryMessage *message, char *buf, int *out_len) {
    SerializeState state;

    memcpy(buf, "SOOD", 4);
    buf[4] = 2; // version
    if      (message->type == QUERY_MESSAGE)    buf[5] = 'Q';
    else if (message->type == RESPONSE_MESSAGE) buf[5] = 'R';
    else return RC__STATUS_UNEXPECTED_ERROR;

    state.buf    = buf;
    state.len    = 6;
    state.status = RC__STATUS_SUCCESS;

    RC__dict_foreach(&message->values, serialize_value, &state);

    *out_len = state.len;
    return state.status;
}


typedef struct {
    RAAT__Discovery    *discovery;
    char                buf[RAAT__DISCOVERY_PACKET_MAX_SIZE];
    int                 len;
    uv_buf_t            uvbuf;
    int                 sends_active;
} RebroadcastState;

static void rebroadcast_cb(uv_udp_send_t *req, int status) {
    RebroadcastState   *state = (RebroadcastState*)req->data;
    RAAT__Discovery *self  = state->discovery;

    if (status) {
        RAAT__ERROR("[discovery] error sending rebroadcast: %s", uv_strerror(status));
    }

    if (--state->sends_active == 0) {
        RC__free(self->alloc, state);
    }
    RC__free(self->alloc, req);
}

static bool copy_to_rebroadcast_cb(void *v_key, void *v_val, RAAT__DiscoveryMessage *rebroadcast) {
    RAAT__discovery_message_set(rebroadcast, (const char*)v_key, (const char*)v_val);
    return false;
}

/* handle incoming message */
static void ev_message_recieved(RAAT__Discovery *self, const struct sockaddr_storage* addr, const uv_buf_t *buf, ssize_t nread) {
    uint8_t ver;
    uint8_t type;
    int pos;
    RAAT__DiscoveryMessage *message;
    RC__Status status;
    bool bad_packet = false;

    char addrbuf[RC__MAX_ADDR_LEN];
    RC__sockaddr_to_string(addr, addrbuf);

    //RAAT__TRACE("got packet len=%d from %s", (int)nread, addrbuf);

    if (buf->base[0] != 'S' || buf->base[1] != 'O' || buf->base[2] != 'O' || buf->base[3] != 'D') {
        RAAT__TRACE("missing sood header");
        return;
    }

    ver = buf->base[4]; 
    if (ver != 2) {
        RAAT__TRACE("bad version");
        return;
    }

    type = buf->base[5];
    if (type != 'Q' && type != 'R') {
        RAAT__TRACE("bad type");
        return;
    }

    status = RAAT__discovery_message_new(self->alloc, &message);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        RAAT__ERROR("[discovery] failed to create message: %s", RC__status_to_string(status));
        return;
    }

    bool do_localhost_rebroadcast = false;
    if (type == 'Q') {
        do_localhost_rebroadcast = true;
        message->type = QUERY_MESSAGE;
    }
    if (type == 'R') {
        message->type = RESPONSE_MESSAGE;
    }

    message->source_addr = *addr;

    pos = 6;    // skip header
    while (pos < nread) {
        char name_buf[256];
        char val_buf[65536];
        bool null_val = false;
        int len = buf->base[pos++];             // 1 byte name len

        memcpy(name_buf, buf->base+pos, len);
        name_buf[len] = 0;
        pos += len;

        len  = buf->base[pos++] << 8;
        len |= buf->base[pos++];
        if (len < 65535) {
            memcpy(val_buf, buf->base+pos, len);
            val_buf[len] = 0;
        } else {
            null_val = true;
        }
        pos += len;

        if (!strcmp(name_buf, "_replyaddr")) {
            //RAAT__TRACE("    got replyaddr %s", val_buf);
            uv_ip4_addr(val_buf, ntohs(((struct sockaddr_in*)&message->source_addr)->sin_port), (struct sockaddr_in*)&message->source_addr);
            do_localhost_rebroadcast = false;
        }
        if (!strcmp(name_buf, "_replyport")) {
            //RAAT__TRACE("    got replyport %s", val_buf);
            ((struct sockaddr_in*)&message->source_addr)->sin_port = htons(atoi(val_buf));
        }

        if (!strcmp(name_buf, "_tid")) {
            if (RC__guid_init_string(&message->transactionid, val_buf)) {
                RAAT__WARNING("[discovery] packet has invalid transaction id: '%s'", val_buf);
                bad_packet = true;
            }
        } else {
            RC__dict_insert(&message->values, RC__allocator_strdup(self->alloc, name_buf), null_val ? NULL : RC__allocator_strdup(self->alloc, val_buf));
        }
    }

    if (do_localhost_rebroadcast && message->type == QUERY_MESSAGE && self->localhost_send_sock) {
        //RAAT__INFO("doing localhost rebroadcast...");

        RAAT__DiscoveryMessage *rebroadcast;
        status = RAAT__discovery_message_new(self->alloc, &rebroadcast);
        RC__ASSERT(RC__STATUS_IS_SUCCESS(status));

        RC__dict_foreach(&message->values, (RC__DictForeachCallback)copy_to_rebroadcast_cb, rebroadcast);

        RC__Status status;
        RebroadcastState *state;

        RC__ASSERT(self != NULL);
        RC__ASSERT(rebroadcast != NULL);

        state = RC__new0(self->alloc, RebroadcastState, 1);
        RC__ASSERT(state);

        state->discovery = self;

        char rebroadcast_addrbuf[RC__MAX_ADDR_LEN];
        RC__sockaddr_to_string(&message->source_addr, rebroadcast_addrbuf);

        char *c = strchr(rebroadcast_addrbuf, ':'); 
        if (c) {
            *c = '\0';

            RAAT__discovery_message_set(rebroadcast, "_replyaddr", rebroadcast_addrbuf);
            RAAT__discovery_message_set(rebroadcast, "_replyport", c+1);

            rebroadcast->type = QUERY_MESSAGE;

            status = serialize_packet(rebroadcast, state->buf, &state->len);
            if (!RC__STATUS_IS_SUCCESS(status)) {
                RC__free(self->alloc, state);
                return;
            }

            state->uvbuf.base = state->buf;
            state->uvbuf.len  = state->len;

            state->sends_active++;

            uv_udp_send_t *udp_send = RC__new0(self->alloc, uv_udp_send_t, 1); 
            udp_send->data = state;

            struct sockaddr_in mcast_addr;
            uv_ip4_addr(RAAT__MULTICAST_ADDR, RAAT__DISCOVERY_PORT, &mcast_addr);
            uv_udp_send(udp_send, self->localhost_send_sock, &state->uvbuf, 1, (struct sockaddr*)&mcast_addr, rebroadcast_cb);
        }

        RAAT__discovery_message_delete(rebroadcast);
    }

    /*
    char replyaddr[RC__MAX_ADDR_LEN];
    RC__sockaddr_to_string(&message->source_addr, replyaddr);
    RAAT__TRACE("    replies will go to %s", replyaddr);
    */

    if (bad_packet) {
        RAAT__WARNING("[discovery] ignoring bad packet");
        RAAT__discovery_message_delete(message);
        return;
    }

    dispatch_message(self, message);
    RAAT__discovery_message_delete(message);
}

static void recv_cb(uv_udp_t *sock, ssize_t nread, const uv_buf_t *buf, const struct sockaddr* addr, unsigned flags) {
    RAAT__Discovery *self = (RAAT__Discovery*)sock->data;

    if (nread == UV_ECANCELED) {
        return;
    }

    if (flags & UV_UDP_PARTIAL) {
        RAAT__WARNING("[discovery] discarding partial packet");
        return;
    }

    if (nread > 0) {
        struct sockaddr_storage sockaddr = {0,};
        if (addr->sa_family == AF_INET) {
            memcpy(&sockaddr, addr, sizeof(struct sockaddr_in));
        } else if (addr->sa_family == AF_INET6) {
            memcpy(&sockaddr, addr, sizeof(struct sockaddr_in6));
        }

        ev_message_recieved(self, &sockaddr, buf, nread);
    }

    // free buffer
    RC__free(self->alloc, buf->base);
}

static bool value_tostring_cb(const char *key, const char *val, RC__String *string) {
    RC__string_append_printf(string, "\"%s\": \"%s\", ", key, val);
    return false;
}

RC_API void         
RAAT__discovery_message_append_to_string(RAAT__DiscoveryMessage *msg, RC__String *string) {
    const char *type;
    char transactionid[RC__GUID_STRLEN + 1];

    RC__ASSERT(msg    != NULL);
    RC__ASSERT(string != NULL);

    RC__guid_tostring(&msg->transactionid, transactionid);

    if      (msg->type == QUERY_MESSAGE)    type = "QUERY";
    else if (msg->type == RESPONSE_MESSAGE) type = "RESPONSE";
    else                                    type ="INVALID";

    RC__string_append_printf(string, "DiscoveryMessage[type=%s, transactionid=%s, Data={ ", type, transactionid);
    RC__dict_foreach(&msg->values, (RC__DictForeachCallback)value_tostring_cb, string);
    RC__string_append(string, " } ]");
}

static void close_sock_cb(uv_handle_t *handle) {
    RC__Allocator *alloc = handle->data;
    RC__free(alloc, handle);
}

static void destroy_multicast_interface_cb(void *data, void *userdata) {
    uv_udp_t *sock = data;
    RC__Allocator   *alloc = (RC__Allocator*)userdata;
    uv_udp_recv_stop(sock);
    sock->data = alloc;
    uv_close((uv_handle_t*)sock, close_sock_cb);
}

static void close_multicast(RAAT__Discovery *self) {
    RAAT__TRACE("closing multicast");
    RC__list_foreach_remove(&self->multicast_recv_interfaces, destroy_multicast_interface_cb, self->alloc);
    RC__list_foreach_remove(&self->multicast_send_interfaces, destroy_multicast_interface_cb, self->alloc);
}

static void close_unicast(RAAT__Discovery *self) {
    if (self->unicast_send) {
        RAAT__TRACE("[discovery] closing unicast send socket");
        uv_udp_recv_stop(self->unicast_send);
        self->unicast_send->data = self->alloc;
        uv_close((uv_handle_t*)self->unicast_send, close_sock_cb);
        self->unicast_send = NULL;
    }
    if (self->unicast_recv) {
        RAAT__TRACE("[discovery] closing unicast recv socket");
        uv_udp_recv_stop(self->unicast_recv);
        self->unicast_recv->data = self->alloc;
        uv_close((uv_handle_t*)self->unicast_recv, close_sock_cb);
        self->unicast_recv = NULL;
    }
}

static RC__Status bind_multicast(RAAT__Discovery *self) {
    int rc;
    uv_interface_address_t *addrs;
    int addrcount;
    int i;

    rc = uv_interface_addresses(&addrs, &addrcount);
    if (rc) {
        RAAT__ERROR("uv_interface_addresses failed: %s", uv_strerror(rc));
        return RAAT__DISCOVERY_STATUS_NETWORK_ERROR;
    }

    for (i = 0; i < addrcount; i++) {
        uv_interface_address_t *addr = &addrs[i];
        char iface_addr_str[RC__MAX_ADDR_LEN];

        struct sockaddr_storage realmaddr = {0,};
        int realmaddr_size = sizeof(realmaddr);
        char realmaddr_str[RC__MAX_ADDR_LEN];

        if (addr->address.address4.sin_family != AF_INET) continue;

        // set up recv socket
        {
            rc = uv_ip4_name((const struct sockaddr_in*)&addr->address.address4, iface_addr_str, RC__MAX_ADDR_LEN);
            if (rc) { RAAT__ERROR("[discovery] [iface:%s] uv_ip4_name failed: %s", iface_addr_str, uv_strerror(rc)); continue; }

            uv_udp_t *sock = RC__new0(self->alloc, uv_udp_t, 1);
            rc = uv_udp_init(self->loop, sock);
            if (rc) { RAAT__ERROR("[discovery] [iface:%s] uv_udp_init failed: %s", iface_addr_str, uv_strerror(rc)); uv_close((uv_handle_t*)sock, NULL); continue; }

            struct sockaddr_in multicast_addr;
            uv_ip4_addr("0.0.0.0", RAAT__DISCOVERY_PORT, &multicast_addr);
            rc = uv_udp_bind(sock, (const struct sockaddr*)&multicast_addr, UV_UDP_REUSEADDR);
            if (rc) { RAAT__ERROR("[discovery] [iface:%s] uv_udp_bind failed: %s", iface_addr_str, uv_strerror(rc)); uv_close((uv_handle_t*)sock, NULL); continue; }

            rc = uv_udp_set_membership(sock, RAAT__MULTICAST_ADDR, iface_addr_str, UV_JOIN_GROUP);
            if (rc) { RAAT__ERROR("[discovery] [iface:%s] uv_udp_set_membership failed: %s", iface_addr_str, uv_strerror(rc)); uv_close((uv_handle_t*)sock, NULL); continue; }

            rc = uv_udp_set_multicast_interface(sock, iface_addr_str);
            if (rc) { RAAT__ERROR("[discovery] [iface:%s] uv_udp_set_multicast_interface failed: %s", iface_addr_str, uv_strerror(rc)); uv_close((uv_handle_t*)sock, NULL); continue; }

            rc = uv_udp_set_multicast_ttl(sock, 2);
            if (rc) { RAAT__ERROR("[discovery] [iface:%s] uv_udp_set_multicast_ttl failed: %s", iface_addr_str, uv_strerror(rc)); uv_close((uv_handle_t*)sock, NULL); continue; }

            rc = uv_udp_set_broadcast(sock, 1);
            if (rc) { RAAT__ERROR("[discovery] [iface:%s] uv_udp_set_broadcast failed: %s", iface_addr_str, uv_strerror(rc)); uv_close((uv_handle_t*)sock, NULL); continue; }

            rc = uv_udp_set_multicast_loop(sock, 1);
            if (rc) { RAAT__ERROR("[discovery] [iface:%s] uv_udp_set_multicast_loop failed: %s", iface_addr_str, uv_strerror(rc)); uv_close((uv_handle_t*)sock, NULL); continue; }

            rc = uv_udp_getsockname(sock, (struct sockaddr*)&realmaddr, &realmaddr_size);
            if (rc) { RAAT__ERROR("[discovery] [iface:%s] uv_udp_getsockname failed: %s", iface_addr_str, uv_strerror(rc)); uv_close((uv_handle_t*)sock, NULL); continue; }

            RC__sockaddr_to_string(&realmaddr, realmaddr_str);
            RAAT__INFO("[discovery] [iface:%s] multicast recv socket is bound to %s", iface_addr_str, realmaddr_str);

            sock->data = self;
            uv_udp_recv_start(sock, alloc_cb, recv_cb);

            RC__list_push(&self->multicast_recv_interfaces, sock);
        }

        // set up send socket
        {
            rc = uv_ip4_name((const struct sockaddr_in*)&addr->address.address4, iface_addr_str, RC__MAX_ADDR_LEN);
            if (rc) { RAAT__ERROR("[discovery] [iface:%s] uv_ip4_name failed: %s", iface_addr_str, uv_strerror(rc)); continue; }

            uv_udp_t *sock = RC__new0(self->alloc, uv_udp_t, 1);
            rc = uv_udp_init(self->loop, sock);
            if (rc) { RAAT__ERROR("[discovery] [iface:%s] uv_udp_init failed: %s", iface_addr_str, uv_strerror(rc)); uv_close((uv_handle_t*)sock, NULL); continue; }

            struct sockaddr_in multicast_addr;
            uv_ip4_addr("0.0.0.0", 0, &multicast_addr);
            rc = uv_udp_bind(sock, (const struct sockaddr*)&multicast_addr, UV_UDP_REUSEADDR);
            if (rc) { RAAT__ERROR("[discovery] [iface:%s] uv_udp_bind failed: %s", iface_addr_str, uv_strerror(rc)); uv_close((uv_handle_t*)sock, NULL); continue; }

            rc = uv_udp_set_multicast_interface(sock, iface_addr_str);
            if (rc) { RAAT__ERROR("[discovery] [iface:%s] uv_udp_set_multicast_interface failed: %s", iface_addr_str, uv_strerror(rc)); uv_close((uv_handle_t*)sock, NULL); continue; }

            rc = uv_udp_set_multicast_ttl(sock, 2);
            if (rc) { RAAT__ERROR("[discovery] [iface:%s] uv_udp_set_multicast_ttl failed: %s", iface_addr_str, uv_strerror(rc)); uv_close((uv_handle_t*)sock, NULL); continue; }

            rc = uv_udp_set_broadcast(sock, 1);
            if (rc) { RAAT__ERROR("[discovery] [iface:%s] uv_udp_set_broadcast failed: %s", iface_addr_str, uv_strerror(rc)); uv_close((uv_handle_t*)sock, NULL); continue; }

            rc = uv_udp_set_multicast_loop(sock, 1);
            if (rc) { RAAT__ERROR("[discovery] [iface:%s] uv_udp_set_multicast_loop failed: %s", iface_addr_str, uv_strerror(rc)); uv_close((uv_handle_t*)sock, NULL); continue; }

            rc = uv_udp_getsockname(sock, (struct sockaddr*)&realmaddr, &realmaddr_size);
            if (rc) { RAAT__ERROR("[discovery] [iface:%s] uv_udp_getsockname failed: %s", iface_addr_str, uv_strerror(rc)); uv_close((uv_handle_t*)sock, NULL); continue; }

            RC__sockaddr_to_string(&realmaddr, realmaddr_str);
            RAAT__INFO("[discovery] [iface:%s] multicast send socket is bound to %s", iface_addr_str, realmaddr_str);

            sock->data = self;
            uv_udp_recv_start(sock, alloc_cb, recv_cb);

            if (addr->is_internal) {
                self->localhost_send_sock = sock;
            }

            RC__list_push(&self->multicast_send_interfaces, sock);
        }
    }

    uv_free_interface_addresses(addrs, addrcount);

    return RC__STATUS_SUCCESS;
}

RC_API RC__Status
RAAT__discovery_stop            (RAAT__Discovery   *self) {
    RC__ASSERT(self != NULL);
    if (self->is_running) {
        RAAT__TRACE("[discovery] stopping");
        close_multicast(self);
        close_unicast(self);
        self->is_running = false;
        ++self->start_seq;
        return RC__STATUS_SUCCESS;
    } else {
        return RAAT__DISCOVERY_STOP_STATUS_NOT_RUNNING;
    }
}


RC_API RC__Status
RAAT__discovery_start            (RAAT__Discovery   *self) {
    int rc;
    RC__Status status = RC__STATUS_UNEXPECTED_ERROR;

    struct sockaddr_storage realaddr = {0,};
    int realaddr_size = sizeof(realaddr);
    char realaddr_str[RC__MAX_ADDR_LEN];

    RC__ASSERT(self != NULL);

    if (self->is_running) {
        return RAAT__DISCOVERY_START_STATUS_ALREADY_RUNNING;
    }
    self->is_running = true;

    RAAT__TRACE("[discovery] starting");
    ++self->start_seq;

    status = bind_multicast(self);
    if (!RC__STATUS_IS_SUCCESS(status)) goto fail;

    {
        struct sockaddr_in unicast_addr;

        self->unicast_send = RC__new0(self->alloc, uv_udp_t, 1);
        rc = uv_udp_init(self->loop, self->unicast_send);
        if (rc) {
            RAAT__ERROR("uv_udp_init failed: %s", uv_strerror(rc));
            status = RAAT__DISCOVERY_STATUS_NETWORK_ERROR;
            goto fail;
        }
        
        uv_ip4_addr("0.0.0.0", 0, &unicast_addr);
        rc = uv_udp_bind(self->unicast_send, (const struct sockaddr*)&unicast_addr, 0);
        if (rc) {
            RAAT__ERROR("uv_udp_bind failed: %s", uv_strerror(rc));
            status = RAAT__DISCOVERY_STATUS_NETWORK_ERROR;
            goto fail;
        }

        rc = uv_udp_getsockname(self->unicast_send, (struct sockaddr*)&realaddr, &realaddr_size);
        if (rc) {
            RAAT__ERROR("[server] uv_udp_getsockname failed: %s", uv_strerror(rc));
            status = RAAT__DISCOVERY_STATUS_NETWORK_ERROR;
            goto fail;
        }

        rc = uv_udp_set_broadcast(self->unicast_send, 1);
        if (rc) {
            RAAT__ERROR("uv_udp_set_broadcast failed: %s", uv_strerror(rc));
            status = RAAT__DISCOVERY_STATUS_NETWORK_ERROR;
            goto fail;
        }
        rc = uv_udp_set_multicast_ttl(self->unicast_send, 2);
        if (rc) {
            RAAT__ERROR("uv_udp_set_multicast_ttl failed: %s", uv_strerror(rc));
            status = RAAT__DISCOVERY_STATUS_NETWORK_ERROR;
            goto fail;
        }

        self->unicast_send->data = self;
        uv_udp_recv_start(self->unicast_send,   alloc_cb, recv_cb);
    }

    {
        struct sockaddr_in unicast_addr;

        self->unicast_recv = RC__new0(self->alloc, uv_udp_t, 1);
        rc = uv_udp_init(self->loop, self->unicast_recv);
        if (rc) {
            RAAT__ERROR("uv_udp_init failed: %s", uv_strerror(rc));
            status = RAAT__DISCOVERY_STATUS_NETWORK_ERROR;
            goto fail;
        }
        
        uv_ip4_addr("0.0.0.0", RAAT__DISCOVERY_PORT, &unicast_addr);
        rc = uv_udp_bind(self->unicast_recv, (const struct sockaddr*)&unicast_addr, UV_UDP_REUSEADDR);
        if (rc) {
            RAAT__WARNING("couldn't bind to 9003. someone else has it. Counting on localhost rebroadcasts.");
            uv_ip4_addr("0.0.0.0", 0, &unicast_addr);
            rc = uv_udp_bind(self->unicast_recv, (const struct sockaddr*)&unicast_addr, 0);
            if (rc) {
                RAAT__ERROR("uv_udp_bind failed: %s", uv_strerror(rc));
                status = RAAT__DISCOVERY_STATUS_NETWORK_ERROR;
                goto fail;
            }
        }

        rc = uv_udp_getsockname(self->unicast_recv, (struct sockaddr*)&realaddr, &realaddr_size);
        if (rc) {
            RAAT__ERROR("[server] uv_udp_getsockname failed: %s", uv_strerror(rc));
            status = RAAT__DISCOVERY_STATUS_NETWORK_ERROR;
            goto fail;
        }

        rc = uv_udp_set_broadcast(self->unicast_recv, 1);
        if (rc) {
            RAAT__ERROR("uv_udp_set_broadcast failed: %s", uv_strerror(rc));
            status = RAAT__DISCOVERY_STATUS_NETWORK_ERROR;
            goto fail;
        }
        rc = uv_udp_set_multicast_ttl(self->unicast_recv, 2);
        if (rc) {
            RAAT__ERROR("uv_udp_set_multicast_ttl failed: %s", uv_strerror(rc));
            status = RAAT__DISCOVERY_STATUS_NETWORK_ERROR;
            goto fail;
        }

        self->unicast_recv->data = self;
        uv_udp_recv_start(self->unicast_recv,   alloc_cb, recv_cb);
    }

    RC__sockaddr_to_string(&realaddr, realaddr_str);
    RAAT__INFO("[discovery] unicast socket is bound to %s", realaddr_str);

    return RC__STATUS_SUCCESS;

fail:
    close_multicast(self);
    close_unicast(self); 
    self->is_running = false;
    return status;
}

RC_API RC__Status 
RAAT__discovery_broadcast                       (RAAT__Discovery               *self, 
                                                 RAAT__DiscoveryMessage        *message) {
    return RAAT__discovery_query(self, message, NULL, NULL);
}

typedef struct {
    RAAT__Discovery    *discovery;
    char                buf[RAAT__DISCOVERY_PACKET_MAX_SIZE];
    int                 len;
    uv_buf_t            uvbuf;
    int                 sends_active;
} ResponseState;

static void respond_cb(uv_udp_send_t *req, int status) {
    ResponseState   *state = (ResponseState*)req->data;
    RAAT__Discovery *self  = state->discovery;

    if (status) {
        RAAT__ERROR("[discovery] error sending response: %s", uv_strerror(status));
    }

    if (--state->sends_active == 0) {
        RC__free(self->alloc, state);
    }
    RC__free(self->alloc, req);
}

RC_API RC__Status 
RAAT__discovery_respond                         (RAAT__Discovery               *self, 
                                                 RAAT__DiscoveryMessage        *response)
{
    RC__Status status;
    ResponseState *state;

    RC__ASSERT(self != NULL);
    RC__ASSERT(response != NULL);

    state = RC__new0(self->alloc, ResponseState, 1);
    if (state == NULL) return RC__STATUS_OUT_OF_MEMORY;

    char guidbuf[RC__GUID_STRLEN + 1];
    RC__guid_tostring(&response->transactionid, guidbuf);
    RAAT__discovery_message_set(response, "_tid", guidbuf);

    state->discovery = self;
    status = serialize_packet(response, state->buf, &state->len);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        RC__free(self->alloc, state);
        return status;
    }

    state->uvbuf.base = state->buf;
    state->uvbuf.len  = state->len;

    char addrbuf[RC__MAX_ADDR_LEN];
    RC__sockaddr_to_string(&response->dest_addr, addrbuf);

    state->sends_active++;

    // respond to literal source address
    uv_udp_send_t *udp_send = RC__new0(self->alloc, uv_udp_send_t, 1); 
    udp_send->data = state;
    uv_udp_send(udp_send, self->unicast_send, &state->uvbuf, 1, (struct sockaddr*)&response->dest_addr, respond_cb);

    return RC__STATUS_SUCCESS;
}

static void transaction_timer_cb(uv_timer_t *timer) {
    PendingTransaction *tx   = (PendingTransaction*)timer->data;
    RAAT__Discovery    *self = tx->discovery;
    RC__list_remove_by_data(&self->pending_transactions, tx);
    uv_close((uv_handle_t*)timer, NULL);
    RC__free(self->alloc, tx);
}

RC_API RC__Status 
RAAT__discovery_query                           (RAAT__Discovery               *self, 
                                                 RAAT__DiscoveryMessage        *message,
                                                 RAAT__DiscoveryMessageCallback cb,
                                                 void *userdata) {
    RC__Status status;
    char buf[RAAT__DISCOVERY_PACKET_MAX_SIZE];
    int len;
    PendingTransaction *t;

    RC__ASSERT(self != NULL);
    RC__ASSERT(message != NULL);

    char guidbuf[RC__GUID_STRLEN + 1];
    RC__guid_tostring(&message->transactionid, guidbuf);
    RAAT__discovery_message_set(message, "_tid", guidbuf);

    if (cb) {
        t = RC__new0(self->alloc, PendingTransaction, 1);
        if (t == NULL) return RC__STATUS_OUT_OF_MEMORY;
    }

    status = serialize_packet(message, buf, &len);
    if (!RC__STATUS_IS_SUCCESS(status)) return status;

    status = broadcast_message(self, buf, len);
    if (!RC__STATUS_IS_SUCCESS(status)) return status;

    if (cb) {
        t->discovery     = self;
        t->cb            = cb;
        t->userdata      = userdata;
        t->transactionid = message->transactionid;
        t->timer.data    = t;
        uv_timer_init(self->loop, &t->timer);
        uv_timer_start(&t->timer, transaction_timer_cb, 10000, 0);
        RC__list_push(&self->pending_transactions, t);
    }

    return RC__STATUS_SUCCESS;
}


RC_API RC__Status
RAAT__discovery_add_message_callback            (RAAT__Discovery                *self, 
                                                 RAAT__DiscoveryMessageCallback  cb, 
                                                 void                           *userdata)
{
    MessageThunk *thunk;

    RC__ASSERT(self != NULL);
    RC__ASSERT(cb != NULL);

    thunk = RC__new0(self->alloc, MessageThunk, 1);
    if (thunk == NULL) return RC__STATUS_OUT_OF_MEMORY;

    thunk->cb = cb;
    thunk->userdata = userdata;

    RC__list_push(&self->message_cbs, thunk);

    return RC__STATUS_SUCCESS;
}


RC_API bool
RAAT__discovery_remove_message_callback         (RAAT__Discovery                *self, 
                                                 RAAT__DiscoveryMessageCallback  cb,
                                                 void                           *userdata) 
{
    RC__ListIter it;
    RC__ASSERT(self != NULL);
    RC__ASSERT(cb != NULL);

    for (it = RC__list_begin(&self->message_cbs); it != RC__list_end(&self->message_cbs); it = RC__listiter_next(it)) {
        MessageThunk *data = (MessageThunk*)RC__listiter_data(it);
        if (data->cb == cb && data->userdata == userdata) {
            RC__list_remove(&self->message_cbs, it);
            return true;
        }
    }

    return false;
}

RC_API RC__Status
RAAT__discovery_message_new(RC__Allocator *alloc, RAAT__DiscoveryMessage **out_self) 
{
    RAAT__DiscoveryMessage *self;

    RC__ASSERT(out_self != NULL);

    alloc = RC__allocator_default(alloc);
    self = RC__new0(alloc, RAAT__DiscoveryMessage, 1);
    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;

    self->alloc         = alloc;
    self->type          = QUERY_MESSAGE;
    RC__guid_init_random(&self->transactionid);

    RC__dict_init(&self->values, alloc, &values_dict_config);

    *out_self = self;
    
    return RC__STATUS_SUCCESS;
}

RC_API RC__Status
RAAT__discovery_message_new_response(RC__Allocator *alloc, RAAT__DiscoveryMessage *query, RAAT__DiscoveryMessage **out_self) 
{
    RAAT__DiscoveryMessage *self;

    RC__ASSERT(out_self != NULL);
    RC__ASSERT(query    != NULL);

    alloc = RC__allocator_default(alloc);
    self = RC__new0(alloc, RAAT__DiscoveryMessage, 1);
    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;

    self->alloc         = alloc;
    self->dest_addr     = query->source_addr;
    self->transactionid = query->transactionid;
    self->type          = RESPONSE_MESSAGE;

    RC__dict_init(&self->values, alloc, &values_dict_config);

    *out_self = self;
    
    return RC__STATUS_SUCCESS;
}

RC_API struct sockaddr_storage *
RAAT__discovery_message_get_source_addr(RAAT__DiscoveryMessage *msg) {
    RC__ASSERT(msg != NULL);
    return &msg->source_addr;
}

RC_API struct sockaddr_storage *
RAAT__discovery_message_get_dest_addr(RAAT__DiscoveryMessage *msg) {
    RC__ASSERT(msg != NULL);
    return &msg->dest_addr;
}

RC_API RC__Status
RAAT__discovery_message_set(RAAT__DiscoveryMessage *self, const char *key, const char *val) 
{
    RC__ASSERT(self != NULL);
    RC__ASSERT(key != NULL);
    if (val == NULL) {
        RC__dict_remove(&self->values, (void*)key);
    } else {
        RC__dict_insert(&self->values, (void*)RC__allocator_strdup(self->alloc, key), (void*)RC__allocator_strdup(self->alloc, val));
    }
    return RC__STATUS_SUCCESS;
}

RC_API const char *
RAAT__discovery_message_get(RAAT__DiscoveryMessage *self, const char *key) 
{
    RC__ASSERT(self != NULL);
    RC__ASSERT(key != NULL);
    return RC__dict_lookup(&self->values, (void*)key);
}

RC_API void
RAAT__discovery_message_delete(RAAT__DiscoveryMessage *self) 
{
    if (!self) return;
    RC__dict_destroy(&self->values);
    RC__free(self->alloc, self);
}

const char * RAAT__discovery_status_to_string(RC__Status status) {
    RC__ASSERT(status >= RAAT__DISCOVERY_STATUS_BASE && status <= RAAT__DISCOVERY_STATUS_MAX);

    switch (status) {
        case RAAT__DISCOVERY_START_STATUS_ALREADY_RUNNING: return "RAAT__DISCOVERY_START_STATUS_ALREADY_RUNNING";
        case RAAT__DISCOVERY_STOP_STATUS_NOT_RUNNING:      return "RAAT__DISCOVERY_STOP_STATUS_NOT_RUNNING";
        case RAAT__DISCOVERY_STATUS_NETWORK_ERROR:         return "RAAT__DISCOVERY_STATUS_NETWORK_ERROR";
        case RAAT__DISCOVERY_STATUS_PACKET_TOO_LARGE:      return "RAAT__DISCOVERY_STATUS_PACKET_TOO_LARGE";
        case RAAT__DISCOVERY_STATUS_INVALID_KEY_OR_VALUE:  return "RAAT__DISCOVERY_STATUS_INVALID_KEY_OR_VALUE";
        default: RC__ASSERT(0); return NULL;
    }
    return NULL;
}
