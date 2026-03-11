//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//

#if 1
// Includes -- mcmurray
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#endif

#include "raat_plugin_source_selection_test.h"
#include "rc_list.h"

#include <uv.h>
#include<stdbool.h>

#define RAAT__CURRENT_LOG self->log
#define STREAMER_STANDBY_ON   0xA7
#define STREAMER_STANDBY_OFF  0xA8
#define STREAMER_SWITCH_REQ   0xA4

// globals
//bool g_roon_stdby_flag = false;	// mcmurray
extern g_roon_stdby_flag;		// mcmurray
extern uint8_t g_m1_state;		// mcmurray
uint8_t roon_stdby_delay = false;	// mcmurray delay conditional

// prototypes and function ptrs
extern void (* m1_stdby_ptr)(void);

/*
 * SourceSelection Plugin
 */
typedef struct {
    RAAT__SourceSelectionPlugin          plugin;          // must be first item in struct
    uv_mutex_t                           lock;
    RC__Allocator                       *alloc;
    RAAT__SourceSelectionStateListeners  state_listeners;
    RAAT__Log                           *log;
    RAAT__SourceSelectionStatus          status;
    RAAT__OutputPlugin                  *output;
    json_t                              *info;
    int                                  request_source_seq;
} TestSourceSelectionPlugin;
static TestSourceSelectionPlugin *g_self_ptr = NULL;

static RC__Status source_selection_add_state_listener(void *vself, RAAT__SourceSelectionStateCallback cb, void *userdata) {
    TestSourceSelectionPlugin *self = (TestSourceSelectionPlugin*)vself;
    return RAAT__source_selection_state_listeners_add(&self->state_listeners, cb, userdata);
}

static RC__Status source_selection_remove_state_listener(void *vself, RAAT__SourceSelectionStateCallback cb, void *userdata) {
    TestSourceSelectionPlugin *self = (TestSourceSelectionPlugin*)vself;
    return RAAT__source_selection_state_listeners_remove(&self->state_listeners, cb, userdata);
}

static RC__Status source_selection_get_info(void *vself, json_t **out_info) {
    TestSourceSelectionPlugin *self = (TestSourceSelectionPlugin*)vself;

    RC__ASSERT(self);
    RC__ASSERT(out_info);

    json_incref(self->info);
    *out_info = self->info;

    return RC__STATUS_SUCCESS;
}

static RC__Status source_selection_get_state(void *vself, RAAT__SourceSelectionState *out_state) {
    TestSourceSelectionPlugin *self = (TestSourceSelectionPlugin*)vself;

    RC__ASSERT(self != NULL);
    RC__ASSERT(out_state != NULL);

    out_state->status   = self->status;

    return RC__STATUS_SUCCESS;
}

#define MPD_IP        ("127.0.0.1")
#define MPD_INET_PORT (6600)

typedef struct {
    TestSourceSelectionPlugin                   *self;
    RAAT__SourceSelectionRequestSourceCallback   cb;
    void                                        *cb_userdata;
    int                                          seq;
} RequestSourceState;

static void request_source_thread(void *vself) {
#define FAIL(msg,errcode) { if (errcode == -1) RAAT__ERROR("[source_selection/mpd] " msg); else RAAT__ERROR("[source_selection/mpd] " msg ": %s", strerror(errcode)); goto fail; }
    RequestSourceState        *state = vself;
    TestSourceSelectionPlugin *self  = state->self;

    int rc;
    FILE *file = NULL;
    int sock = -1;

    int tty_fd;
    struct termios tio;
    unsigned char c = (unsigned char)0xA1;
    unsigned char d = (unsigned char)0xA2;
    static unsigned char e = (unsigned char)0xA4;
    ssize_t tty_fd_wr;

    // create the socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (!sock) FAIL("socket() failed", errno)

    // switch to nonblocking mode so we can connect with timeout
    rc = fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    if (rc < 0) FAIL("fcntl(F_SETFL) failed", errno)

    // connect to mpd
    RAAT__TRACE("[source_selection/mpd] connecting to mpd..");
    struct sockaddr_in name = {0,};
    name.sin_family      = AF_INET;
    name.sin_addr.s_addr = inet_addr(MPD_IP);
    name.sin_port        = htons(MPD_INET_PORT);
    rc = connect(sock, (struct sockaddr *)&name, sizeof(name));
    if (rc < 0 && errno != EINPROGRESS) FAIL("connect() failed", errno)

    // wait for up to 5 seconds for connect() to complete. if it fails, fail
    struct timeval connect_timeout = { 5, 0 };
    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(sock, &fdset);
    if (select(sock+1, NULL, &fdset, NULL, &connect_timeout) == 1) {
        socklen_t len       = sizeof(rc);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &rc, &len);
        if (rc == 0) {
            RAAT__TRACE("[source_selection/mpd] connected");
        } else {
            RAAT__WARNING("[source_selection/mpd] MPD is not accepting connections, so assuming that it does not have the audio device--this should not happen");
            goto success;
        }
    } else {
        RAAT__WARNING("[source_selection/mpd] MPD is not accepting connections, so assuming that it does not have the audio device--this should not happen");
        goto success;
    }

    // go back to blocking mode since we will do blocking i/o to speak the protocol
    rc = fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) & ~O_NONBLOCK);
    if (rc < 0) FAIL("fcntl(F_SETFL) failed", errno)

    // set timeouts, so if things stop moving on the socket for 5 seconds, we bail out and fail instead of
    // potentially hanging forever
    struct timeval send_recv_timeout = { 5, 0 };
    rc = fcntl(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&send_recv_timeout, sizeof(send_recv_timeout));
    if (rc < 0) FAIL("fcntl(SO_RCVTIMEO) failed", errno)

    rc = fcntl(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&send_recv_timeout, sizeof(send_recv_timeout));
    if (rc < 0) FAIL("fcntl(SO_SNDTIMEO) failed", errno)

    // open the file using stdio so we don't have to parse for newlines ourselves
    char buf[65536] = {0,};
    file = fdopen(sock, "r+b");   // convert to FILE* so we can use stdio to read/write lines at a time
    sock = -1;                    // set sock to -1 so we don't double-close it later. file owns it now

    char *line = fgets(buf, sizeof(buf), file); // read first line. Should be something like "OK MPD 0.19.0"
    if (!line) FAIL("connection dropped", -1);
    if (strstr(line, "OK MPD") != line) FAIL("expected OK line from MPD after connect", -1);

    bool needs_stop = true;

    fputs("status\n", file);
    //RAAT__DEBUG("[source_selection/mpd] [mpd] SENT status");

    for (;;) {
        line = fgets(buf, sizeof(buf), file); // read first line. Should be something like "OK MPD 0.19.0"
        if (!line) FAIL("connection dropped", -1);
        //RAAT__TRACE("[source_selection/mpd] [mpd] GOT %s", line);
        if      (strstr(line, "OK")  == line) break;
        else if (strstr(line, "ACK") == line) FAIL("got ACK from MPD", -1)
        else if (strstr(line, "state:") == line && strstr(line, "stop")) needs_stop = false;
    }

    if (needs_stop) {
        RAAT__TRACE("[source_selection/mpd] MPD is not stopped. Asking it to stop.");
        fputs("stop\n", file);
        //RAAT__DEBUG("[source_selection/mpd] [mpd] SENT stop");
        for (;;) {
            line = fgets(buf, sizeof(buf), file); // read first line. Should be something like "OK MPD 0.19.0"
            if (!line) FAIL("connection dropped", -1);
            //RAAT__TRACE("[source_selection/mpd] [mpd] GOT %s", line);
            if      (strstr(line, "OK")  == line) {
                RAAT__TRACE("[source_selection/mpd] MPD stop command was successful");
                break;
            }
            else if (strstr(line, "ACK") == line) FAIL("got ACK from MPD", -1);
        }

        // now wait for MPD to transition to stopped state, polling for up to 20 iterations (5 seconds)
        int iter = 20;
        bool is_stopped = false;
            RAAT__TRACE("[source_selection/mpd] waiting for state change from MPD");
        for (;;) {
            fputs("status\n", file);
            //RAAT__DEBUG("[source_selection/mpd] [mpd] SENT status");

            for (;;) {
                line = fgets(buf, sizeof(buf), file); // read first line. Should be something like "OK MPD 0.19.0"
                if (!line) FAIL("connection dropped", -1);
                //RAAT__DEBUG("[source_selection/mpd] [mpd] GOT %s", line);
                if      (strstr(line, "OK")  == line) break;
                else if (strstr(line, "ACK") == line) FAIL("got ACK from MPD", -1)
                else if (strstr(line, "state:") == line && strstr(line, "stop")) {
                    RAAT__TRACE("[source_selection/mpd] got state change--MPD is stopped");
                    is_stopped = true;
                }
            }
            if (is_stopped)  break;
            if (--iter == 0) break;

            usleep(250000);     // wait 250ms before retrying
        }

        if (!is_stopped) FAIL("MPD failed to stop within 5s timeout", -1);

    } else {
        RAAT__TRACE("[source_selection/mpd] stop not needed, since MPD is already stopped");
    }

success:
#if 1    // M1 convenience Switching related
    tty_fd = open("/dev/ttyO1", O_RDWR | O_NONBLOCK);
    {
        memset(&tio, 0, sizeof(tio));
        tio.c_iflag = 0;
        tio.c_oflag = 0;
        tio.c_cflag = CS8|CREAD|CLOCAL;
        tio.c_lflag = 0;
        tio.c_cc[VMIN] = 1;
        tio.c_cc[VTIME] = 5;
        cfsetospeed(&tio, B115200);
        cfsetispeed(&tio, B115200);
        tcsetattr(tty_fd, TCSANOW, &tio);
        tty_fd_wr = write(tty_fd, &e, 1);
#if 0
        printf(" CONVENIENCE SWITCHING TEST 11 \n");
        if(e == c)
        {
            tty_fd_wr = write(tty_fd, &d, 1);
            e = d;
        }
        else
        {
            tty_fd_wr = write(tty_fd, &c, 1);
            e = c;
        }
        printf(" CONVENIENCE SWITCHING TEST 22 \n");
        printf("open_fd: %d, write_fd: %zd\n", tty_fd, tty_fd_wr);
#endif
    }
    close(tty_fd);
#endif

    //Notify quboz 
    FILE *q_pidfile = fopen("/run/quboz-aes.pid","r");
    int q_pid;
    if(q_pidfile != NULL){
         if (fscanf(q_pidfile, "%d", &q_pid) == 1) {
            kill(q_pid,SIGHUP);
        }
        else {
            RAAT__TRACE("Could not read an integer from the file, or the file was empty.\n");
        }
        fclose(q_pidfile);
    }
    q_pidfile = NULL;
    //Notify quboz 
    q_pidfile = fopen("/run/quboz-usb.pid","r");
    if(q_pidfile != NULL){
         if (fscanf(q_pidfile, "%d", &q_pid) == 1) {
            kill(q_pid,SIGHUP);
        }
        else {
            RAAT__TRACE("Could not read an integer from the file, or the file was empty.\n");
        }
        fclose(q_pidfile);
    }
    
    


// Add delay to allow audio to start before having Roon switch
if(roon_stdby_delay == true)  // delay only if in standby
{
    usleep(250000);
    usleep(250000);
    usleep(250000);
    usleep(250000);
    usleep(250000);
    usleep(250000);
    usleep(250000);
    usleep(250000);
    usleep(250000);
    usleep(250000);
    usleep(250000);
    usleep(250000);
    usleep(250000);
    usleep(250000);
    roon_stdby_delay = false;
}

    RAAT__TRACE("[source_selection/mpd] request_source was successful");
    uv_mutex_lock(&self->lock);
    if (state->seq == self->request_source_seq) {
        self->status = RAAT__SOURCE_SELECTION_STATUS_SELECTED;
        RAAT__SourceSelectionState state = {0,};
        state.status = self->status;
        RAAT__source_selection_state_listeners_invoke(&self->state_listeners, &state);
    }
    uv_mutex_unlock(&self->lock);

    state->cb(state->cb_userdata, RC__STATUS_SUCCESS, NULL);
    goto cleanup;

fail:
    state->cb(state->cb_userdata, RC__STATUS_UNEXPECTED_ERROR, NULL);
cleanup:
    if (file)      fclose(file);
    if (sock >= 0) close(sock);
    RC__free(self->alloc, state);

#undef FAIL
}

static void source_selection_request_source(void *vself, RAAT__SourceSelectionRequestSourceCallback cb, void *cb_userdata) {
    TestSourceSelectionPlugin *self = (TestSourceSelectionPlugin*)vself;

    RAAT__TRACE("[source_selection/test] requesting source");
    RC__ASSERT(self);

    uv_mutex_lock(&self->lock);
    RequestSourceState *state = RC__new0(self->alloc, RequestSourceState, 1);
    state->self        = self;
    state->cb          = cb;
    state->cb_userdata = cb_userdata;
    state->seq         = ++self->request_source_seq;
    uv_mutex_unlock(&self->lock);

    uv_thread_t tid;
    uv_thread_create(&tid, request_source_thread, state);
}

static void source_selection_request_standby(void *vself, RAAT__SourceSelectionRequestSourceCallback cb, void *cb_userdata) {
    TestSourceSelectionPlugin *self = (TestSourceSelectionPlugin*)vself;

    RAAT__TRACE("[source_selection/test] requesting standby");
    RC__ASSERT(self);

    RAAT__TRACE("[source_selection/test] in standby");

    g_roon_stdby_flag = true;// mcmurray --  Roon put us in stdby
    roon_stdby_delay = true; // mcmurray -- stdby for delay or not
    self->status = RAAT__SOURCE_SELECTION_STATUS_STANDBY;
    RAAT__SourceSelectionState state = {0,};
    state.status = self->status;
    RAAT__source_selection_state_listeners_invoke(&self->state_listeners, &state);

    cb(cb_userdata, RC__STATUS_SUCCESS, NULL);
}

void m1_stdby(void)
{
     TestSourceSelectionPlugin *self = g_self_ptr;

     if(g_m1_state == STREAMER_STANDBY_ON)
     {   
        roon_stdby_delay = true; // mcmurray -- stdby for delay or not
	// stop audio
	{
    		json_t *reason = json_object();
    		json_object_set_new(reason, "reason", json_string("standby"));
    		self->output->force_teardown(self->output, reason);
    		json_decref(reason);
	}

	// update state
    	uv_mutex_lock(&self->lock);
        {
            RAAT__TRACE("McMurray status => %d", self->status);
            self->status = RAAT__SOURCE_SELECTION_STATUS_STANDBY;
            RAAT__SourceSelectionState state = {0,};
            state.status = self->status;
            RAAT__source_selection_state_listeners_invoke(&self->state_listeners, &state);
            RAAT__TRACE("McMurray status => %d", self->status);
        }
        uv_mutex_unlock(&self->lock);
     }
     else if(g_m1_state == STREAMER_SWITCH_REQ)
     {
	// stop audio
	{
    		json_t *reason = json_object();
    		json_object_set_new(reason, "reason", json_string("source_deselected"));
    		self->output->force_teardown(self->output, reason);
    		json_decref(reason);
	}

	// update state
    	uv_mutex_lock(&self->lock);
        {
                self->status = RAAT__SOURCE_SELECTION_STATUS_DESELECTED;
                RAAT__SourceSelectionState state = {0,};
                state.status = self->status;
                RAAT__source_selection_state_listeners_invoke(&self->state_listeners, &state);
        }
        uv_mutex_unlock(&self->lock);
     }   
     else if(g_m1_state == STREAMER_STANDBY_OFF)
     {
        roon_stdby_delay = false; // mcmurray -- stdby for delay or not
        // update state
        uv_mutex_lock(&self->lock);
        {
                self->status = RAAT__SOURCE_SELECTION_STATUS_DESELECTED;
                RAAT__SourceSelectionState state = {0,};
                state.status = self->status;
                RAAT__source_selection_state_listeners_invoke(&self->state_listeners, &state);
        }
        uv_mutex_unlock(&self->lock);
     }
}

RC__Status
RAAT__test_source_selection_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__SourceSelectionPlugin **out_source_selection) {
    alloc = RC__allocator_default(alloc);
    TestSourceSelectionPlugin *self            = RC__new0(alloc, TestSourceSelectionPlugin, 1);

    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;
    self->alloc                        = alloc;
    self->log                          = RAAT__device_get_log(device);
    self->plugin.get_info              = source_selection_get_info;
    self->plugin.add_state_listener    = source_selection_add_state_listener;
    self->plugin.remove_state_listener = source_selection_remove_state_listener;
    self->plugin.get_state             = source_selection_get_state;
    self->plugin.request_source        = source_selection_request_source;
    self->plugin.request_standby       = source_selection_request_standby;
    self->status                       = RAAT__SOURCE_SELECTION_STATUS_DESELECTED;
    self->output                       = RAAT__device_get_output_plugin(device);

    g_self_ptr = self;		       // mcmurray
    m1_stdby_ptr = &m1_stdby;	       // mcmurray

    uv_mutex_init(&self->lock);
    RAAT__source_selection_state_listeners_init(&self->state_listeners, self->alloc);

    self->info = json_object();
    json_object_set(self->info, "config", config);

    RAAT__TRACE("[source_selection/testings] initialized");

    *out_source_selection = &self->plugin;
    return RC__STATUS_SUCCESS;
}

void
RAAT__test_source_selection_plugin_delete(RAAT__SourceSelectionPlugin *source_selection) {
    TestSourceSelectionPlugin *self = (TestSourceSelectionPlugin*)source_selection;
    uv_mutex_destroy(&self->lock);
    if (self->info) json_decref(self->info);
    RAAT__source_selection_state_listeners_destroy(&self->state_listeners);
    RC__free(self->alloc, self);
}

