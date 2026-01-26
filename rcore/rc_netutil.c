//
// The contents of this file are subject to RC SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "rc_netutil.h"

#include <string.h>
#include <stdio.h>

RC__Status 
RC__get_arp_ips(RC__Allocator *alloc, uint32_t **out_ips, int *out_count)
{
    RC__ASSERT(alloc != NULL);
    RC__ASSERT(out_ips != NULL);
    RC__ASSERT(out_count != NULL);

    *out_count = 0;
    *out_ips = NULL;

    // XXX: implement

    return RC__STATUS_SUCCESS;
}

void 
RC__sockaddr_to_string(const void *sockaddr, char *buf/*[RC__MAX_ADDR_LEN]*/) {
    const struct sockaddr_storage *storage = sockaddr;
    char portbuf[64];

    if (storage->ss_family == AF_INET) {
        const struct sockaddr_in* in = sockaddr;
        uv_ip4_name(in, buf, RC__MAX_ADDR_LEN);
        sprintf(portbuf, ":%d", ntohs(in->sin_port));
        strcat(buf, portbuf);
    } else if (storage->ss_family == AF_INET6) {
        const struct sockaddr_in6* in6 = sockaddr;
        sprintf(portbuf, ":%d", ntohs(in6->sin6_port));
        uv_ip6_name(in6, buf, RC__MAX_ADDR_LEN);
        strcat(buf, portbuf);
    } else {
        strcpy(buf, "unknown");
    }
}

#ifdef __linux__

#include <netinet/in.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <errno.h>
#include <poll.h>
#include <sys/select.h>
#include <fcntl.h>

struct RC__NetworkStatus {
    RC__Allocator             *alloc;
    int                        sock;

    int                        pipe_read_fd;
    int                        pipe_write_fd;

    uv_thread_t                tid;
    uv_loop_t                 *loop;
    uv_timer_t                 timer;
    RC__NetworkStatusCallback  cb;
    void                      *userdata;
};

static void timer_close_cb(uv_handle_t *handle) {
}

static void notification_timer_cb(uv_timer_t *timer) {
    RC__NetworkStatus *self = timer->data;
    self->cb(self->userdata);
}

static void async_close_cb(uv_handle_t *handle) {
    RC__Allocator *alloc = handle->data;
    RC__free(alloc, handle);
}

static void notification_async_cb(uv_async_t *async) {
    RC__NetworkStatus *self = async->data;
    uv_timer_start(&self->timer, notification_timer_cb, 5000, 0);
    async->data = self->alloc;
    uv_close((uv_handle_t*)async, async_close_cb);
}

static void networkstatus_thread(void *vself) {
    RC__NetworkStatus *self = vself;
    int len;
    char buffer[4096];
    struct nlmsghdr *nlh;

    fd_set readset;
    for (;;) {
        FD_ZERO(&readset);
        FD_SET(self->sock,         &readset);
        FD_SET(self->pipe_read_fd, &readset);

        int rc = select(RC__max(self->sock, self->pipe_read_fd) + 1, &readset, NULL, NULL, NULL);
        if (rc < 0) {
            break;
        }

        if (FD_ISSET(self->pipe_read_fd, &readset)) {
            break;
        } else if (FD_ISSET(self->sock, &readset)) {
            nlh = (struct nlmsghdr *)buffer;
            if ((len = recv(self->sock, nlh, 4096, 0)) > 0) {
                bool got_something = false;
                while ((NLMSG_OK(nlh, len)) && (nlh->nlmsg_type != NLMSG_DONE)) {
                    if (nlh->nlmsg_type == RTM_NEWADDR || nlh->nlmsg_type == RTM_DELADDR || nlh->nlmsg_type == RTM_NEWLINK || nlh->nlmsg_type == RTM_DELLINK) {
                        got_something = true;
                    }
                    nlh = NLMSG_NEXT(nlh, len);
                }

                if (got_something) {
                    // notify in 2s, in case things aren't quite there yet
                    uv_async_t *async = RC__new0(self->alloc, uv_async_t, 1);
                    uv_async_init(self->loop, async, notification_async_cb);
                    async->data = self;
                    uv_async_send(async);
                }
            }
        }
    }
}

RC__NetworkStatus *
RC__networkstatus_begin_watch(RC__Allocator *alloc, uv_loop_t *loop, RC__NetworkStatusCallback cb, void *userdata) {
    int sock;
    struct sockaddr_nl addr;

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        return NULL;
    }

#ifdef SOCK_NONBLOCK
    if ((sock = socket(PF_NETLINK, SOCK_RAW | SOCK_NONBLOCK, NETLINK_ROUTE)) == -1) {
        close(pipefd[0]); close(pipefd[1]);
        return NULL;
    }
#else
    if ((sock = socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE)) == -1) {
        close(pipefd[0]); close(pipefd[1]);
        return NULL;
    }
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = RTMGRP_IPV4_IFADDR | RTMGRP_LINK;
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        return NULL;
    }

    alloc = RC__allocator_default(alloc);
    RC__NetworkStatus *self = RC__new0(alloc, RC__NetworkStatus, 1);
    if (!self) { close(sock); return NULL; }

    self->alloc         = alloc;
    self->sock          = sock;
    self->loop          = loop;
    self->cb            = cb;
    self->userdata      = userdata;
    self->pipe_read_fd  = pipefd[0];
    self->pipe_write_fd = pipefd[1];

    uv_timer_init(loop, &self->timer);
    self->timer.data = self;

    uv_thread_create(&self->tid, networkstatus_thread, self);

    return self;
}

void
RC__networkstatus_end_watch(RC__NetworkStatus *self) {
    uint8_t byte = 0;
    write(self->pipe_write_fd, &byte, 1);
    uv_thread_join(&self->tid);
    uv_close((uv_handle_t*)&self->timer, timer_close_cb);
    close(self->sock);
    close(self->pipe_read_fd);
    close(self->pipe_write_fd);
    RC__free(self->alloc, self);
}

#else

RC__NetworkStatus *
RC__networkstatus_begin_watch(RC__Allocator *alloc, uv_loop_t *loop, RC__NetworkStatusCallback cb, void *userdata) {
    return NULL;
}

void
RC__networkstatus_end_watch(RC__NetworkStatus *self) {
}
#endif
