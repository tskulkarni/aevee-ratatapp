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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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
    int                         volume_socket;
    int                         volume_thread_running;
} BricastiVolumePlugin;

/////////////
// file scope
static int tty_fd;
static struct termios tio;


//static callbacks 
static void on_connect(uv_connect_t* connection, int status);
static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);
static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
static void on_write_data(uv_write_t* wreq, int status);



// Prototypes
void uart_init(BricastiVolumePlugin *self);
int volume__manager_socket_connect(BricastiVolumePlugin *self);
void volume_manager_write_socket(BricastiVolumePlugin *self,  uint8_t data);
void  volume_manager_socket_ctl_init(BricastiVolumePlugin *self);
void volume_manager_read_thread(void*arg);
void process_volume_byte(BricastiVolumePlugin *self, uint8_t byte);

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
    char volume_str[8];
    bool changed = false;

    uv_mutex_lock(&self->lock);
    if (self->volume != volume_value) {
        self->volume = volume_value;
        RAAT__TRACE("[volume/bricasti] volume => %f", volume_value);
        uint8_t volume_byte = (int)(volume_value + 100); 
        volume_manager_write_socket(self,volume_byte);
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
    uint8_t mute_byte;
    bool changed = false;

    uv_mutex_lock(&self->lock);
    if (self->mute != mute_value) {
        self->mute = mute_value;
        RAAT__TRACE("[volume/bricasti] mute => %d", mute_value);
        if(mute_value == true) mute_byte = STREAMER_MUTE_ON;
		else if(mute_value == false) mute_byte = STREAMER_MUTE_OFF;
        volume_manager_write_socket(self,mute_value);
        changed = true;
        LOCKED_get_state(self, &state);
    }
    uv_mutex_unlock(&self->lock);

    if (changed) RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);

    return RC__STATUS_SUCCESS;
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
    //uart_init(self);
    if( volume__manager_socket_connect(self) ==0)
    {
        //uv_thread_create(&self->tid, uart_read_thread, self);
        self->volume_thread_running=1;  
        uv_thread_create(&self->tid, volume_manager_read_thread, self);
    }

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


void volume_manager_socket_ctl_init(BricastiVolumePlugin *self)
{
    
        volume__manager_socket_connect(self);

    
}

 int volume__manager_socket_connect(BricastiVolumePlugin *self)
{
        struct sockaddr_in dest;
        
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons(9000);
        self->volume_socket = socket(AF_INET, SOCK_STREAM, 0);        
        if (inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr) <= 0) {
            RAAT__ERROR("Connection to Volume Server Failed..");
            close(self->volume_socket);
            return -1;
         }
         if (connect(self->volume_socket, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
            RAAT__ERROR("Connection to Volume Server Failed..");
            close(self->volume_socket);
            return -1;
        }
        return 0;
}



void volume_manager_write_socket(BricastiVolumePlugin *self,  uint8_t data)
{
    if (send(self->volume_socket, (char *)&data, 1, 0) < 0) {
            RAAT__ERROR("Error Sending Data to Volume Server..");
        }

    
}   


void volume_manager_read_thread(void *arg)
{
    BricastiVolumePlugin *self = arg;
    uint8_t volume;
    uint8_t volume_bytes[20];
    size_t bytes_read;

    while(1) //(self->volume_thread_running)
    {
            
        size_t bytes_read = recv(self->volume_socket, &volume_bytes, 1, 0);

        if (bytes_read > 0) 
        {
            process_volume_byte(self,volume_bytes[0]);            
        }
        else if (bytes_read == 0) 
        {   
            RAAT__TRACE("Volume Server Closed Connection..");
            self->volume_thread_running = 0;
        } 
        else 
        {
            RAAT__TRACE("Error: Unknown...");
        }
        usleep(250000);
    }   
}

void process_volume_byte(BricastiVolumePlugin *self, uint8_t byte)
{
        uint8_t volume = byte;
        
        RAAT__VolumeState state;
        uv_mutex_lock(&self->lock);
        
        //RAAT__TRACE("[RX] Received %d bytes: ", bytes_read);

        if(volume == STREAMER_MUTE_ON) 
        {
            self->mute = true;
        }
        else if(volume == STREAMER_MUTE_OFF)
        { 
            self->mute = false;
        }
        else if(volume == STREAMER_STANDBY_ON || volume == STREAMER_SWITCH_REQ || volume == STREAMER_STANDBY_OFF)
        {
            g_m1_state = volume;
            (*m1_stdby_ptr)();
        }
        else if(volume == STREAMER_PHASE_INVERTED || volume == STREAMER_PHASE_NORMAL)
        {
                (*roon_signal_path_ptr)(volume);
        }
        else if(volume == STREAMER_UPSAMPLING_ACTIVE || volume == STREAMER_UPSAMPLING_INACTIVE)
        {
            (*roon_signal_path_ptr)(volume);
        }
#if 0
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
#endif
        else if(volume < 0x80)
        {
            self->volume = volume - 100;  //was  self->volume = volume; /*NEW_VOLUME_FROM_UART*/0;
        }
        LOCKED_get_state(self, &state);
        uv_mutex_unlock(&self->lock);
        RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);

}

