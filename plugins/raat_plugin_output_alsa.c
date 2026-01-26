// 
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_plugin_output_alsa.h"
#include "raat_dsp.h"
#include "rc_list.h"

#include <uv.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <limits.h>
#include <sys/time.h>
#include <alloca.h>

#include <alsa/asoundlib.h>

#define SUPPORTS_NATIVE_DSD (defined(ENABLE_NATIVE_DSD) || \
                             (!defined(DISABLE_NATIVE_DSD) && (SND_LIB_MAJOR >= 2 || (SND_LIB_MAJOR == 1 && SND_LIB_MINOR >= 1) || (SND_LIB_MAJOR == 1 && SND_LIB_MINOR == 0 && SND_LIB_SUBMINOR >= 29)))) 

#define RAAT__CURRENT_LOG self->log

#define STREAMER_PHASE_INVERTED         0xA9
#define STREAMER_PHASE_NORMAL           0xAA
#define STREAMER_BALANCE_PLUS           0xAB   // second byte is data
#define STREAMER_BALANCE_MINUS          0xAC   // second byte is data
#define STREAMER_UPSAMPLING_ACTIVE      0xAD
#define STREAMER_UPSAMPLING_INACTIVE    0xAE

/*
 * Output Plugin
 */

typedef enum {
    IDLE                = 0,
    STOPPED             = 1,
    RUNNING             = 2,
} AlsaOutputState;

typedef enum {
    DSD_MODE_NONE,
    DSD_MODE_NATIVE,
    DSD_MODE_DOP,
    DSD_MODE_DCS,
    DSD_MODE_NATIVE_OR_DOP,
    DSD_MODE_NATIVE_OR_DCS,
    DSD_MODE_DOP_OR_NATIVE,
    DSD_MODE_DCS_OR_NATIVE,
} DsdMode;

typedef struct {
    RAAT__OutputPlugin           plugin;          // must be first item in struct

    RC__Allocator               *alloc;
    RAAT__Log                   *log;

    uv_mutex_t                   lock;

    RAAT__OutputMessageListeners message_listeners;

    AlsaOutputState             state;

    json_t                      *custom_signal_path;
    json_t                      *soft_volume_signal_path;

    int                          next_token;

    RAAT__StreamFormat           origformat;
    RAAT__StreamFormat           pcmformat;  
    int                          current_token;
    RAAT__OutputLostCallback     cb_lost;
    void                        *cb_lost_userdata;

    RAAT__Stream                *stream;

    int64_t                      last_monotonic_sample_systime;  // in ns (RC__now_ns())
    int64_t                      last_monotonic_sample;          // in samples, based on counter

    int64_t                      start_streamsample;
    int64_t                      start_time;               
    bool                         new_stream;

    // state for alsa
    const char                  *unique_id;
    snd_pcm_t                   *pcm;
    unsigned                     periods;
    double                       buffer_duration_secs;
    snd_pcm_uframes_t            pcm_samples_per_buf;
    snd_pcm_format_t             hw_format;
    int                          hw_channels;
    DsdMode                      dsd_mode;
    DsdMode                      effective_dsd_mode;
    int                          max_dsd_rate;
    int                          max_pcm_rate;

    double                       resync_delay_secs;
    int64_t                      resync_delay_remaining_ns;
    int64_t                      last_delay_ns;
    int64_t                      sync_offset_ns;;

    bool                         dsd_dop_flipper;

    RAAT__OutputSetupCallback    setup_cb;
    void *                       setup_cb_userdata;

    RC__Status                   get_supported_formats_status;
    RAAT__StreamFormat          *supported_formats;
    size_t                       n_supported_formats;

    int                          volume_delay;
    RAAT__DriftCorrection        drift_correction;

    uv_thread_t                  tid;

    bool                         force_max_volume;

    RAAT__OutputSoftwareVolume  *volume;

    json_t                      *info;
    json_t                      *config;
} AlsaOutputPlugin;

// proto function ptrs and globals -- mcmurray
extern void (* roon_signal_path_ptr)(uint8_t);
RAAT__OutputPlugin *g_output_plugin_ptr;

static RC__Status output_get_info(void *vself, json_t **out_info) {
    AlsaOutputPlugin *self = (AlsaOutputPlugin*)vself;

    RC__ASSERT(self);
    RC__ASSERT(out_info);

    json_incref(self->info);
    *out_info = self->info;

    return RC__STATUS_SUCCESS;
}

static void LOCKED_update_signal_path(AlsaOutputPlugin *self) {
    json_t *message = json_object();
    json_t *signal_path    = json_array();

    if (self->soft_volume_signal_path) {
        if (json_typeof(self->soft_volume_signal_path) == JSON_ARRAY) {
            size_t index;      
            json_t *value;
            json_array_foreach(self->soft_volume_signal_path, index, value) {
                json_array_append(signal_path, value);
            }
        } else {
            RAAT__WARNING("invalid software volume signal path: must be JSON_ARRAY");
            RC__ASSERT(0);
        }
    }

    if (self->custom_signal_path) {
        size_t index;
        json_t *value;

        json_array_foreach(self->custom_signal_path, index, value) {
            json_t *condition = json_object_get(value, "condition");
            if (condition) {
                json_t *j_sample_rate     = json_object_get(condition, "sample_rate");
                if (j_sample_rate && json_integer_value(j_sample_rate) != self->origformat.sample_rate)
                    continue;

                json_t *j_bits_per_sample     = json_object_get(condition, "bits_per_sample");
                if (j_bits_per_sample && json_integer_value(j_bits_per_sample) != self->origformat.bits_per_sample)
                    continue;

                json_t *j_channels     = json_object_get(condition, "channels");
                if (j_channels && json_integer_value(j_channels) != self->origformat.channels)
                    continue;

                json_t *copy = json_deep_copy(value);
                json_object_del(copy, "condition");
                json_array_append_new(signal_path, copy);
                
            } else {
                json_array_append_new(signal_path, json_deep_copy(value));
            }
        }
    } else {
        if (self->origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) {
            switch (self->effective_dsd_mode) {
                case DSD_MODE_NONE: break;

                case DSD_MODE_DOP: {
                    json_t *encapsulate = json_object();
                    json_object_set_new(encapsulate, "type", json_string("dsd_encapsulate"));
                    json_object_set_new(encapsulate, "quality", json_string("lossless"));
                    json_object_set_new(encapsulate, "method", json_string("dop"));
                    json_array_append_new(signal_path, encapsulate);
                } break;

                case DSD_MODE_DCS: {
                    json_t *encapsulate = json_object();
                    json_object_set_new(encapsulate, "type", json_string("dsd_encapsulate"));
                    json_object_set_new(encapsulate, "quality", json_string("lossless"));
                    json_object_set_new(encapsulate, "method", json_string("dcs"));
                    json_array_append_new(signal_path, encapsulate);
                } break;

                default: break;
            }
        }

        json_t *output = json_object();
        if (self->info) {
            json_t *alsa_device = json_object_get(self->info, "alsa_device");
            if (alsa_device) {
                json_object_set(output, "alsa_device", alsa_device);
            }
        }
        json_object_set_new(output, "type", json_string("output"));
        json_object_set_new(output, "method", json_string("alsa"));
        json_object_set_new(output, "quality", json_string("lossless"));
        json_array_append_new(signal_path, output);
    }

    json_object_set_new(message, "signal_path", signal_path);
    RAAT__output_message_listeners_invoke(&self->message_listeners, message);
    json_decref(message);
}

RC__Status 
RAAT__alsa_output_plugin_update_signal_path(RAAT__OutputPlugin *vself, json_t *custom_signal_path) {
    AlsaOutputPlugin *self = (AlsaOutputPlugin*)vself;
    uv_mutex_lock(&self->lock);
    if (self->custom_signal_path) json_decref(self->custom_signal_path);
    self->custom_signal_path = custom_signal_path;
    if (self->custom_signal_path) json_incref(custom_signal_path);
    LOCKED_update_signal_path(self);
    uv_mutex_unlock(&self->lock);
    return RC__STATUS_SUCCESS;
}

void roon_signal_path(uint8_t data)
{
    // JSON data type variables
    json_t *signal_path = json_array();
    json_int_t from_number;
    json_int_t to_number;
    
    // Static signal path variables
    static uint8_t phase = 0;
    static uint8_t upsampling = 0;
    
    static char b_data[] = "R00";
    static uint8_t balance_data_num = 0;
    
    ////////////////////////////////////////////////////////
    // Which type of action initiated call to this function?
    // Assign data to Active feature static.
    ////////////////////////////////////////////////////////
    if(data == STREAMER_PHASE_NORMAL) 
    {
        phase = data;
    }
    else if(data == STREAMER_PHASE_INVERTED) 
    {
        phase = data;
    }
    else if(data == STREAMER_UPSAMPLING_ACTIVE) 
    {
        upsampling = data;
        from_number = 44100;
        to_number = 96000;
    }
    else if(data == STREAMER_UPSAMPLING_INACTIVE) 
    {
        upsampling = data;
    }
    else // must be balance data
    {
        if(data <= 40)  // balance plus data
        {
            // convert balance nums into char arrays
            sprintf(b_data, "R%02d", data);

            // keep balance data for non-zero check
            balance_data_num = data;

        }
        else  // balance minus data -- with upper two bits mask
        {
            // Unmask top two bits of balance minus num 0xC0
            uint8_t balance_minus_data = data & 0x3F;       
            
            // convert balance nums into char arrays
            sprintf(b_data, "L%02d", balance_minus_data);

            // keep track of balance data -- just to know it is non-zero
            balance_data_num = balance_minus_data;
        }
    }
    
    ////////////////////////////////////////////
    // Sometimes Displyed Signal Path Items
    // update Active Signal Display feature Only
    ////////////////////////////////////////////
    
    // phase state
    if(phase == STREAMER_PHASE_INVERTED)
    {
        json_t *encapsulate = json_object();
        json_object_set_new(encapsulate,"quality",json_string("lossless"));
        json_object_set_new(encapsulate,"type",json_string("invert_phase"));
        json_array_append_new(signal_path, encapsulate);
    }

    //////////////////////////////////////
    // Always Displayed Signal Path Items
    //////////////////////////////////////

    // upsampling conversion
    {
        // 44.1K
        {
            json_t *encapsulate = json_object();
            json_object_set_new(encapsulate,"quality",json_string("enhanced"));
            json_object_set_new(encapsulate,"type",json_string("pcm_sample_rate_conversion"));
            json_object_set_new(encapsulate,"from_sample_rate",json_integer(44100));
            json_object_set_new(encapsulate,"to_sample_rate",json_integer(352800));
        
            // create condition object and attach
            json_t *encapsulate2 = json_object();
            json_object_set_new(encapsulate2,"sample_rate",json_integer(44100));
            json_object_set_new(encapsulate,"condition",encapsulate2);
        
            json_array_append_new(signal_path, encapsulate);
        }
        // 48K
        {
            json_t *encapsulate = json_object();
            json_object_set_new(encapsulate,"quality",json_string("enhanced"));
            json_object_set_new(encapsulate,"type",json_string("pcm_sample_rate_conversion"));
            json_object_set_new(encapsulate,"from_sample_rate",json_integer(48000));
            json_object_set_new(encapsulate,"to_sample_rate",json_integer(384000));
        
            // create condition object and attach
            json_t *encapsulate2 = json_object();
            json_object_set_new(encapsulate2,"sample_rate",json_integer(48000));
            json_object_set_new(encapsulate,"condition",encapsulate2);
        
            json_array_append_new(signal_path, encapsulate);
        }
        // 88.2K
        {
            json_t *encapsulate = json_object();
            json_object_set_new(encapsulate,"quality",json_string("enhanced"));
            json_object_set_new(encapsulate,"type",json_string("pcm_sample_rate_conversion"));
            json_object_set_new(encapsulate,"from_sample_rate",json_integer(88200));
            json_object_set_new(encapsulate,"to_sample_rate",json_integer(352800));
        
            // create condition object and attach
            json_t *encapsulate2 = json_object();
            json_object_set_new(encapsulate2,"sample_rate",json_integer(88200));
            json_object_set_new(encapsulate,"condition",encapsulate2);
        
            json_array_append_new(signal_path, encapsulate);
        }        
        // 96K
        {
            json_t *encapsulate = json_object();
            json_object_set_new(encapsulate,"quality",json_string("enhanced"));
            json_object_set_new(encapsulate,"type",json_string("pcm_sample_rate_conversion"));
            json_object_set_new(encapsulate,"from_sample_rate",json_integer(96000));
            json_object_set_new(encapsulate,"to_sample_rate",json_integer(384000));
        
            // create condition object and attach
            json_t *encapsulate2 = json_object();
            json_object_set_new(encapsulate2,"sample_rate",json_integer(96000));
            json_object_set_new(encapsulate,"condition",encapsulate2);
        
            json_array_append_new(signal_path, encapsulate);
        }    
        // 176.4K
        {
            json_t *encapsulate = json_object();
            json_object_set_new(encapsulate,"quality",json_string("enhanced"));
            json_object_set_new(encapsulate,"type",json_string("pcm_sample_rate_conversion"));
            json_object_set_new(encapsulate,"from_sample_rate",json_integer(176400));
            json_object_set_new(encapsulate,"to_sample_rate",json_integer(352800));
        
            // create condition object and attach
            json_t *encapsulate2 = json_object();
            json_object_set_new(encapsulate2,"sample_rate",json_integer(176400));
            json_object_set_new(encapsulate,"condition",encapsulate2);
        
            json_array_append_new(signal_path, encapsulate);
        }     
        // 192K
        {
            json_t *encapsulate = json_object();
            json_object_set_new(encapsulate,"quality",json_string("enhanced"));
            json_object_set_new(encapsulate,"type",json_string("pcm_sample_rate_conversion"));
            json_object_set_new(encapsulate,"from_sample_rate",json_integer(192000));
            json_object_set_new(encapsulate,"to_sample_rate",json_integer(384000));
        
            // create condition object and attach
            json_t *encapsulate2 = json_object();
            json_object_set_new(encapsulate2,"sample_rate",json_integer(192000));
            json_object_set_new(encapsulate,"condition",encapsulate2);
        
            json_array_append_new(signal_path, encapsulate);
        } 
    }
    
    // Balance -- Only displayed if Balance is not neutral 
    if(balance_data_num != 0) 
    {
        json_t *encapsulate = json_object();
        json_object_set_new(encapsulate,"quality",json_string("lossless"));
        json_object_set_new(encapsulate,"type",json_string("balance"));
        json_object_set_new(encapsulate,"value",json_string(b_data));
        json_array_append_new(signal_path, encapsulate);
    }

    // Output signal path
    {
        json_t *encapsulate = json_object();
        json_object_set_new(encapsulate,"quality",json_string("lossless"));
        json_object_set_new(encapsulate,"type",json_string("output"));
        json_object_set_new(encapsulate,"method",json_string("analog"));
        json_array_append_new(signal_path, encapsulate);
    }
    //////////////////////////////
    // Update Signal Path Display
    //////////////////////////////
    RAAT__alsa_output_plugin_update_signal_path(g_output_plugin_ptr, signal_path);
}

static int count(const int *arr) {
    int i = 0;
    while (arr[i]) i++;
    return i;
}

#define COUNT(a) (sizeof(a) / sizeof *(a))

static void print_snd_error(AlsaOutputPlugin *self, int rc, const char *api) {
    if (rc == 0) return;
    RAAT__ERROR("error in %s: %s (%d)", api, snd_strerror(rc), rc);
}

static RC__Status output_get_supported_formats(void *vself, RC__Allocator *alloc, size_t *out_nformats, RAAT__StreamFormat/*?*/ **out_formats) {
    AlsaOutputPlugin *self = (AlsaOutputPlugin*)vself;
    if (self->get_supported_formats_status != RC__STATUS_SUCCESS) return self->get_supported_formats_status;
    RAAT__StreamFormat *formats      = RC__new0(alloc, RAAT__StreamFormat, self->n_supported_formats);
    if (formats == NULL) return RC__STATUS_OUT_OF_MEMORY;
    memcpy(formats, self->supported_formats, self->n_supported_formats * sizeof(RAAT__StreamFormat));

    int i;
    int outidx = 0;
    for (i = 0; i < self->n_supported_formats; i++) {
        if (!self->volume || self->volume->supports_format(self->volume->userdata, &self->supported_formats[i]) == RC__STATUS_SUCCESS) {
            formats[outidx++] = self->supported_formats[i];
        }
    }

    *out_nformats = outidx;
    *out_formats  = formats;

    return RC__STATUS_SUCCESS;
}

static void get_hw_info(AlsaOutputPlugin *self) {
    snd_ctl_t *ctl = NULL;
    snd_pcm_t *pcm = NULL;
    int rc;
    snd_ctl_card_info_t *cardinfo;
    snd_pcm_info_t *pcminfo;

    snd_ctl_card_info_alloca(&cardinfo);
    snd_pcm_info_alloca(&pcminfo);

    rc = snd_pcm_open(&pcm, self->unique_id, SND_PCM_STREAM_PLAYBACK, 0);
    if (0 != rc) { print_snd_error(self, rc, "snd_pcm_open"); goto fail; }

    rc = snd_pcm_info(pcm, pcminfo);
    if (0 != rc) { print_snd_error(self, rc, "snd_pcm_info"); goto fail; }

    char hwid[32];
    sprintf(hwid, "hw:%d", snd_pcm_info_get_card(pcminfo));
    if ((rc = snd_ctl_open(&ctl, hwid, 0)) < 0) {
        goto fail;
    }

    if ((rc = snd_ctl_card_info(ctl, cardinfo)) < 0) {
        goto fail;
    }

    const char *id         = snd_ctl_card_info_get_id(cardinfo);
    const char *name       = snd_ctl_card_info_get_name(cardinfo);
    const char *driver     = snd_ctl_card_info_get_driver(cardinfo);
    const char *longname   = snd_ctl_card_info_get_longname(cardinfo);
    const char *mixername  = snd_ctl_card_info_get_mixername(cardinfo);
    const char *components = snd_ctl_card_info_get_id(cardinfo);

    json_t *device = json_object();
    json_object_set_new(self->info, "alsa_device", device);

    if (id) {
        RAAT__TRACE("pcm card id %s", snd_ctl_card_info_get_id(cardinfo));
        json_object_set_new(device, "id", json_string(id));
    }
    if (name) {
        RAAT__TRACE("pcm card name %s", snd_ctl_card_info_get_name(cardinfo));
        json_object_set_new(device, "name", json_string(name));
    }
    if (longname) {
        RAAT__TRACE("pcm card longname %s", snd_ctl_card_info_get_longname(cardinfo));
        json_object_set_new(device, "longname", json_string(longname));
    }
    if (mixername) {
        RAAT__TRACE("pcm card mixername %s", snd_ctl_card_info_get_mixername(cardinfo));
        json_object_set_new(device, "mixername", json_string(mixername));
    }
    if (components) {
        RAAT__TRACE("pcm card components %s", snd_ctl_card_info_get_components(cardinfo));
        json_object_set_new(device, "components", json_string(components));
    }
    if (driver) {
        RAAT__TRACE("pcm card driver %s", snd_ctl_card_info_get_driver(cardinfo));
        json_object_set_new(device, "driver", json_string(driver));
    }

    json_t *disable_usb_hw_info = json_object_get(self->config, "disable_usb_hw_info");
    if (!json_boolean_value(disable_usb_hw_info)) {
        // try to get usb id
        char buf[1024];
        sprintf(buf, "/proc/asound/card%d/usbid", snd_pcm_info_get_card(pcminfo));
        FILE *fusbid = fopen(buf, "r");
        if (fusbid) {
            int read = fread(buf, 1, sizeof(buf) - 1, fusbid);
            if (read > 0) {
                char *p = buf;
                buf[read] = '\0';
                while (*p) { if (*p == '\n') *p = '\0'; p++; }
                RAAT__TRACE("pcm card usb id %s", buf);
                json_object_set_new(device, "usbid", json_string(buf));
            }
            fclose(fusbid);
        }
    }

fail:
    if (pcm) { snd_pcm_close(pcm); }
    if (ctl)                       snd_ctl_close(ctl);
}

bool supports_format_already(RAAT__StreamFormat format, RAAT__StreamFormat *formats, int nformats) {
    int i;
    for (i = 0; i < nformats; i++) {
        if (format.sample_type     == formats[i].sample_type &&
            format.sample_rate     == formats[i].sample_rate &&
            format.bits_per_sample == formats[i].bits_per_sample &&
            format.channels        == formats[i].channels) {
            return true;
        }
    }
    return false;
}

static void probe_formats(AlsaOutputPlugin *self) {
    RC__Status status = RC__STATUS_SUCCESS;

    int pcmbaserate[]      = { 44100, 48000 };
    int pcmmult[]          = { 1, 2, 4, 8, 16, 32 };
    int channels[]         = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int pcmbitspersample[] = { 16, 24, 32 };
    int dsdmult[]          = { 64, 128, 256, 512, 1024 };

    int channel_i, pcmbaserate_i, pcmmult_i, pcmbitspersample_i;
    size_t nformats = 0;

    int rc;

    size_t              max_nformats = COUNT(channels) * (COUNT(pcmbaserate) * COUNT(pcmmult) * COUNT(pcmbitspersample) + COUNT(dsdmult));
    RAAT__StreamFormat *formats      = RC__new0(self->alloc, RAAT__StreamFormat, max_nformats);
    RC__ASSERT(formats);

    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *hw_params;

    rc = snd_pcm_open(&pcm, self->unique_id, SND_PCM_STREAM_PLAYBACK, 0);
    if (0 != rc) { print_snd_error(self, rc, "snd_pcm_open"); status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail; }

    snd_pcm_hw_params_alloca(&hw_params);
    rc = snd_pcm_hw_params_any(pcm, hw_params);
    if (0 != rc) { print_snd_error(self, rc, "snd_pcm_hw_params_any"); status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED; goto fail; }

    RAAT__TRACE("[alsa] [%s] probing formats", self->unique_id);

    unsigned int ch_min = 0, ch_max = 0;
    if (!snd_pcm_hw_params_get_channels_min(hw_params, &ch_min) && !snd_pcm_hw_params_get_channels_max(hw_params, &ch_max))
        RAAT__TRACE("[alsa] [%s]     device supports channels range %d-%d", self->unique_id, ch_min, ch_max);

    for (channel_i = 0; channel_i < COUNT(channels); channel_i++) {
        int channel       = channels[channel_i];

#if SUPPORTS_NATIVE_DSD
        // native DSD
        int dsdmult_i;
        for (dsdmult_i = 0; dsdmult_i < COUNT(dsdmult); dsdmult_i++) {
            int mult = dsdmult[dsdmult_i];
            int sample_rate = mult*44100;

            bool supports_native        = self->dsd_mode == DSD_MODE_NATIVE        || 
                                          self->dsd_mode == DSD_MODE_NATIVE_OR_DCS || 
                                          self->dsd_mode == DSD_MODE_NATIVE_OR_DOP ||
                                          self->dsd_mode == DSD_MODE_DCS_OR_NATIVE || 
                                          self->dsd_mode == DSD_MODE_DOP_OR_NATIVE;

            if (supports_native) {
                snd_pcm_hw_params_any(pcm, hw_params);
                if ((!snd_pcm_hw_params_set_format(pcm, hw_params, SND_PCM_FORMAT_DSD_U32_LE) || !snd_pcm_hw_params_set_format(pcm, hw_params, SND_PCM_FORMAT_DSD_U32_BE))) {
                    if (!snd_pcm_hw_params_test_rate(pcm, hw_params, sample_rate / 32, 0) && channel <= ch_max) {
                        RAAT__StreamFormat format;
                        format.sample_type     = RAAT__SAMPLE_TYPE_DSD;
                        format.sample_rate     = sample_rate;
                        format.bits_per_sample = 1;
                        format.channels        = channel;
                        RAAT__TRACE("[alsa] [%s] supports DSD format %d/%d/%d (Native)", self->unique_id, 44100*mult, 1, channel);

                        if (!supports_format_already(format, formats, nformats)) {
                            formats[nformats++] = format;
                        }
                    }
                }

                snd_pcm_hw_params_any(pcm, hw_params);
                if ((!snd_pcm_hw_params_set_format(pcm, hw_params, SND_PCM_FORMAT_DSD_U16_LE) || !snd_pcm_hw_params_set_format(pcm, hw_params, SND_PCM_FORMAT_DSD_U16_BE))) {
                    if (!snd_pcm_hw_params_test_rate(pcm, hw_params, sample_rate / 16, 0) && channel <= ch_max) {
                        RAAT__StreamFormat format;
                        format.sample_type     = RAAT__SAMPLE_TYPE_DSD;
                        format.sample_rate     = sample_rate;
                        format.bits_per_sample = 1;
                        format.channels        = channel;
                        RAAT__TRACE("[alsa] [%s] supports DSD format %d/%d/%d (Native)", self->unique_id, 44100*mult, 1, channel);

                        if (!supports_format_already(format, formats, nformats)) {
                            formats[nformats++] = format;
                        }
                    }
                }

                snd_pcm_hw_params_any(pcm, hw_params);
                if (!snd_pcm_hw_params_set_format(pcm, hw_params, SND_PCM_FORMAT_DSD_U8)) {
                    if (!snd_pcm_hw_params_test_rate(pcm, hw_params, sample_rate / 8, 0) && channel <= ch_max) {
                        RAAT__StreamFormat format;
                        format.sample_type     = RAAT__SAMPLE_TYPE_DSD;
                        format.sample_rate     = sample_rate;
                        format.bits_per_sample = 1;
                        format.channels        = channel;
                        RAAT__TRACE("[alsa] [%s] supports DSD format %d/%d/%d (Native)", self->unique_id, 44100*mult, 1, channel);

                        if (!supports_format_already(format, formats, nformats)) {
                            formats[nformats++] = format;
                        }
                    }
                }

            }
        }
#endif

        // PCM + Encapsulated DSD
        for (pcmmult_i = 0; pcmmult_i < COUNT(pcmmult); pcmmult_i++)
        for (pcmbaserate_i = 0; pcmbaserate_i < COUNT(pcmbaserate); pcmbaserate_i++)
        for (pcmbitspersample_i = 0; pcmbitspersample_i < COUNT(pcmbitspersample); pcmbitspersample_i++) {
            snd_pcm_hw_params_any(pcm, hw_params);

            int samplerate    = pcmbaserate[pcmbaserate_i] * pcmmult[pcmmult_i];
            int bitspersample = pcmbitspersample[pcmbitspersample_i];
            bool bitspersample_supported = false;

            switch (bitspersample) {
                case 16:
                    bitspersample_supported = !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_S16_LE) || 
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_S24_LE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_S24_3LE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_S32_LE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_S16_BE) || 
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_S24_BE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_S24_3BE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_S32_BE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_FLOAT_LE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_FLOAT_BE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_FLOAT64_LE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_FLOAT64_BE);
                    break;
                case 24:
                    bitspersample_supported = !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_S32_LE) || 
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_S24_3LE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_S24_3BE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_S24_LE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_S24_BE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_S32_BE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_FLOAT_LE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_FLOAT_BE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_FLOAT64_LE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_FLOAT64_BE);
                    break;
                case 32:
                    bitspersample_supported = !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_S32_LE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_S32_BE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_FLOAT64_LE) ||
                                              !snd_pcm_hw_params_test_format(pcm, hw_params, SND_PCM_FORMAT_FLOAT64_BE);
                    break;
            }

            if (!snd_pcm_hw_params_test_rate(pcm, hw_params, samplerate, 0) && channel <= ch_max && bitspersample_supported) {
                if (self->max_pcm_rate >= samplerate) {
                    formats[nformats].sample_type     = RAAT__SAMPLE_TYPE_PCM;
                    formats[nformats].sample_rate     = samplerate;
                    formats[nformats].bits_per_sample = bitspersample;
                    formats[nformats].channels        = channel;
                    RAAT__TRACE("[alsa] [%s] supports PCM format %d/%d/%d", self->unique_id, samplerate, bitspersample, channel);
                    nformats++;
                }

                int dsdmult_i;
                for (dsdmult_i = 0; dsdmult_i < COUNT(dsdmult); dsdmult_i++) {
                    int mult = dsdmult[dsdmult_i];
                    bool supports_encapsulation = self->dsd_mode == DSD_MODE_DOP           || 
                                                  self->dsd_mode == DSD_MODE_DCS           || 
                                                  self->dsd_mode == DSD_MODE_NATIVE_OR_DCS || 
                                                  self->dsd_mode == DSD_MODE_NATIVE_OR_DOP ||
                                                  self->dsd_mode == DSD_MODE_DCS_OR_NATIVE || 
                                                  self->dsd_mode == DSD_MODE_DOP_OR_NATIVE;

                    if (supports_encapsulation && self->max_dsd_rate >= mult && bitspersample == 24 && samplerate == 44100*mult/16) {
                        formats[nformats].sample_type     = RAAT__SAMPLE_TYPE_DSD;
                        formats[nformats].sample_rate     = 44100*mult;
                        formats[nformats].bits_per_sample = 1;
                        formats[nformats].channels        = channel;
                        if (!supports_format_already(formats[nformats], formats, nformats)) {
                            nformats++;
                        }
                        RAAT__TRACE("[alsa] [%s] supports DSD format %d/%d/%d (Encapsulated)", self->unique_id, 44100*mult, 1, channel);
                    }

                }
            }
        }
    }

    self->supported_formats             = formats;
    self->n_supported_formats           = nformats;
    self->get_supported_formats_status  = status;

fail:
    if (pcm)                          {
        snd_pcm_close(pcm);
    }
    if (status != RC__STATUS_SUCCESS) RC__free(self->alloc,formats);
    self->get_supported_formats_status  = status;
}

typedef struct {
    AlsaOutputPlugin *self;
    int                token;
} AlsaOutputThreadState;

int compute_pcm_hw_buffer_size(RAAT__StreamFormat *format, snd_pcm_format_t hw_format, unsigned int hw_channels, int nsamples) {
    int bytes_per_sample = -1;
    switch (hw_format) {
        case SND_PCM_FORMAT_S16_LE:  bytes_per_sample = 2; break;
        case SND_PCM_FORMAT_S16_BE:  bytes_per_sample = 2; break;
        case SND_PCM_FORMAT_S24_3LE: bytes_per_sample = 3; break;
        case SND_PCM_FORMAT_S24_3BE: bytes_per_sample = 3; break;
        case SND_PCM_FORMAT_S24_LE: bytes_per_sample = 4; break;
        case SND_PCM_FORMAT_S24_BE: bytes_per_sample = 4; break;
        case SND_PCM_FORMAT_S32_LE:  bytes_per_sample = 4; break;
        case SND_PCM_FORMAT_S32_BE:  bytes_per_sample = 4; break;
        case SND_PCM_FORMAT_FLOAT_LE:  bytes_per_sample = 4; break;
        case SND_PCM_FORMAT_FLOAT_BE:  bytes_per_sample = 4; break;
        case SND_PCM_FORMAT_FLOAT64_LE:  bytes_per_sample = 8; break;
        case SND_PCM_FORMAT_FLOAT64_BE:  bytes_per_sample = 8; break;
#if SUPPORTS_NATIVE_DSD
        case SND_PCM_FORMAT_DSD_U8:  bytes_per_sample = 1; break;
        case SND_PCM_FORMAT_DSD_U16_LE:  bytes_per_sample = 2; break;
        case SND_PCM_FORMAT_DSD_U16_BE:  bytes_per_sample = 2; break;
        case SND_PCM_FORMAT_DSD_U32_LE:  bytes_per_sample = 4; break;
        case SND_PCM_FORMAT_DSD_U32_BE:  bytes_per_sample = 4; break;
#endif
        default: RC__ASSERT(false); break;
    }
    return bytes_per_sample * hw_channels * nsamples;
}

static double flip_double(double i) {
    void *ii = &i;
    unsigned long long u = *(unsigned long long*)ii;
    unsigned long long o = ((u << 56) & 0xff00000000000000) |
                           ((u << 48) & 0x00ff000000000000) |
                           ((u << 40) & 0x0000ff0000000000) |
                           ((u << 32) & 0x000000ff00000000) |
                           ((u >> 32) & 0x00000000ff000000) |
                           ((u >> 40) & 0x0000000000ff0000) |
                           ((u >> 48) & 0x000000000000ff00) |
                           ((u >> 56) & 0x00000000000000ff);
    void *oo = &o;
    return *(double*)oo;
}

static float flip_float(float i) {
    void *ii = &i;
    unsigned u = *(unsigned*)ii;
    unsigned o = ((u << 24) & 0xff000000) |
                 ((u <<  8) & 0x00ff0000) |
                 ((u >>  8) & 0x0000ff00) |
                 ((u >> 24) & 0x000000ff);
    void *oo = &o;
    return *(float*)oo;
}

/*
 * Repack samples for the hardware
 *
 * Note: not all of these cases are thoroughly QA'd. Less commonly used cases might have bugs. If you copy-paste this code, please be careful
 */
void pack_hw_samples(AlsaOutputPlugin *self, uint8_t *input_buf, uint8_t *output_buf, int nsamples) {
    RAAT__StreamFormat *format = &self->pcmformat;
    int samplevalues = nsamples * format->channels;
    int input_stride = format->bits_per_sample / 8;

    int sampleidx       = 0;
    int output_channels = self->hw_channels;
    int zero_channels   = self->hw_channels - format->channels;

    switch (self->hw_format) {
        case SND_PCM_FORMAT_S16_LE: {
            for (input_buf = input_buf; samplevalues; samplevalues--, input_buf += input_stride, output_buf += 2) {
                switch (format->bits_per_sample) {
                    case 16:
                        output_buf[0] = input_buf[0];
                        output_buf[1] = input_buf[1];
                        break;
                    case 24:
                        output_buf[0] = input_buf[1];
                        output_buf[1] = input_buf[2];
                        break;
                    case 32:
                        output_buf[0] = input_buf[2];
                        output_buf[1] = input_buf[3];
                        break;
                }
                if (++sampleidx % format->channels == 0) output_buf += 2 * zero_channels;
            }
        } break;

        case SND_PCM_FORMAT_S16_BE: {
            for (input_buf = input_buf; samplevalues; samplevalues--, input_buf += input_stride, output_buf += 2) {
                switch (format->bits_per_sample) {
                    case 16:
                        output_buf[1] = input_buf[0];
                        output_buf[0] = input_buf[1];
                        break;
                    case 24:
                        output_buf[1] = input_buf[1];
                        output_buf[0] = input_buf[2];
                        break;
                    case 32:
                        output_buf[1] = input_buf[2];
                        output_buf[0] = input_buf[3];
                        break;
                }
                if (++sampleidx % format->channels == 0) output_buf += 2 * zero_channels;
            }
        } break;

        case SND_PCM_FORMAT_S24_LE: {
            for (input_buf = input_buf ; samplevalues; samplevalues--, input_buf += input_stride, output_buf += 4) {
                switch (format->bits_per_sample) {
                    case 16:
                        output_buf[0] = 0;
                        output_buf[1] = input_buf[0];
                        output_buf[2] = input_buf[1];
                        output_buf[3] = 0;
                        break;
                    case 24:
                        output_buf[0] = input_buf[0];
                        output_buf[1] = input_buf[1];
                        output_buf[2] = input_buf[2];
                        output_buf[3] = 0;
                        break;
                }
                if (++sampleidx % format->channels == 0) output_buf += 4 * zero_channels;
            }
        } break;

        case SND_PCM_FORMAT_S24_BE: {
            for (input_buf = input_buf ; samplevalues; samplevalues--, input_buf += input_stride, output_buf += 4) {
                switch (format->bits_per_sample) {
                    case 16:
                        output_buf[3] = 0;
                        output_buf[2] = input_buf[0];
                        output_buf[1] = input_buf[1];
                        output_buf[0] = 0;
                        break;
                    case 24:
                        output_buf[3] = input_buf[0];
                        output_buf[2] = input_buf[1];
                        output_buf[1] = input_buf[2];
                        output_buf[0] = 0;
                        break;
                }
                if (++sampleidx % format->channels == 0) output_buf += 4 * zero_channels;
            }
        } break;

        case SND_PCM_FORMAT_S24_3LE: {
            for (input_buf = input_buf ; samplevalues; samplevalues--, input_buf += input_stride, output_buf += 3) {
                switch (format->bits_per_sample) {
                    case 16:
                        output_buf[0] = 0;
                        output_buf[1] = input_buf[0];
                        output_buf[2] = input_buf[1];
                        break;
                    case 24:
                        output_buf[0] = input_buf[0];
                        output_buf[1] = input_buf[1];
                        output_buf[2] = input_buf[2];
                        break;
                }
                if (++sampleidx % format->channels == 0) output_buf += 3 * zero_channels;
            }
        } break;

        case SND_PCM_FORMAT_S24_3BE: {
            for (input_buf = input_buf ; samplevalues; samplevalues--, input_buf += input_stride, output_buf += 3) {
                switch (format->bits_per_sample) {
                    case 16:
                        output_buf[2] = 0;
                        output_buf[1] = input_buf[0];
                        output_buf[0] = input_buf[1];
                        break;
                    case 24:
                        output_buf[2] = input_buf[0];
                        output_buf[1] = input_buf[1];
                        output_buf[0] = input_buf[2];
                        break;
                }
                if (++sampleidx % format->channels == 0) output_buf += 3 * zero_channels;
            }
        } break;

        case SND_PCM_FORMAT_S32_LE: {
            for (input_buf = input_buf; samplevalues; samplevalues--, input_buf += input_stride, output_buf += 4) {
                switch (format->bits_per_sample) {
                    case 16:
                        output_buf[0] = 0;
                        output_buf[1] = 0;
                        output_buf[2] = input_buf[0];
                        output_buf[3] = input_buf[1];
                        break;
                    case 24:
                        output_buf[0] = 0;
                        output_buf[1] = input_buf[0];
                        output_buf[2] = input_buf[1];
                        output_buf[3] = input_buf[2];
                        break;
                    case 32:
                        output_buf[0] = input_buf[0];
                        output_buf[1] = input_buf[1];
                        output_buf[2] = input_buf[2];
                        output_buf[3] = input_buf[3];
                        break;
                }
                if (++sampleidx % format->channels == 0) output_buf += 4 * zero_channels;
            }
        } break;

        case SND_PCM_FORMAT_S32_BE: {
            for (input_buf = input_buf; samplevalues; samplevalues--, input_buf += input_stride, output_buf += 4) {
                switch (format->bits_per_sample) {
                    case 16:
                        output_buf[3] = 0;
                        output_buf[2] = 0;
                        output_buf[1] = input_buf[0];
                        output_buf[0] = input_buf[1];
                        break;
                    case 24:
                        output_buf[3] = 0;
                        output_buf[2] = input_buf[0];
                        output_buf[1] = input_buf[1];
                        output_buf[0] = input_buf[2];
                    case 32:
                        output_buf[0] = input_buf[3];
                        output_buf[1] = input_buf[2];
                        output_buf[2] = input_buf[1];
                        output_buf[3] = input_buf[0];
                        break;
                }
                if (++sampleidx % format->channels == 0) output_buf += 4 * zero_channels;
            }
        } break;

        case SND_PCM_FORMAT_FLOAT_LE: {
            for (input_buf = input_buf; samplevalues; samplevalues--, input_buf += input_stride, output_buf += 4) {
                int sample = -1;
                switch (format->bits_per_sample) {
                    case 16:
                        sample = (int)input_buf[0] << 16 | ((int)input_buf[1] << 24);
                        break;
                    case 24:
                        sample = (int)input_buf[0] << 8 | ((int)input_buf[1] << 16) | ((int)input_buf[2] << 24);
                        break;
                }
                *((float*)output_buf) = (float)sample / (float)INT_MAX;
                if (++sampleidx % format->channels == 0) output_buf += 4 * zero_channels;
            }
        } break;

        case SND_PCM_FORMAT_FLOAT_BE: {
            for (input_buf = input_buf; samplevalues; samplevalues--, input_buf += input_stride, output_buf += 4) {
                int sample = -1;
                switch (format->bits_per_sample) {
                    case 16:
                        sample = (int)input_buf[0] << 16 | ((int)input_buf[1] << 24);
                        break;
                    case 24:
                        sample = (int)input_buf[0] << 8 | ((int)input_buf[1] << 16) | ((int)input_buf[2] << 8);
                        break;
                }
                *((float*)output_buf) = flip_float((float)sample / (float)INT_MAX);
                if (++sampleidx % format->channels == 0) output_buf += 4 * zero_channels;
            }
        } break;

        case SND_PCM_FORMAT_FLOAT64_LE: {
            for (input_buf = input_buf; samplevalues; samplevalues--, input_buf += input_stride, output_buf += 8) {
                int sample = -1;
                switch (format->bits_per_sample) {
                    case 16:
                        sample = (int)input_buf[0] << 16 | ((int)input_buf[1] << 24);
                        break;
                    case 24:
                        sample = (int)input_buf[0] << 8 | ((int)input_buf[1] << 16) | ((int)input_buf[2] << 8);
                        break;
                }
                *((double*)output_buf) = (double)sample / (double)INT_MAX;
                if (++sampleidx % format->channels == 0) output_buf += 8 * zero_channels;
            }
        } break;

        case SND_PCM_FORMAT_FLOAT64_BE: {
            for (input_buf = input_buf; samplevalues; samplevalues--, input_buf += input_stride, output_buf += 8) {
                int sample = -1;
                switch (format->bits_per_sample) {
                    case 16:
                        sample = (int)input_buf[0] << 16 | ((int)input_buf[1] << 24);
                        break;
                    case 24:
                        sample = (int)input_buf[0] << 8 | ((int)input_buf[1] << 16) | ((int)input_buf[2] << 8);
                        break;
                }
                *((double*)output_buf) = flip_double((double)sample / (double)INT_MAX);
                if (++sampleidx % format->channels == 0) output_buf += 8 * zero_channels;
            }
        } break;

#if SUPPORTS_NATIVE_DSD
        case SND_PCM_FORMAT_DSD_U8: {
            if (format->channels == output_channels) {
                memcpy(output_buf, input_buf, nsamples * format->channels);
            } else {
                int i,chs = format->channels;
                for (i = 0; i < nsamples; i++) {
                    off_t in_s_off  = i*chs;
                    off_t out_s_off = i*output_channels;
                    int ch;
                    for (ch = 0; ch < chs; ch++) {
                        output_buf[out_s_off+ch] = input_buf[in_s_off+ch];
                    }
                }
            }
        } break;

        case SND_PCM_FORMAT_DSD_U16_LE: {
            int i,chs = format->channels;
            for (i = 0; i < nsamples; i++) {
                off_t in_s_off = i*2*chs;
                off_t out_s_off = i*2*output_channels;
                int ch;
                for (ch = 0; ch < chs; ch++) {
                    output_buf[out_s_off+ch*2+0] = input_buf[in_s_off+1*chs+ch];
                    output_buf[out_s_off+ch*2+1] = input_buf[in_s_off+0*chs+ch];
                }
            }
        } break;

        case SND_PCM_FORMAT_DSD_U16_BE: {
            int i,chs = format->channels;
            for (i = 0; i < nsamples; i++) {
                off_t in_s_off = i*2*chs;
                off_t out_s_off = i*2*output_channels;
                int ch;
                for (ch = 0; ch < chs; ch++) {
                    output_buf[out_s_off+ch*2+0] = input_buf[in_s_off+0*chs+ch];
                    output_buf[out_s_off+ch*2+1] = input_buf[in_s_off+1*chs+ch];
                }
            }
        } break;

        case SND_PCM_FORMAT_DSD_U32_LE: {
            int i,chs = format->channels;
            for (i = 0; i < nsamples; i++) {
                off_t in_s_off = i*4*chs;
                off_t out_s_off = i*4*output_channels;
                int ch;
                for (ch = 0; ch < chs; ch++) {
                    output_buf[out_s_off+ch*4+0] = input_buf[in_s_off+3*chs+ch];
                    output_buf[out_s_off+ch*4+1] = input_buf[in_s_off+2*chs+ch];
                    output_buf[out_s_off+ch*4+2] = input_buf[in_s_off+1*chs+ch];
                    output_buf[out_s_off+ch*4+3] = input_buf[in_s_off+0*chs+ch];
                }
            }
        } break;

        case SND_PCM_FORMAT_DSD_U32_BE: {
            int i,chs = format->channels;
            for (i = 0; i < nsamples; i++) {
                off_t in_s_off = i*4*chs;
                off_t out_s_off = i*4*output_channels;
                int ch;
                for (ch = 0; ch < chs; ch++) {
                    output_buf[out_s_off+ch*4+0] = input_buf[in_s_off+0*chs+ch];
                    output_buf[out_s_off+ch*4+1] = input_buf[in_s_off+1*chs+ch];
                    output_buf[out_s_off+ch*4+2] = input_buf[in_s_off+2*chs+ch];
                    output_buf[out_s_off+ch*4+3] = input_buf[in_s_off+3*chs+ch];
                }
            }
        } break;
#endif

        default: RC__ASSERT(false);
    }

    return;
}

static void LOCKED__on_teardown(AlsaOutputPlugin *self) {
    usleep(100000);     // provide 100ms of space. Some XMOS chipsets fail if we switch formats too frequently on recent Linux kernels
    if (self->volume) {
        self->volume->teardown(self->volume->userdata);
    }
}

static RC__Status output_force_teardown(void *vself, json_t *reason) {
    AlsaOutputPlugin *self = (AlsaOutputPlugin*)vself;

    RC__Status status = RC__STATUS_SUCCESS;
    int        token = RAAT__OUTPUT_TOKEN_INVALID;

    RAAT__OutputLostCallback  old_cb_lost          = NULL;
    void                     *old_cb_lost_userdata = NULL;
    bool                      needs_join           = false;

    uv_mutex_lock(&self->lock);
retry:
    self->current_token = token = ++self->next_token;

    if (self->state != IDLE) {
        RAAT__TRACE("[alsa] [%s] kicking off old output in force teardown", self->unique_id);
        needs_join             = true;
        self->state            = IDLE;
        old_cb_lost            = self->cb_lost;
        old_cb_lost_userdata   = self->cb_lost_userdata;
        self->cb_lost          = NULL;
        self->cb_lost_userdata = NULL;
        LOCKED__on_teardown(self);
    }
    uv_mutex_unlock(&self->lock);

    if (needs_join) {
        RAAT__TRACE("[alsa] [%s] joining thread in force teardown", self->unique_id);
        uv_thread_join(&self->tid);
    }

    if (old_cb_lost != NULL) {
        old_cb_lost(old_cb_lost_userdata, reason);
    }

    uv_mutex_lock(&self->lock);
    if (self->current_token != token) {
        RAAT__TRACE("[alsa] [%s] force teardown required a retry", self->unique_id);
        goto retry;
    }

    // clean up leftover stream 
    if (self->stream != NULL) {
        RAAT__stream_decref(self->stream);
        self->stream = NULL;
    }

    // clean up leftover open PCM
    if (self->pcm) {
        snd_pcm_close(self->pcm);
        self->pcm = NULL;
    }

    uv_mutex_unlock(&self->lock);

    return status;
}

static void alsa_output_thread(void *vself) {
    AlsaOutputThreadState *threadstate = vself;
    AlsaOutputPlugin      *self        = threadstate->self;

    RAAT__Stream       *stream = NULL;
    RAAT__StreamFormat  origformat;
    RAAT__StreamFormat  pcmformat;
    bool                finished = false;
    uint8_t            *streambuf       = NULL;
    uint8_t            *hwbuf     = NULL;
    uint8_t            *dsdbuf    = NULL;
    uint8_t            *expandbuf = NULL;
    snd_pcm_t          *pcm;

    struct sched_param sparams = {0,};
    sparams.sched_priority = 95;
    int rc = sched_setscheduler(0, SCHED_RR, &sparams);
    if (rc == -1) {
        RAAT__WARNING("sched_setscheduler failed: %s", strerror(errno));
    } else {
        RAAT__TRACE("sched_setscheduler succeeded");
    }

    uv_mutex_lock(&self->lock);
    if (threadstate->token == self->current_token) {
        pcmformat  = self->pcmformat;
        origformat = self->origformat;
        pcm    = self->pcm;
    } else {
        finished = true;
    }
    uv_mutex_unlock(&self->lock);
    if (finished) return;

    int64_t streamsample = 0;

    int origformat_samples_per_buf = 0;

    if (origformat.sample_type == RAAT__SAMPLE_TYPE_PCM) {
        origformat_samples_per_buf = self->pcm_samples_per_buf;
    } else if (origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) {
        if (self->effective_dsd_mode == DSD_MODE_DOP || self->effective_dsd_mode == DSD_MODE_DCS) {
            origformat_samples_per_buf = self->pcm_samples_per_buf * 16;
#if SUPPORTS_NATIVE_DSD
        } else if (self->effective_dsd_mode == DSD_MODE_NATIVE) {
            switch (self->hw_format) {
                case SND_PCM_FORMAT_DSD_U8:     origformat_samples_per_buf = self->pcm_samples_per_buf * 8;  break;
                case SND_PCM_FORMAT_DSD_U16_LE: origformat_samples_per_buf = self->pcm_samples_per_buf * 16; break;
                case SND_PCM_FORMAT_DSD_U16_BE: origformat_samples_per_buf = self->pcm_samples_per_buf * 16; break;
                case SND_PCM_FORMAT_DSD_U32_LE: origformat_samples_per_buf = self->pcm_samples_per_buf * 32; break;
                case SND_PCM_FORMAT_DSD_U32_BE: origformat_samples_per_buf = self->pcm_samples_per_buf * 32; break;
                default: break;
            }
#endif
        }
    } else {
        RC__ASSERT(false);
    }

    int     ns_per_buf      = (int)RAAT__stream_format_samples_to_ns(&pcmformat, self->pcm_samples_per_buf);
    int     bytes_per_buf   = RAAT__stream_format_compute_buffer_size(&origformat, origformat_samples_per_buf);
    int     bytes_per_hwbuf = compute_pcm_hw_buffer_size(&pcmformat, self->hw_format, self->hw_channels, self->pcm_samples_per_buf);

    RAAT__TRACE("samples per buf %d ns per buf %d samplerate %d", self->pcm_samples_per_buf, ns_per_buf, pcmformat.sample_rate);
    RAAT__TRACE("%d samples per buf, %d bytes per buf, %d bytes per hwbuf", self->pcm_samples_per_buf, bytes_per_buf, bytes_per_hwbuf);

    streambuf    = RC__alloc(RC__ALLOCATOR_DEFAULT, bytes_per_buf);
    hwbuf        = RC__alloc(RC__ALLOCATOR_DEFAULT, bytes_per_hwbuf);

    if (origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) {
        // Both DoP and Dcs packing requires 3 bytes of 24bit PCM data for every 2 bytes of DSD data
        dsdbuf = RC__alloc(RC__ALLOCATOR_DEFAULT, bytes_per_buf * 3 / 2);
    }

    if (origformat.sample_type == RAAT__SAMPLE_TYPE_PCM && pcmformat.bits_per_sample > origformat.bits_per_sample) {
        expandbuf = RC__alloc(RC__ALLOCATOR_DEFAULT, RAAT__stream_format_compute_buffer_size(&pcmformat, self->pcm_samples_per_buf));
    }

    RAAT__TRACE("[output/alsa] [%s] waiting for device to be ready", self->unique_id);
    rc = snd_pcm_wait(pcm, 2000);
    if (rc < 0) { print_snd_error(self, rc, "snd_pcm_wait"); return; }
    RAAT__TRACE("[output/alsa] [%s] device is ready", self->unique_id);

    // retry up to 3x at 100ms intervals because some DACs briefly disappear during param setting
    rc = snd_pcm_prepare(pcm);
    if (rc != 0) { usleep(100000); rc = snd_pcm_prepare(pcm); }
    if (rc != 0) { usleep(100000); rc = snd_pcm_prepare(pcm); }
    if (rc != 0) { usleep(100000); rc = snd_pcm_prepare(pcm); }
    if (0 != rc) { print_snd_error(self, rc, "snd_pcm_prepare"); return; }

    while (true) {
        int64_t now_raw_ns = RC__now_ns();
        int origformat_samples_to_zerofill = 0;

        snd_pcm_sframes_t delay_pcm_samples = 0;
        snd_pcm_delay(pcm, &delay_pcm_samples);
        int64_t delay_ns = RAAT__stream_format_samples_to_ns(&pcmformat, delay_pcm_samples + self->volume_delay) + self->sync_offset_ns;

        uv_mutex_lock(&self->lock);
        self->last_delay_ns = delay_ns;
        self->last_monotonic_sample_systime         =  now_raw_ns + delay_ns;
        self->last_monotonic_sample                 += origformat_samples_per_buf;

        int64_t now = RAAT__stream_format_samples_to_ns(&origformat, self->last_monotonic_sample);

        int correction_samples = 0; 

        //RAAT__TRACE("[alsa] at top of loop, pcm state: %s tokens %d %d", snd_pcm_state_name(snd_pcm_state(pcm)), threadstate->token, self->current_token);
        if (threadstate->token == self->current_token) {
            if (stream != NULL) {
                RAAT__stream_decref(stream);
                stream = NULL;
            }

            if (self->new_stream && self->stream) {
                int64_t start_time = self->start_time;
                if (now + ns_per_buf > start_time) {
                    RAAT__TRACE("starting playback: now (%lldns) + ns_per_buf(%dns) = %lldns > %lldns streamsample=%lld", now, ns_per_buf, now+ns_per_buf, start_time, self->start_streamsample);
                    self->new_stream    = false;
                    stream              = self->stream;
                    streamsample        = self->start_streamsample;

                    // streamsample must fall on an 8-bit boundary or bad things will happen
                    if (origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) 
                        streamsample -= streamsample % 8;

                    if (now < start_time) {
                        int ns_to_fill      = start_time - now;
                        origformat_samples_to_zerofill = RAAT__stream_format_ns_to_samples(&origformat, ns_to_fill);
                    } if (now > start_time) {
                        streamsample += RAAT__stream_format_ns_to_samples(&origformat, now - start_time);
                    }
                } else {
                    RAAT__TRACE("waiting for start time...");
                }
            } else {
                stream              = self->stream;
                origformat_samples_to_zerofill = 0;
            }

            correction_samples = RAAT__drift_correction_compute_correction(&self->drift_correction, origformat_samples_per_buf);

            if (stream != NULL) RAAT__stream_incref(stream);
        } else {
            finished = true;
        }
        uv_mutex_unlock(&self->lock);

        if (finished) break;

        if (stream == NULL || self->resync_delay_remaining_ns > 0) {
            origformat_samples_to_zerofill = origformat_samples_per_buf;
        }

        //
        // Zero-fill bytes at the start of the buf, if needed.
        //
        // This happens in two situations:
        //
        // - We are playing the first packet of a stream and it begins in the middle of an hardware buffer
        // - We have no stream active, and need to play silence
        //
        int bytes_to_zerofill = RAAT__stream_format_compute_buffer_size(&origformat, origformat_samples_to_zerofill);
        int i;
        for (i = 0; i < bytes_to_zerofill; i++) {
            if (origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) streambuf[i] = 0x69;
            else                                                 streambuf[i] = 0x00;
        }

        if (stream != NULL && self->resync_delay_remaining_ns <= 0) {
            //if (correction_samples != 0) RAAT__TRACE("[alsa] applying %d sample correction", correction_samples);
            RAAT__read_stream_with_drift_correction(stream, streamsample, streambuf + bytes_to_zerofill, origformat_samples_per_buf - origformat_samples_to_zerofill, correction_samples, NULL, NULL);
            streamsample += origformat_samples_per_buf - origformat_samples_to_zerofill + correction_samples;
        }

        if (self->resync_delay_remaining_ns > 0) {
            self->resync_delay_remaining_ns -= ns_per_buf;
            if (self->resync_delay_remaining_ns <= 0) {
                self->setup_cb(self->setup_cb_userdata, RC__STATUS_SUCCESS, threadstate->token);
            }
        }

        uint8_t *buf = streambuf;

        if (origformat.sample_type == RAAT__SAMPLE_TYPE_PCM && pcmformat.bits_per_sample > origformat.bits_per_sample) {
            // repack PCM into wider PCM. This case is most relevant when performing digital volume adjustments. We may have
            // opened the audio device at a wider bits-per-sample than the stream, so that we can preserve as much data as possible after
            // volume attenuation
            RAAT__stream_format_repack(&origformat, buf, &pcmformat, expandbuf, origformat_samples_per_buf);
            buf = expandbuf;
        }

        if (self->volume) {
            self->volume->process(self->volume->userdata, 0, buf, origformat_samples_per_buf);
        }

        // DSD must be repacked
        if (origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) {
            if (self->effective_dsd_mode == DSD_MODE_DOP) {
                RAAT__pack_dop_samples(&origformat, buf, dsdbuf, origformat_samples_per_buf, &self->dsd_dop_flipper);
                buf = dsdbuf;
            } else if (self->effective_dsd_mode == DSD_MODE_DCS) {
                RAAT__pack_dcs_samples(&origformat, buf, dsdbuf, origformat_samples_per_buf);
                buf = dsdbuf;
            } else if (self->effective_dsd_mode == DSD_MODE_NATIVE) {
                /* nothing to do. pack_hw_samples will handle the repack */
            } else {
                RC__ASSERT(false);
            }
        }

        pack_hw_samples(self, buf, hwbuf, self->pcm_samples_per_buf);

        //RAAT__TRACE("before write ms %f", RC__now_us() / 1000.0);
        snd_pcm_sframes_t iprc = snd_pcm_writei(pcm, hwbuf, self->pcm_samples_per_buf);
        //RAAT__TRACE("after  write ms %f", RC__now_us() / 1000.0);
        if (iprc < 0) {
            rc = snd_pcm_recover(pcm, iprc, 1);
            if (0 != rc) { print_snd_error(self, rc, "snd_pcm_recover"); }
            if (rc == -19) {
                json_t *reason = json_object();
                json_object_set_new(reason, "reason", json_string("device_not_available"));
                output_force_teardown(self, reason);
                json_decref(reason);
            }
            //RAAT__WARNING("[ALSA] [%s] underrun, rc={%d}", self->unique_id, (int)iprc);
        }
    }

    if (stream != NULL) {
        RAAT__stream_decref(stream);
        stream = NULL;
    }

    RC__free(RC__ALLOCATOR_DEFAULT, streambuf);
    RC__free(RC__ALLOCATOR_DEFAULT, hwbuf);
    RC__free(RC__ALLOCATOR_DEFAULT, dsdbuf);
    RC__free(RC__ALLOCATOR_DEFAULT, expandbuf);
    RC__free(RC__ALLOCATOR_DEFAULT, threadstate);
}

static RC__Status LOCKED_force_max_volume(AlsaOutputPlugin *self) {
    RC__Status status = RC__STATUS_SUCCESS;
    int rc = 0;

    const char *raw_unique_id = self->unique_id;

    // remove device spec
    char unique_id[1024];
    strcpy(unique_id, raw_unique_id);
    char *comma = strchr(unique_id, ',');
    if (comma) *comma = '\0';

    snd_mixer_t *mixer = NULL;

    rc = snd_mixer_open(&mixer, 0);
    if (0 != rc) { print_snd_error(self, rc, "snd_mixer_open"); status = RAAT__VOLUME_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail; }

    rc = snd_mixer_attach(mixer, unique_id);
    if (0 != rc) { print_snd_error(self, rc, "snd_mixer_attach"); status = RAAT__VOLUME_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail; }

    rc = snd_mixer_selem_register(mixer, NULL, NULL);
    if (0 != rc) { print_snd_error(self, rc, "snd_mixer_selem_register"); status = RAAT__VOLUME_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail; }

    rc = snd_mixer_load(mixer);
    if (0 != rc) { print_snd_error(self, rc, "snd_mixer_load"); status = RAAT__VOLUME_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail; }

    snd_mixer_selem_id_t *sid;
    snd_mixer_selem_id_alloca(&sid);

    //RAAT__TRACE("[output/alsa] searching for volume control element");
    snd_mixer_elem_t *elem = NULL, *good_elem = NULL, *good_mute_elem = NULL;

    for (elem = snd_mixer_first_elem(mixer); elem; elem = snd_mixer_elem_next(elem)) {
        //RAAT__TRACE("[output/alsa]     card has element %d, %s", snd_mixer_selem_get_index(elem), snd_mixer_selem_get_name(elem));

        if (!snd_mixer_selem_has_playback_volume(elem)) {
            //RAAT__TRACE("[output/alsa]         (skipping: element doesn't support playback volume)");
            continue;
        }

        if (good_elem) {
            //RAAT__TRACE("[output/alsa]         (skipping: already chose element)");
            continue;
        }

        if (!good_elem) {
            //RAAT__TRACE("[output/alsa]         (using this mixer element)");
            good_elem = elem;
        }

        good_elem = elem;
    }

    //RAAT__TRACE("[output/alsa] searching for mute element");
    snd_mixer_elem_t *mute_elem;

    for (mute_elem = snd_mixer_first_elem(mixer); mute_elem; mute_elem = snd_mixer_elem_next(mute_elem)) {
        //RAAT__TRACE("[output/alsa]     card has element %d, %s", snd_mixer_selem_get_index(mute_elem), snd_mixer_selem_get_name(mute_elem));

        if (!snd_mixer_selem_has_playback_switch(mute_elem)) {
            //RAAT__TRACE("[output/alsa]         (skipping: element doesn't support playback switch)");
            continue;
        }

        if (good_mute_elem) {
            //RAAT__TRACE("[output/alsa]         (skipping: already chose element)");
            continue;
        }

        if (!good_mute_elem) {
            //RAAT__TRACE("[output/alsa]         (using this mixer element)");
            good_mute_elem = mute_elem;
        }

        good_mute_elem = mute_elem;
    }

    if (good_elem) {
        long min, max;
        rc = snd_mixer_selem_get_playback_volume_range(good_elem, &min, &max);
        if (!rc) {
            snd_mixer_selem_set_playback_volume_all(good_elem, max);
        }
    }

    if (good_mute_elem) {
        snd_mixer_selem_set_playback_switch_all(good_mute_elem, (int)1);
    }

fail:
    if (mixer) snd_mixer_close(mixer);
    return status;
}

static void output_setup(void *vself, RAAT__StreamFormat *format, RAAT__OutputSetupCallback cb_setup, void *cb_setup_userdata, RAAT__OutputLostCallback cb_lost, void *cb_lost_userdata) {
    AlsaOutputPlugin *self = (AlsaOutputPlugin*)vself;
    RC__Status status = RC__STATUS_SUCCESS;
    int        token = RAAT__OUTPUT_TOKEN_INVALID;

    RAAT__OutputLostCallback  old_cb_lost          = NULL;
    void                     *old_cb_lost_userdata = NULL;
    bool                      needs_join           = false;
    int rc;

    char formatstr[RAAT__STREAM_FORMAT_MAX_STRLEN];
    RAAT__stream_format_to_string(format, formatstr);

    uv_mutex_lock(&self->lock);
    // before we do anything else, kick off anyone currently using the device
    self->current_token = token = ++self->next_token;

    if (self->state != IDLE) {
        RAAT__TRACE("[alsa] [%s] kicking off old output", self->unique_id);
        needs_join             = true;
        self->state            = IDLE;
        old_cb_lost            = self->cb_lost;
        old_cb_lost_userdata   = self->cb_lost_userdata;
        self->cb_lost          = NULL;
        self->cb_lost_userdata = NULL;
        LOCKED__on_teardown(self);
    }
    uv_mutex_unlock(&self->lock);

    if (needs_join) {
        RAAT__TRACE("[alsa] [%s] joining thread", self->unique_id);
        uv_thread_join(&self->tid);
    }

    if (old_cb_lost != NULL) {
        json_t *reason = json_object();
        json_object_set_new(reason, "reason", json_string("setup"));
        old_cb_lost(old_cb_lost_userdata, reason);
        json_decref(reason);
    }

    uv_mutex_lock(&self->lock);

    if (self->current_token != token) {
        RAAT__TRACE("alsa output setup: lost the race");
        status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED;
        goto fail;
    }

    // clean up leftover stream 
    if (self->stream != NULL) {
        RAAT__stream_decref(self->stream);
        self->stream = NULL;
    }

    // clean up leftover open PCM
    if (self->pcm) {
        snd_pcm_close(self->pcm);
        self->pcm = NULL;
    }

    RAAT__TRACE("alsa output setup: format is %s", formatstr);

    RAAT__StreamFormat *formats = NULL;
    size_t              nformats;
    RAAT__StreamFormat pcmformat;
    status = output_get_supported_formats(self, RC__ALLOCATOR_DEFAULT, &nformats, &formats);
    if (RC__STATUS_IS_SUCCESS(status)) {
        bool found = false;
        size_t i;
                    
        for (i = 0; i < nformats; i++) {
            if (RAAT__stream_format_equals(format, &formats[i])) {
                found = true; break;
            }
        }
        RAAT__TRACE("opening [%s] %d/%d/%d", self->unique_id, format->sample_rate, format->bits_per_sample, format->channels);

        if (format->sample_subtype == RAAT__SAMPLE_SUBTYPE_MQA) {
            RAAT__TRACE("[ALSA] [%s] Audio content is MQA, ORFS=%d", self->unique_id, format->mqa_original_sample_rate);
        } else if (format->sample_subtype == RAAT__SAMPLE_SUBTYPE_MQA_CORE) {
            RAAT__TRACE("[ALSA] [%s] Audio content is MQA_CORE, ORFS=%d", self->unique_id, format->mqa_original_sample_rate);
        }

        if (found) {
            rc = snd_pcm_open(&self->pcm, self->unique_id, SND_PCM_STREAM_PLAYBACK, 0);
            if (0 != rc) { print_snd_error(self, rc, "snd_pcm_open"); status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail; }

            snd_pcm_hw_params_t *hw_params;
            snd_pcm_hw_params_alloca(&hw_params);

            rc = snd_pcm_hw_params_any(self->pcm, hw_params);
            if (0 != rc) { print_snd_error(self, rc, "snd_pcm_hw_params_any"); status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED; goto fail; }

            rc = snd_pcm_hw_params_set_access(self->pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
            if (0 != rc) { print_snd_error(self, rc, "snd_pcm_hw_params_set_access"); status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED; goto fail; }

            self->effective_dsd_mode = DSD_MODE_NONE;

            bool needs_format_negotiation = true;

#if SUPPORTS_NATIVE_DSD
            bool supports_native_dsd        = self->dsd_mode == DSD_MODE_NATIVE        || 
                                              self->dsd_mode == DSD_MODE_NATIVE_OR_DCS || 
                                              self->dsd_mode == DSD_MODE_NATIVE_OR_DOP ||
                                              self->dsd_mode == DSD_MODE_DCS_OR_NATIVE || 
                                              self->dsd_mode == DSD_MODE_DOP_OR_NATIVE;

            if (format->sample_type == RAAT__SAMPLE_TYPE_DSD && supports_native_dsd) {
                // try to set this up in native mode first. If not, encapsulate instead
               
                bool prefer_encapsulate = self->dsd_mode == DSD_MODE_DCS_OR_NATIVE || self->dsd_mode == DSD_MODE_DOP_OR_NATIVE;
                bool prevent_native = false;
                if (prefer_encapsulate) {
                    RAAT__StreamFormat encapsulated_format;
                    encapsulated_format.sample_type     = RAAT__SAMPLE_TYPE_PCM;
                    encapsulated_format.sample_rate     = format->sample_rate / 16;
                    encapsulated_format.bits_per_sample = 24;
                    encapsulated_format.channels        = format->channels;
                    for (i = 0; i < nformats; i++) {
                        if (RAAT__stream_format_equals(&encapsulated_format, &formats[i])) {
                            prevent_native = true; break;
                        }
                    }
                }

                if (!prevent_native) {
                    rc = snd_pcm_hw_params_set_channels(self->pcm, hw_params, format->channels);
                    if (0 == rc) {
                        self->hw_channels = format->channels;
                    } else {
                        print_snd_error(self, rc, "failed to snd_pcm_hw_params_set_channels exactly. trying to set max channels instead");
                        rc = snd_pcm_hw_params_set_channels_last(self->pcm, hw_params, &self->hw_channels);
                        if (0 != rc) { 
                            print_snd_error(self, rc, "snd_pcm_hw_params_set_channels_last"); status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED; goto fail; 
                        }
                    }

                    if (!snd_pcm_hw_params_set_format(self->pcm, hw_params, SND_PCM_FORMAT_DSD_U32_LE) && !snd_pcm_hw_params_set_rate(self->pcm, hw_params, format->sample_rate / 32, 0)) {
                        needs_format_negotiation = false; pcmformat = *format; self->hw_format = SND_PCM_FORMAT_DSD_U32_LE; self->effective_dsd_mode = DSD_MODE_NATIVE; pcmformat.sample_rate /= 32;
                    }
                    if (!snd_pcm_hw_params_set_format(self->pcm, hw_params, SND_PCM_FORMAT_DSD_U32_BE) && !snd_pcm_hw_params_set_rate(self->pcm, hw_params, format->sample_rate / 32, 0)) {
                        needs_format_negotiation = false; pcmformat = *format; self->hw_format = SND_PCM_FORMAT_DSD_U32_BE; self->effective_dsd_mode = DSD_MODE_NATIVE; pcmformat.sample_rate /= 32;
                    }
                    if (!snd_pcm_hw_params_set_format(self->pcm, hw_params, SND_PCM_FORMAT_DSD_U16_LE) && !snd_pcm_hw_params_set_rate(self->pcm, hw_params, format->sample_rate / 16, 0)) {
                        needs_format_negotiation = false; pcmformat = *format; self->hw_format = SND_PCM_FORMAT_DSD_U16_LE; self->effective_dsd_mode = DSD_MODE_NATIVE; pcmformat.sample_rate /= 16;
                    }
                    if (!snd_pcm_hw_params_set_format(self->pcm, hw_params, SND_PCM_FORMAT_DSD_U16_BE) && !snd_pcm_hw_params_set_rate(self->pcm, hw_params, format->sample_rate / 16, 0)) {
                        needs_format_negotiation = false; pcmformat = *format; self->hw_format = SND_PCM_FORMAT_DSD_U16_BE; self->effective_dsd_mode = DSD_MODE_NATIVE; pcmformat.sample_rate /= 16;
                    }
                    if (!snd_pcm_hw_params_set_format(self->pcm, hw_params, SND_PCM_FORMAT_DSD_U8) && !snd_pcm_hw_params_set_rate(self->pcm, hw_params, format->sample_rate / 8, 0)) {
                        needs_format_negotiation = false; pcmformat = *format; self->hw_format = SND_PCM_FORMAT_DSD_U8; self->effective_dsd_mode = DSD_MODE_NATIVE; pcmformat.sample_rate /= 8;
                    }
                }
            }
#endif

            if (needs_format_negotiation) {
                bool supports_dsd_encapsulation = self->dsd_mode == DSD_MODE_DOP           || 
                                                  self->dsd_mode == DSD_MODE_DCS           || 
                                                  self->dsd_mode == DSD_MODE_NATIVE_OR_DCS || 
                                                  self->dsd_mode == DSD_MODE_NATIVE_OR_DOP ||
                                                  self->dsd_mode == DSD_MODE_DCS_OR_NATIVE || 
                                                  self->dsd_mode == DSD_MODE_DOP_OR_NATIVE;

                if (format->sample_type == RAAT__SAMPLE_TYPE_DSD && supports_dsd_encapsulation) {
                    pcmformat.sample_type     = RAAT__SAMPLE_TYPE_PCM;
                    pcmformat.sample_rate     = format->sample_rate / 16;
                    pcmformat.bits_per_sample = 24;
                    pcmformat.channels        = format->channels;

                    switch (self->dsd_mode) {
                        case DSD_MODE_DOP: case DSD_MODE_NATIVE_OR_DOP: case DSD_MODE_DOP_OR_NATIVE: self->effective_dsd_mode = DSD_MODE_DOP; break;
                        case DSD_MODE_DCS: case DSD_MODE_NATIVE_OR_DCS: case DSD_MODE_DCS_OR_NATIVE: self->effective_dsd_mode = DSD_MODE_DCS; break;
                        default: RC__ASSERT(0);
                    }
                } else {
                    pcmformat = *format;
                }

                bool prefer_larger_samples = self->volume != NULL && format->sample_type == RAAT__SAMPLE_TYPE_PCM;
                RAAT__TRACE("prefer larger samples = %d", prefer_larger_samples);

                switch (pcmformat.bits_per_sample) {
                    case 16:
                        if (prefer_larger_samples) {
                            if      (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S32_LE)))      { self->hw_format = SND_PCM_FORMAT_S32_LE;      pcmformat.bits_per_sample = 32; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S32_BE)))      { self->hw_format = SND_PCM_FORMAT_S32_BE;      pcmformat.bits_per_sample = 32; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S24_LE)))      { self->hw_format = SND_PCM_FORMAT_S24_LE;      pcmformat.bits_per_sample = 24; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S24_BE)))      { self->hw_format = SND_PCM_FORMAT_S24_BE;      pcmformat.bits_per_sample = 24; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S24_3LE)))     { self->hw_format = SND_PCM_FORMAT_S24_3LE;     pcmformat.bits_per_sample = 24; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S24_3BE)))     { self->hw_format = SND_PCM_FORMAT_S24_3BE;     pcmformat.bits_per_sample = 24; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_FLOAT_LE)))    { self->hw_format = SND_PCM_FORMAT_FLOAT_LE;    pcmformat.bits_per_sample = 32; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_FLOAT64_LE)))  { self->hw_format = SND_PCM_FORMAT_FLOAT64_LE;  pcmformat.bits_per_sample = 32; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_FLOAT_BE)))    { self->hw_format = SND_PCM_FORMAT_FLOAT_BE;    pcmformat.bits_per_sample = 32; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_FLOAT64_BE)))  { self->hw_format = SND_PCM_FORMAT_FLOAT64_BE;  pcmformat.bits_per_sample = 32; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S16_LE)))      { self->hw_format = SND_PCM_FORMAT_S16_LE; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S16_BE)))      { self->hw_format = SND_PCM_FORMAT_S16_BE; }
                            else { print_snd_error(self, rc, "snd_pcm_hw_params_set_format"); status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED; goto fail; }
                        } else {
                            if      (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S16_LE)))      self->hw_format = SND_PCM_FORMAT_S16_LE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S16_BE)))      self->hw_format = SND_PCM_FORMAT_S16_BE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S32_LE)))      self->hw_format = SND_PCM_FORMAT_S32_LE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S32_BE)))      self->hw_format = SND_PCM_FORMAT_S32_BE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S24_LE)))      self->hw_format = SND_PCM_FORMAT_S24_LE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S24_BE)))      self->hw_format = SND_PCM_FORMAT_S24_BE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S24_3LE)))     self->hw_format = SND_PCM_FORMAT_S24_3LE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S24_3BE)))     self->hw_format = SND_PCM_FORMAT_S24_3BE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_FLOAT_LE)))    self->hw_format = SND_PCM_FORMAT_FLOAT_LE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_FLOAT64_LE)))  self->hw_format = SND_PCM_FORMAT_FLOAT64_LE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_FLOAT_BE)))    self->hw_format = SND_PCM_FORMAT_FLOAT_BE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_FLOAT64_BE)))  self->hw_format = SND_PCM_FORMAT_FLOAT64_BE;
                            else { print_snd_error(self, rc, "snd_pcm_hw_params_set_format"); status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED; goto fail; }
                        }
                        break;
                    case 24:
                        if (prefer_larger_samples) {
                            if      (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S32_LE)))          { self->hw_format = SND_PCM_FORMAT_S32_LE;       pcmformat.bits_per_sample = 32; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S32_BE)))          { self->hw_format = SND_PCM_FORMAT_S32_BE;       pcmformat.bits_per_sample = 32; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_FLOAT_LE)))        { self->hw_format = SND_PCM_FORMAT_FLOAT_LE;     pcmformat.bits_per_sample = 32; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_FLOAT_BE)))        { self->hw_format = SND_PCM_FORMAT_FLOAT_BE;     pcmformat.bits_per_sample = 32; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_FLOAT64_LE)))      { self->hw_format = SND_PCM_FORMAT_FLOAT64_LE;   pcmformat.bits_per_sample = 32; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_FLOAT64_BE)))      { self->hw_format = SND_PCM_FORMAT_FLOAT64_BE;   pcmformat.bits_per_sample = 32; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S24_LE)))          { self->hw_format = SND_PCM_FORMAT_S24_LE; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S24_BE)))          { self->hw_format = SND_PCM_FORMAT_S24_BE; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S24_3LE)))         { self->hw_format = SND_PCM_FORMAT_S24_3LE; }
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S24_3BE)))         { self->hw_format = SND_PCM_FORMAT_S24_3BE; }
                            else { print_snd_error(self, rc, "snd_pcm_hw_params_set_format"); status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED; goto fail; }
                        } else {
                            if      (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S32_LE)))          self->hw_format = SND_PCM_FORMAT_S32_LE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S32_BE)))          self->hw_format = SND_PCM_FORMAT_S32_BE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S24_3LE)))         self->hw_format = SND_PCM_FORMAT_S24_3LE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S24_3BE)))         self->hw_format = SND_PCM_FORMAT_S24_3BE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S24_LE)))          self->hw_format = SND_PCM_FORMAT_S24_LE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S24_BE)))          self->hw_format = SND_PCM_FORMAT_S24_BE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_FLOAT_LE)))        self->hw_format = SND_PCM_FORMAT_FLOAT_LE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_FLOAT_BE)))        self->hw_format = SND_PCM_FORMAT_FLOAT_BE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_FLOAT64_LE)))      self->hw_format = SND_PCM_FORMAT_FLOAT64_LE;
                            else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_FLOAT64_BE)))      self->hw_format = SND_PCM_FORMAT_FLOAT64_BE;
                            else { print_snd_error(self, rc, "snd_pcm_hw_params_set_format"); status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED; goto fail; }
                        }
                        break;
                    case 32:
                        if      (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S32_LE)))          self->hw_format = SND_PCM_FORMAT_S32_LE;
                        else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_S32_BE)))          self->hw_format = SND_PCM_FORMAT_S32_BE;
                        else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_FLOAT64_LE)))      self->hw_format = SND_PCM_FORMAT_FLOAT64_LE;
                        else if (0 == (rc = snd_pcm_hw_params_test_format(self->pcm, hw_params, SND_PCM_FORMAT_FLOAT64_BE)))      self->hw_format = SND_PCM_FORMAT_FLOAT64_BE;
                        else { print_snd_error(self, rc, "snd_pcm_hw_params_set_format"); status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED; goto fail; }
                        break;
                    default: status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED; goto fail;
                }

                rc = snd_pcm_hw_params_set_format(self->pcm, hw_params, self->hw_format);
                if (0 != rc) { print_snd_error(self, rc, "snd_pcm_hw_params_set_format"); status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED; goto fail; }

                RAAT__TRACE("[ALSA] [%s] using hw pcmformat %s bitspersample %d", self->unique_id, snd_pcm_format_name(self->hw_format), pcmformat.bits_per_sample);

                rc = snd_pcm_hw_params_set_rate(self->pcm, hw_params, pcmformat.sample_rate, 0);
                if (0 != rc) { print_snd_error(self, rc, "snd_pcm_hw_params_set_rate"); status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED; goto fail; }

                rc = snd_pcm_hw_params_set_channels(self->pcm, hw_params, pcmformat.channels);
                if (0 == rc) {
                    self->hw_channels = pcmformat.channels;
                } else {
                    print_snd_error(self, rc, "failed to snd_pcm_hw_params_set_channels exactly. trying to set max channels instead");
                    rc = snd_pcm_hw_params_set_channels_last(self->pcm, hw_params, &self->hw_channels);
                    if (0 != rc) { 
                        print_snd_error(self, rc, "snd_pcm_hw_params_set_channels_last"); status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED; goto fail; 
                    }
                }
            }

            self->periods = 2;
            int dir = 0;
            RAAT__TRACE("[ALSA] [%s] Requesting %d periods", self->unique_id, (int)self->periods);
            rc = snd_pcm_hw_params_set_periods_near(self->pcm, hw_params,  &self->periods, &dir);
            if (0 != rc) { print_snd_error(self, rc, "snd_pcm_hw_params_set_periods_near"); status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED; goto fail; }

            self->pcm_samples_per_buf = (int)(pcmformat.sample_rate * self->buffer_duration_secs);

            RAAT__TRACE("[ALSA] [%s] Requesting %d frames/buffer (%d frames/period)", self->unique_id, (int)self->pcm_samples_per_buf, (int)self->pcm_samples_per_buf/self->periods);
            rc = snd_pcm_hw_params_set_buffer_size_near(self->pcm, hw_params, &self->pcm_samples_per_buf);
            if (0 != rc) { print_snd_error(self, rc, "snd_pcm_hw_params_set_buffer_size_near"); status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED; goto fail; }

            int64_t full_pcm_samples_per_buf = self->pcm_samples_per_buf;
            self->pcm_samples_per_buf /= self->periods;         

            RAAT__TRACE("[ALSA] [%s] Initialized with %d periods with %d frames/period and %d frames/buffer", self->unique_id, (int)self->periods, (int)self->pcm_samples_per_buf, (int)full_pcm_samples_per_buf);

            // retry up to 3x at 100ms intervals because some DACs briefly disappear during param setting
            rc = snd_pcm_hw_params(self->pcm, hw_params);
            if (rc != 0) { usleep(100000); rc = snd_pcm_hw_params(self->pcm, hw_params); }
            if (rc != 0) { usleep(100000); rc = snd_pcm_hw_params(self->pcm, hw_params); }
            if (rc != 0) { usleep(100000); rc = snd_pcm_hw_params(self->pcm, hw_params); }
            if (0 != rc) { print_snd_error(self, rc, "snd_pcm_hw_params"); status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED; goto fail; }

            if (format->channels > 2) {
                snd_pcm_chmap_t *map = malloc(sizeof(int) * (format->channels + 1));
                map->channels = format->channels;
                switch (format->channels) {
                    /* N/A
                    case 1:
                        map->pos[0] = SND_CHMAP_MONO;
                        break;

                    case 2:
                        map->pos[0] = SND_CHMAP_FL;
                        map->pos[1] = SND_CHMAP_FR;
                        break;
                    */
                    case 3:
                        map->pos[0] = SND_CHMAP_FL;
                        map->pos[1] = SND_CHMAP_FR;
                        map->pos[2] = SND_CHMAP_FC;
                        break;

                    case 4:
                        map->pos[0] = SND_CHMAP_FL;
                        map->pos[1] = SND_CHMAP_FR;
                        map->pos[2] = SND_CHMAP_RL;
                        map->pos[3] = SND_CHMAP_RR;
                        break;

                    case 5:
                        map->pos[0] = SND_CHMAP_FL;
                        map->pos[1] = SND_CHMAP_FR;
                        map->pos[2] = SND_CHMAP_FC;
                        map->pos[3] = SND_CHMAP_RL;
                        map->pos[4] = SND_CHMAP_RR;
                        break;

                    case 6:
                        map->pos[0] = SND_CHMAP_FL;
                        map->pos[1] = SND_CHMAP_FR;
                        map->pos[2] = SND_CHMAP_FC;
                        map->pos[3] = SND_CHMAP_LFE;
                        map->pos[4] = SND_CHMAP_RL;
                        map->pos[5] = SND_CHMAP_RR;
                        break;

                    case 7:
                        map->pos[0] = SND_CHMAP_FL;
                        map->pos[1] = SND_CHMAP_FR;
                        map->pos[2] = SND_CHMAP_FC;
                        map->pos[3] = SND_CHMAP_LFE;
                        map->pos[4] = SND_CHMAP_RL;
                        map->pos[5] = SND_CHMAP_RR;
                        map->pos[6] = SND_CHMAP_RC;
                        break;

                    default: 
                        map->pos[0] = SND_CHMAP_FL;
                        map->pos[1] = SND_CHMAP_FR;
                        map->pos[2] = SND_CHMAP_FC;
                        map->pos[3] = SND_CHMAP_LFE;
                        map->pos[4] = SND_CHMAP_RL;
                        map->pos[5] = SND_CHMAP_RR;
                        map->pos[6] = SND_CHMAP_RLC;
                        map->pos[7] = SND_CHMAP_RRC;
                        int rch;
                        for (rch = 8; rch < format->channels; rch++) map->pos[rch] = SND_CHMAP_UNKNOWN;
                        break;
                }
                rc = snd_pcm_set_chmap(self->pcm, map);
                if (0 != rc) { print_snd_error(self, rc, "snd_pcm_set_chmap"); }
                free(map);
            }

            // OK. we are initialized. Now do the non-alsa stuff
            //
            self->current_token = token = ++self->next_token;
            status                      = RC__STATUS_SUCCESS;
            self->origformat            = *format;
            self->pcmformat             = pcmformat;
            self->state                 = STOPPED;

            RAAT__drift_correction_init(&self->drift_correction, self->log, format);

            self->cb_lost               = cb_lost;
            self->cb_lost_userdata      = cb_lost_userdata;

            AlsaOutputThreadState *threadstate = RC__new0(RC__ALLOCATOR_DEFAULT, AlsaOutputThreadState, 1);
            threadstate->self  = self;
            threadstate->token = token;

            if (self->force_max_volume) {
                LOCKED_force_max_volume(self);
            }

            self->sync_offset_ns = 0;
            json_t *j_sync_offset = json_object_get(self->config, "sync_offset_ns");
            if (j_sync_offset && json_typeof(j_sync_offset) == JSON_ARRAY) {
                size_t index;
                json_t *value;

                json_array_foreach(j_sync_offset, index, value) {
                    json_t *j_sample_rate = json_object_get(value, "sample_rate");
                    json_t *j_offset_ns   = json_object_get(value, "offset");
                    json_t *j_subtype     = json_object_get(value, "sample_subtype");
                    if (json_is_number(j_sample_rate) && json_is_number(j_offset_ns)) {
                        int     sample_rate = json_integer_value(j_sample_rate);
                        int64_t offset_ns   = json_integer_value(j_offset_ns);

                        if (format->sample_rate == sample_rate) {
                            if (json_is_string(j_subtype)) {
                                const char *subtype = json_string_value(j_subtype);
                                if (!strcmp(subtype, "mqa")) {
                                    if (format->sample_subtype == RAAT__SAMPLE_SUBTYPE_MQA) {
                                        self->sync_offset_ns = offset_ns;
                                        break;
                                    }
                                } else if (!strcmp(subtype, "mqa_core")) {
                                    if (format->sample_subtype == RAAT__SAMPLE_SUBTYPE_MQA_CORE) {
                                        self->sync_offset_ns = offset_ns;
                                        break;
                                    }
                                } else {
                                    RAAT__TRACE("[ALSA] [%s] invalid sample subtype for sync_offset_ns at index %d: '%s'", self->unique_id, (int)index, subtype);
                                    break;
                                }
                            } else {
                                self->sync_offset_ns = offset_ns;
                                break;
                            }
                        }
                    } else {
                        RAAT__TRACE("[ALSA] [%s] invalid config entry for sync_offset_ns at index %d", self->unique_id, (int)index);
                        break;
                    }
                }
            } else if (json_is_number(j_sync_offset)) {
                self->sync_offset_ns = json_integer_value(j_sync_offset);
            }

            if (self->sync_offset_ns) {
                RAAT__TRACE("[ALSA] [%s] using configured sync adjustment %lld ns for %dhz", self->unique_id, self->sync_offset_ns, format->sample_rate);
            }

            uv_thread_create(&self->tid, alsa_output_thread, threadstate);

            LOCKED_update_signal_path(self);
        } else {
            status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED;
        }
    }

fail:
    if (formats) RC__free(self->alloc, formats);
    uv_mutex_unlock(&self->lock);

    // mutexes might not be recursive, so the following happen outside of the lock.
    if (self->volume && status == RC__STATUS_SUCCESS) {
        if (format->sample_type == RAAT__SAMPLE_TYPE_DSD) {
            self->volume->setup(self->volume->userdata, format, &self->volume_delay);
        } else {
            self->volume->setup(self->volume->userdata, &pcmformat, &self->volume_delay);
        }
    }

    if (status == RC__STATUS_SUCCESS) {
        self->resync_delay_remaining_ns = self->resync_delay_secs * 1000000000LL;
        self->setup_cb          = cb_setup;
        self->setup_cb_userdata = cb_setup_userdata;
    } else {
        // call our setup callback
        cb_setup(cb_setup_userdata, status, token);
    }
}

static RC__Status output_teardown(void *vself, int token) {
    AlsaOutputPlugin *self = (AlsaOutputPlugin*)vself;
    RC__Status status = RC__STATUS_UNEXPECTED_ERROR;

    bool needs_join = false;
    snd_pcm_t * close_pcm = NULL;

    uv_mutex_lock(&self->lock);
    if (self->current_token == token) {
        RAAT__TRACE("[alsa] teardown");
        self->current_token = RAAT__OUTPUT_TOKEN_INVALID;
        self->state = IDLE;
        self->cb_lost = NULL;
        self->cb_lost_userdata = NULL;
        LOCKED__on_teardown(self);

        if (self->stream != NULL) {
            RAAT__stream_decref(self->stream);
            self->stream = NULL;
        }

        needs_join = true;

        if (self->pcm) {
            close_pcm = self->pcm;
            self->pcm = NULL;
        }

        status = RC__STATUS_SUCCESS;
    } else {
        status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_TOKEN;
    }
    uv_mutex_unlock(&self->lock);

    if (needs_join) {
        uv_thread_join(&self->tid);
    }
    if (close_pcm) {
        snd_pcm_close(close_pcm);
    }

    return status;
}

static RC__Status output_stop(void *vself, int token) {
    AlsaOutputPlugin *self = (AlsaOutputPlugin*)vself;
    RC__Status status = RC__STATUS_UNEXPECTED_ERROR;

    uv_mutex_lock(&self->lock);
    if (self->current_token == token) {
        if (self->state == RUNNING) {
            self->state = STOPPED;
            RAAT__stream_decref(self->stream);
            self->stream = NULL;
            status = RC__STATUS_SUCCESS;
        } else {
            status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_STATE;
        }
    } else {
        status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_TOKEN;
    }
    uv_mutex_unlock(&self->lock);

    return status;
}

static RC__Status output_start(void *vself, int token, int64_t time, int64_t streamsample, RAAT__Stream *stream) {
    AlsaOutputPlugin *self = (AlsaOutputPlugin*)vself;
    RC__Status status = RC__STATUS_UNEXPECTED_ERROR;

    uv_mutex_lock(&self->lock);
    if (self->current_token == token) {
        if (self->state == STOPPED) {
            RAAT__stream_incref(stream);
            self->stream             = stream;
            self->start_streamsample = streamsample;
            self->start_time         = time;
            self->new_stream         = true;
            self->state              = RUNNING;
            status = RC__STATUS_SUCCESS;
        } else {
            status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_STATE;
        }
    } else {
        status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_TOKEN;
    }
    uv_mutex_unlock(&self->lock);

    return status;
}

static int64_t LOCKED_get_local_time(AlsaOutputPlugin *self) {
    int64_t now = RC__now_ns();
    int64_t last_wall_sample = self->last_monotonic_sample;
    return RAAT__stream_format_samples_to_ns(&self->origformat, last_wall_sample) + (now - self->last_monotonic_sample_systime);
}

static RC__Status output_get_output_delay(void *vself, int token, int64_t *out_delay) {
    AlsaOutputPlugin *self = (AlsaOutputPlugin*)vself;
    RC__Status status = RC__STATUS_UNEXPECTED_ERROR;
    uv_mutex_lock(&self->lock);
    if (self->current_token == token) {
        if (self->state != IDLE) {
            *out_delay = self->last_delay_ns;
            status = RC__STATUS_SUCCESS;
        } else {
            status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_STATE;
        }
    } else {
        status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_TOKEN;
    }
    uv_mutex_unlock(&self->lock);
    return status;
}

static RC__Status output_get_local_time(void *vself, int token, int64_t *out_time) {
    AlsaOutputPlugin *self = (AlsaOutputPlugin*)vself;
    RC__Status status = RC__STATUS_UNEXPECTED_ERROR;
    uv_mutex_lock(&self->lock);
    if (self->current_token == token) {
        if (self->state != IDLE) {
            *out_time = LOCKED_get_local_time(self);
            status = RC__STATUS_SUCCESS;
        } else {
            status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_STATE;
        }
    } else {
        status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_TOKEN;
    }
    uv_mutex_unlock(&self->lock);
    return status;
}

static RC__Status output_set_remote_time(void *vself, int token, int64_t remote_time_offset, bool new_source) {
    AlsaOutputPlugin *self = (AlsaOutputPlugin*)vself;
    RC__Status status = RC__STATUS_UNEXPECTED_ERROR;
    uv_mutex_lock(&self->lock);
    if (self->current_token == token) {
        if (self->state != IDLE) {
            int64_t local_time  = LOCKED_get_local_time(self);
            RAAT__drift_correction_set_remote_time(&self->drift_correction, local_time, remote_time_offset, new_source);
            status = RC__STATUS_SUCCESS;
        } else {
            status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_STATE;
        }
    } else {
        status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_TOKEN;
    }
    uv_mutex_unlock(&self->lock);
    return status;
}

static RC__Status 
output_add_message_listener(void *vself, RAAT__OutputMessageCallback cb, void *cb_userdata) {
    AlsaOutputPlugin *self = (AlsaOutputPlugin*)vself;
    return RAAT__output_message_listeners_add(&self->message_listeners, cb, cb_userdata);
}

static RC__Status 
output_remove_message_listener(void *vself, RAAT__OutputMessageCallback cb, void *cb_userdata) {
    AlsaOutputPlugin *self = (AlsaOutputPlugin*)vself;
    return RAAT__output_message_listeners_remove(&self->message_listeners, cb, cb_userdata);
}

static RC__Status 
output_send_message(void *vself, json_t *message) {
    AlsaOutputPlugin *self = (AlsaOutputPlugin*)vself;
    char *s = json_dumps(message, 0);
    RAAT__TRACE("[alsa] [%s] GOT MESSAGE %s", self->unique_id, s);
    free(s);

    return RC__STATUS_SUCCESS;
}

static RC__Status 
output_set_software_volume_signal_path(void *vself, json_t *soft_volume_signal_path) {
    AlsaOutputPlugin *self = (AlsaOutputPlugin*)vself;
    uv_mutex_lock(&self->lock);
    if (self->soft_volume_signal_path) json_decref(self->soft_volume_signal_path);
    self->soft_volume_signal_path = json_incref(soft_volume_signal_path);
    LOCKED_update_signal_path(self);
    uv_mutex_unlock(&self->lock);
    return RC__STATUS_SUCCESS;
}   

static RC__Status 
output_set_software_volume(void *vself, RAAT__OutputSoftwareVolume *volume) {
    AlsaOutputPlugin *self = (AlsaOutputPlugin*)vself;
    uv_mutex_lock(&self->lock);
    self->volume          = volume;
    uv_mutex_unlock(&self->lock);
    return RC__STATUS_SUCCESS;
}

RC__Status 
RAAT__alsa_output_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__OutputPlugin **out_output) {
    const char *unique_id = json_string_value(json_object_get(config, "device"));
    if (unique_id == NULL) return RAAT__OUTPUT_PLUGIN_STATUS_INVALID_CONFIG;

    if (!json_is_true(json_object_get(config, "skip_startup_device_check"))) {
        // check to make sure device exists
        snd_pcm_t *pcm = NULL;
        int rc = snd_pcm_open(&pcm, unique_id, SND_PCM_STREAM_PLAYBACK, 0);
        if (0 != rc) { return RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED; }
        snd_pcm_close(pcm);
    }

    alloc = RC__allocator_default(alloc);
    AlsaOutputPlugin *self            = RC__new0(alloc, AlsaOutputPlugin, 1);
    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;
#if 0   // Sample Rate Upsampling
 AlsaOutputPlugin *myself = (AlsaOutputPlugin*)g_output_plugin_ptr;
 g_sample_rate_ptr = &(myself->origformat.sample_rate);
#endif  // Sample Rate Upsampling
    // initialize the state that is associated with AlsaOutputPlugin 
    self->alloc                        = alloc;
    self->log                          = RAAT__device_get_log(device);
    self->current_token                = RAAT__OUTPUT_TOKEN_INVALID;
    self->next_token                   = 1;
    self->state                        = IDLE;
    self->unique_id                    = RC__allocator_strdup(alloc, unique_id);
    self->config                       = config;

    json_incref(self->config);
    roon_signal_path_ptr = &roon_signal_path;	// mcmurray func ptr
    RAAT__TRACE("[output/alsa] initializing output uniqueid=%s", unique_id);

    json_t *buffer_duration = json_object_get(config, "buffer_duration");
    if (buffer_duration) self->buffer_duration_secs = json_number_value(buffer_duration);
    if (self->buffer_duration_secs == 0) self->buffer_duration_secs = .040;
    RAAT__TRACE("[output/alsa] preferred buffer duration=%fs", self->buffer_duration_secs);

    if (json_is_true(json_object_get(config, "force_max_volume")))
        self->force_max_volume = true;
    RAAT__TRACE("[output/alsa] force_max_volume=%d", self->force_max_volume);

    json_t *custom_signal_path = json_object_get(config, "signal_path");
    if (custom_signal_path) {
        self->custom_signal_path = json_copy(custom_signal_path);
    }

    json_t *resync_delay = json_object_get(config, "resync_delay");
    if (resync_delay) self->resync_delay_secs = json_number_value(resync_delay);
    if (self->resync_delay_secs == 0) self->resync_delay_secs = 0.1;
    RAAT__TRACE("[output/alsa] resync delay=%fs", self->resync_delay_secs);

    json_t *max_pcm_rate = json_object_get(config, "max_pcm_rate");
    if (max_pcm_rate) self->max_pcm_rate = json_number_value(max_pcm_rate);
    if (self->max_pcm_rate == 0) self->max_pcm_rate = 44100*32;
    RAAT__TRACE("[ALSA] max pcm rate=%d", self->max_pcm_rate);

    json_t *max_dsd_rate = json_object_get(config, "max_dsd_rate");
    if (max_dsd_rate) self->max_dsd_rate = json_number_value(max_dsd_rate);
    if (self->max_dsd_rate == 0) self->max_dsd_rate = 1024;
    RAAT__TRACE("[output/alsa] max dsd rate=%d", self->max_dsd_rate);

    const char *dsd_mode_s     = json_string_value(json_object_get(config, "dsd_mode"));
    if      (dsd_mode_s && !strcmp(dsd_mode_s, "dop"))           self->dsd_mode = DSD_MODE_DOP;
    else if (dsd_mode_s && !strcmp(dsd_mode_s, "dcs"))           self->dsd_mode = DSD_MODE_DCS;
    else if (dsd_mode_s && !strcmp(dsd_mode_s, "native"))        self->dsd_mode = DSD_MODE_NATIVE;
    else if (dsd_mode_s && !strcmp(dsd_mode_s, "native_or_dop")) self->dsd_mode = DSD_MODE_NATIVE_OR_DOP;
    else if (dsd_mode_s && !strcmp(dsd_mode_s, "native_or_dcs")) self->dsd_mode = DSD_MODE_NATIVE_OR_DCS;
    else if (dsd_mode_s && !strcmp(dsd_mode_s, "dop_or_native")) self->dsd_mode = DSD_MODE_DOP_OR_NATIVE;
    else if (dsd_mode_s && !strcmp(dsd_mode_s, "dcs_or_native")) self->dsd_mode = DSD_MODE_DCS_OR_NATIVE;
    else                                                         self->dsd_mode = DSD_MODE_NONE;
    RAAT__TRACE("[output/alsa] dsd_mode=%s", dsd_mode_s == NULL ? "none" : dsd_mode_s);

    // set up vtable of plugin functions
    self->plugin.get_info                        = output_get_info;
    self->plugin.get_supported_formats           = output_get_supported_formats;
    self->plugin.setup                           = output_setup;
    self->plugin.teardown                        = output_teardown;
    self->plugin.start                           = output_start;
    self->plugin.get_local_time                  = output_get_local_time;
    self->plugin.set_remote_time                 = output_set_remote_time;
    self->plugin.stop                            = output_stop;
    self->plugin.force_teardown                  = output_force_teardown;
    self->plugin.send_message                    = output_send_message;
    self->plugin.add_message_listener            = output_add_message_listener;
    self->plugin.remove_message_listener         = output_remove_message_listener;
    self->plugin.set_software_volume             = output_set_software_volume;
    self->plugin.set_software_volume_signal_path = output_set_software_volume_signal_path;
    self->plugin.get_output_delay                = output_get_output_delay;

    RAAT__output_message_listeners_init(&self->message_listeners, self->alloc);

    self->info = json_object();

    json_object_set(self->info, "config", config);

    RAAT__TRACE("[output/alsa] getting hardware info");
    get_hw_info(self);

    RC__Status status;

    json_t *j_supported_formats = json_object_get(config, "supported_formats");
    if (j_supported_formats) {
        if (!json_is_array(j_supported_formats)) {
            RAAT__ERROR("[output/alsa] config was missing required array element 'supported_formats'");
            status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_CONFIG; goto exit;
        }

        self->supported_formats = RC__new0(self->alloc, RAAT__StreamFormat, json_array_size(j_supported_formats));

        size_t index;
        json_t *value;
        json_array_foreach(j_supported_formats, index, value) {
            json_t *j_sample_type     = json_object_get(value, "sample_type");
            json_t *j_sample_rate     = json_object_get(value, "sample_rate");
            json_t *j_bits_per_sample = json_object_get(value, "bits_per_sample");
            json_t *j_channels        = json_object_get(value, "channels");

            if (!j_sample_type) {
                RAAT__ERROR("[output/alsa] [%s] while parsing supported_formats, missing required field 'sample_type'", self->unique_id);
                status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_CONFIG; goto exit;
            }
            if (!j_sample_rate) {
                RAAT__ERROR("[output/alsa] [%s] while parsing supported_formats, missing required field 'sample_rate'", self->unique_id);
                status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_CONFIG; goto exit;
            }
            if (!j_bits_per_sample) {
                RAAT__ERROR("[output/alsa] [%s] while parsing supported_formats, missing required field 'bits_per_sample'", self->unique_id);
                status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_CONFIG; goto exit;
            }
            if (!j_channels) {
                RAAT__ERROR("[output/alsa] [%s] while parsing supported_formats, missing required field 'channels'", self->unique_id);
                status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_CONFIG; goto exit;
            }

            RAAT__SampleType sample_type;
            const char *s_sample_type = json_string_value(j_sample_type);
            if (!strcmp(s_sample_type, "pcm")) {
                sample_type = RAAT__SAMPLE_TYPE_PCM;
            } else if (!strcmp(s_sample_type, "dsd")) {
                sample_type = RAAT__SAMPLE_TYPE_DSD;
            } else {
                RAAT__ERROR("[output/alsa] [%s] while parsing supported_formats, got invalid sample type '%s'", self->unique_id, s_sample_type == NULL ? "(null)" : s_sample_type);
                status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_CONFIG; goto exit;
            }

            int sample_rate     = json_integer_value(j_sample_rate);
            int bits_per_sample = json_integer_value(j_bits_per_sample);
            int channels        = json_integer_value(j_channels);

            if (sample_rate == 0) {
                RAAT__ERROR("[output/alsa] [%s] while parsing supported_formats, got invalid sample_rate", self->unique_id);
                status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_CONFIG; goto exit;
            }
            if (bits_per_sample == 0) {
                RAAT__ERROR("[output/alsa] [%s] while parsing supported_formats, got invalid bits_per_sample", self->unique_id);
                status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_CONFIG; goto exit;
            }
            if (channels == 0) {
                RAAT__ERROR("[output/alsa] [%s] while parsing supported_formats, got invalid channels", self->unique_id);
                status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_CONFIG; goto exit;
            }

            self->supported_formats[self->n_supported_formats].sample_type     = sample_type;
            self->supported_formats[self->n_supported_formats].bits_per_sample = bits_per_sample;
            self->supported_formats[self->n_supported_formats].sample_rate     = sample_rate;
            self->supported_formats[self->n_supported_formats].channels        = channels;

            RAAT__TRACE("[output/alsa] [%s] [hardcoded] supports %s format %d/%d/%d", self->unique_id, s_sample_type, sample_rate, bits_per_sample, channels);

            self->n_supported_formats++;
        }


    } else {
        RAAT__TRACE("[output/alsa] probing formats");
        probe_formats(self);
    }

    uv_mutex_init(&self->lock);
    RAAT__TRACE("[output/alsa] initialized");
    *out_output = &self->plugin;
    return RC__STATUS_SUCCESS;

exit:
    return status;
}

void
RAAT__alsa_output_plugin_delete(RAAT__OutputPlugin *output) {
    AlsaOutputPlugin *self = (AlsaOutputPlugin*)output;

    uv_mutex_destroy(&self->lock);
    if (self->info)                    json_decref(self->info);
    if (self->config)                  json_decref(self->config);
    if (self->custom_signal_path)      json_decref(self->custom_signal_path);
    if (self->soft_volume_signal_path) json_decref(self->soft_volume_signal_path);
    RAAT__output_message_listeners_destroy(&self->message_listeners);
    RC__free(self->alloc, self);
}

