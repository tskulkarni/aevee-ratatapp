//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//

////////////
// INCLUDES
#include<string.h>
#include<stdio.h>
#include<stdlib.h>
#include<stdint.h>
#include<unistd.h>
#include<fcntl.h>
#include<termios.h>
#include<stdio.h>
#include<stdbool.h>
#include "raat_plugin_volume_dummy.h"
#include "rc_list.h"
#include <uv.h>
#include <errno.h>

//////////
// MACROS
#define RAAT__CURRENT_LOG self->log
// STREAMER STATUS CODES
#define STREAMER_CONNECTED    0xA1
#define STREAMER_DISCONNECTED 0xA2
#define STREAMER_SWITCH_REQ   0xA4
#define STREAMER_MUTE_ON      0xA5
#define STREAMER_MUTE_OFF     0xA6
#define STREAMER_STANDBY_ON   0xA7
#define STREAMER_STANDBY_OFF  0xA8
#define STREAMER_PHASE_INVERTED       0xA9
#define STREAMER_PHASE_NORMAL         0xAA
#define STREAMER_BALANCE_PLUS         0xAB   // second byte is data
#define STREAMER_BALANCE_MINUS        0xAC   // second byte is data
#define STREAMER_UPSAMPLING_ACTIVE    0xAD
#define STREAMER_UPSAMPLING_INACTIVE  0xAE

// global declaration
//extern bool g_roon_stdby_flag;	// mcmurray
bool g_roon_stdby_flag = false;		// mcmurray

// prototype
void m1_stdby(void);
void (* m1_stdby_ptr)(void) = NULL;
uint8_t g_m1_state;
void roon_signal_path(uint8_t);
void (* roon_signal_path_ptr)(uint8_t) = NULL;

/////////////////////
// STRUCT DEFINITIONS
/*
 * Volume Plugin
 */
typedef struct {
    RAAT__VolumePlugin          plugin;          // must be first item in struct
    RC__Allocator              *alloc;
    RAAT__Log                  *log;
    json_t                     *info;
    json_t                     *config;
    RAAT__VolumeStateListeners  state_listeners;
    uv_mutex_t                  lock;
    uv_thread_t                 tid;
    bool                        mute;
    double                      volume;
} BricastiVolumePlugin;

/////////////
// file scope
static int tty_fd;
static struct termios tio;
static  int pipe_fd;
const char *volume_fifo = "/opt/Streamer/raat_fifo";
// Prototypes
void uart_init(void);

static RC__Status volume_add_state_listener(void *vself, RAAT__VolumeStateCallback cb, void *userdata) {
    BricastiVolumePlugin *self = (BricastiVolumePlugin*)vself;
    return RAAT__volume_state_listeners_add(&self->state_listeners, cb, userdata);
}

static RC__Status volume_remove_state_listener(void *vself, RAAT__VolumeStateCallback cb, void *userdata) {
    BricastiVolumePlugin *self = (BricastiVolumePlugin*)vself;
    return RAAT__volume_state_listeners_remove(&self->state_listeners, cb, userdata);
}

static RC__Status volume_get_info(void *vself, json_t **out_info) {
    BricastiVolumePlugin *self = (BricastiVolumePlugin*)vself;

    RC__ASSERT(self);
    RC__ASSERT(out_info);

    json_incref(self->info);
    *out_info = self->info;

    return RC__STATUS_SUCCESS;
}

static void LOCKED_get_state(BricastiVolumePlugin *self, RAAT__VolumeState *out_state) {
    memset(out_state, 0, sizeof(RAAT__VolumeState));

    RC__ASSERT(self      != NULL);
    RC__ASSERT(out_state != NULL);

    // TODO: make sure to set these values up appropraitely. This example is for a dB control from -80dB to 0dB.
    out_state->volume_type   = RAAT__VOLUME_TYPE_DB; // either RAAT__VOLUME_TYPE_DB if the frontpanel displays dB, otherwise RAAT__VOLUME_TYPE_NUMBER
    out_state->min_volume    = -100;                  // min volume on the front panel display
    out_state->max_volume    = 0;                    // max volume on the front panel display
    out_state->db_min_volume = -100;                  // min volume, measured in dB. for dB controls, this is the same as min_volume, otherwise should reflect the dB adjustment of min_volume
    out_state->db_max_volume = 0;                    // max volume, measured in dB. for dB controls, this is same as max_volume, otherwise should reflect the dB adjustment of max_volume
    out_state->volume_step   = 1.0;                  // step size for front panel control. Usually 0.1, 0.5 or 1.0 for dB. Usually 1.0 for NUMBER

    // current values
    out_state->volume_value  = self->volume;
    out_state->mute_value    = self->mute;
}

static RC__Status volume_get_state(void *vself, RAAT__VolumeState *out_state) {
    BricastiVolumePlugin *self = (BricastiVolumePlugin*)vself;

    RC__ASSERT(self != NULL);
    RC__ASSERT(out_state != NULL);

    uv_mutex_lock(&self->lock);
    LOCKED_get_state(self, out_state);
    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

static RC__Status volume_set_volume(void *vself, double volume_value) {
    BricastiVolumePlugin *self = (BricastiVolumePlugin*)vself;
    RAAT__VolumeState state;
    bool changed = false;

    uv_mutex_lock(&self->lock);
    if (self->volume != volume_value) {
        self->volume = volume_value;
        RAAT__TRACE("[volume/bricasti] volume => %f", volume_value);
        // TODO: notify UART of volume state. Avoid doing any activity that takes more than ~100ms here, like blocking I/O.
        //      If you have to do something that could possibly take longer, do it in a bg thread
#if 1
    // mcmurray -- write volume to DSP via UART
    ssize_t tty_fd_wr;
    uint8_t volume_byte = (int)(volume_value + 100); // was uint8_t volume_byte = (int)volume_value;
//    tty_fd = open("/dev/ttyO1", O_RDWR | O_NONBLOCK);
    {
//        tcsetattr(tty_fd, TCSANOW, &tio);
        tty_fd_wr = write(tty_fd, &volume_byte, 1);
    }
//    close(tty_fd);
#endif
        changed = true;
        LOCKED_get_state(self, &state);
    }
    uv_mutex_unlock(&self->lock);

    if (changed) RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);

    return RC__STATUS_SUCCESS;
}

static RC__Status volume_set_mute(void *vself, bool mute_value) {
    BricastiVolumePlugin *self = (BricastiVolumePlugin*)vself;
    RAAT__VolumeState state;
    bool changed = false;

    uv_mutex_lock(&self->lock);
    if (self->mute != mute_value) {
        self->mute = mute_value;
        RAAT__TRACE("[volume/bricasti] mute => %d", mute_value);
        // TODO: notify UART of mute state. Avoid doing any activity that takes more than ~100ms here, like blocking I/O.
        //       If you have to do something that could possibly take longer, do it in a bg thread

       // update DSP with Roon mute status -- mcmurray
		    ssize_t tty_fd_wr;
		    uint8_t mute_byte;
		    if(mute_value == true) mute_byte = STREAMER_MUTE_ON;
		    else if(mute_value == false) mute_byte = STREAMER_MUTE_OFF;
		    tty_fd_wr = write(tty_fd, &mute_byte, 1);

        changed = true;
        LOCKED_get_state(self, &state);
    }
    uv_mutex_unlock(&self->lock);

    if (changed) RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);

    return RC__STATUS_SUCCESS;
}

// mcmurray -- read volume from DSP via UART
static void uart_read_thread(void*arg)
{
    BricastiVolumePlugin *self = arg;
    uint8_t volume;
    uint8_t volume_bytes[20];
    ssize_t bytes_read;

    // while (read from uart + plugin has not been deleted)
    // TODO: read from UART. If you get a new value for volume or mute,
    while(1)
    {
#if 1
                // check UART to see if DSP has updated volume
                //if( (tty_fd = open("/dev/ttyO1", O_RDWR | O_NONBLOCK)) > 0)
//                if( (tty_fd = open("/dev/ttyO1", O_RDWR | O_NOCTTY)) > 0)
                {
//                        uart_init();
//                      tcsetattr(tty_fd, TCSANOW, &tio);
                        //lseek(tty_fd, -1, SEEK_END);
                        bytes_read = read(tty_fd, volume_bytes, 20);
RAAT__TRACE("BYTES => %d , ERRNO => %d", bytes_read, errno);
//                        close(tty_fd);
                }

// TEST CODE -- start
#if 0
static uint8_t testCnt = 0;
bytes_read = 1;
volume_bytes[0] = testCnt++;
#endif
// TEST CODE -- end
             // Get the latest volume level, if there is any
             if(bytes_read > 0)
             {
                    volume = volume_bytes[bytes_read - 1];
                    
                    //Write the byte to the name pipe 
                    write(pipe_fd, &volume, 1);
                    
                    ////////////////////////////////////////
                    // (1) update the vars
                    // (2) generate a new RAAT__VolumeState
                    // (3) notify listeners of a state change

                    RAAT__VolumeState state;
                    uv_mutex_lock(&self->lock);

                   // Check first, if we received a Mute related command and tell RAAT -- mcmurray
                   if(volume == STREAMER_MUTE_ON) self->mute = true;
                   else if(volume == STREAMER_MUTE_OFF) self->mute = false;
		   else if(volume == STREAMER_STANDBY_ON || volume == STREAMER_SWITCH_REQ || volume == STREAMER_STANDBY_OFF)
                   {
                       g_m1_state = volume;
                       (*m1_stdby_ptr)();
                   }
                   else if(volume == STREAMER_PHASE_INVERTED || volume == STREAMER_PHASE_NORMAL)
                   {
//                       roon_signal_path(volume);
                         (*roon_signal_path_ptr)(volume);
                   }
                   else if(volume == STREAMER_UPSAMPLING_ACTIVE || volume == STREAMER_UPSAMPLING_INACTIVE)
                   {
                        (*roon_signal_path_ptr)(volume);
                   }
                   // Did we get a balance signal from SHARC  --                                                                                       
                   else if(volume_bytes[bytes_read - 2] == STREAMER_BALANCE_PLUS)
                   {
                       // remove 0xC0 mask, send balance data
                       uint8_t balance_data = volume - 0xC0;
                       (*roon_signal_path_ptr)(balance_data);
                   }
                   else if(volume_bytes[bytes_read - 2] == STREAMER_BALANCE_MINUS)
                   {
                       // keep 0xC0 mask, send balance data
                       uint8_t balance_data = volume;
                       (*roon_signal_path_ptr)(balance_data);
                   }
                   else if(volume < 0x80)
                    self->volume = volume - 100;  //was  self->volume = volume; /*NEW_VOLUME_FROM_UART*/0;
                    //self->mute   = /*NEW_MUTE_FROM_UART*/false;

#if 0   // Upsampling
 static int sample_rate_last = 0;

 // Has sample rate changed
 if(*g_sample_rate_ptr != sample_rate_last)
 {
     //(*roon_signal_path_ptr)(STREAMER_UPSAMPLING_ACTIVE);    // Actual
     (*roon_signal_path_ptr)(0);                             // test
     sample_rate_last = *g_sample_rate_ptr;
 }
#endif  // Upsampling

                    /////////////////////////////////////////////////////////////////////////////
                    // This is now a section that uses the polling to write to uart -- mcmurray
                    /////////////////////////////////////////////////////////////////////////////
#if 0		    // mcmurray
                    uint8_t stdby_byte = STREAMER_STANDBY_ON;
                    if(g_roon_stdby_flag == true)               // did M1 put us into Stdby -- mcmurray
                    {
                        write(tty_fd, &stdby_byte, 1);
                        g_roon_stdby_flag = false;
                    }
#endif

                    LOCKED_get_state(self, &state);
                    uv_mutex_unlock(&self->lock);

                    RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);
              }
#endif

                    /////////////////////////////////////////////////////////////////////////////
                    // This is now a section that uses the polling to write to uart -- mcmurray
                    /////////////////////////////////////////////////////////////////////////////
#if 1
                    uint8_t stdby_byte = STREAMER_STANDBY_ON;
                    if(g_roon_stdby_flag == true)               // did M1 put us into Stdby -- mcmurray
                    {
                        write(tty_fd, &stdby_byte, 1);
                        g_roon_stdby_flag = false;
                    }
#endif

              // sleep for 250ms until it's time to poll uart again
              usleep(250000);
    }
}

void uart_init(void)
{
    
        memset(&tio, 0, sizeof(tio));
        tio.c_iflag = 0;
        tio.c_oflag = 0;
        tio.c_cflag = CS8|CREAD|CLOCAL;
        tio.c_lflag = 0;
        tio.c_cc[VMIN] = 0;     // MIN/TIME: 1/5,  0/0, 0/5
        tio.c_cc[VTIME] = 0;
        cfsetospeed(&tio, B115200);
        cfsetispeed(&tio, B115200);

    tty_fd = open("/dev/ttyO1", O_RDWR | O_NONBLOCK);
    tcsetattr(tty_fd, TCSANOW, &tio);
    //Also open the fifo
    pipe_fd = open(volume_fifo, O_WRONLY);
    if(pipe_fd<0)
    {
        RAAT__ERROR("Cannot open ipc fifo, volume data will not be passed..");
    }
}

RC__Status
RAAT__dummy_volume_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__VolumePlugin **out_volume) {
    alloc = RC__allocator_default(alloc);
    BricastiVolumePlugin *self            = RC__new0(alloc, BricastiVolumePlugin, 1);
    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;
    self->alloc                        = alloc;
    self->log                          = RAAT__device_get_log(device);
    self->plugin.get_info              = volume_get_info;
    self->plugin.add_state_listener    = volume_add_state_listener;
    self->plugin.remove_state_listener = volume_remove_state_listener;
    self->plugin.get_state             = volume_get_state;
    self->plugin.set_volume            = volume_set_volume;
    self->plugin.set_mute              = volume_set_mute;


    uv_mutex_init(&self->lock);
    RAAT__volume_state_listeners_init(&self->state_listeners, self->alloc);

    // TODO: populate initial volume/mute state from device. This should be maintained across restarts somewhere.
    self->volume = 0;
    self->mute   = false;

    // kick off the background thread that reads from UART
    uart_init();
    uv_thread_create(&self->tid, uart_read_thread, self);

    self->config = json_deep_copy(config);

    self->info = json_object();
    json_object_set(self->info, "config", config);

    RAAT__TRACE("[volume/bricasti] initialized");

    *out_volume = &self->plugin;
    return RC__STATUS_SUCCESS;
}

void
RAAT__bricasti_volume_plugin_delete(RAAT__VolumePlugin *volume) {
    BricastiVolumePlugin *self = (BricastiVolumePlugin*)volume;
    uv_mutex_destroy(&self->lock);
    RAAT__volume_state_listeners_destroy(&self->state_listeners);

    uv_mutex_destroy(&self->lock);
    if (self->info) json_decref(self->info);
    json_decref(self->config);
    RAAT__volume_state_listeners_destroy(&self->state_listeners);

    // TODO: notify uart_read_thread that plugin has been deleted --set a flag, or close an fd, or whatever makes sense for you

    uv_thread_join(&self->tid);         // this waits for the uart read thread to exit

    RC__free(self->alloc, self);
}

