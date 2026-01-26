//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_plugin_output_capture.h"
#include "raat_dsp.h"
#include "rc_list.h"

#include <uv.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

#define RAAT__CURRENT_LOG self->log

/*
 * Output Plugin
 */

typedef enum {
    IDLE                = 0,
    STOPPED             = 1,
    RUNNING             = 2,
} CaptureOutputState;

typedef enum {
    DSD_MODE_NONE,
    DSD_MODE_DOP,
    DSD_MODE_DCS,
} DsdMode;

typedef struct {
    RAAT__OutputPlugin           plugin;          // must be first item in struct

    RC__Allocator               *alloc;
    RAAT__Log                   *log;

    uv_mutex_t                   lock;

    RAAT__OutputMessageListeners message_listeners;

    CaptureOutputState             state;

    json_t                      *custom_signal_path;

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

    // state for capture
    unsigned                     periods;
    double                       buffer_duration_secs;
    int                          pcm_samples_per_buf;
    DsdMode                      dsd_mode;
    int                          max_dsd_rate;

    double                       resync_delay_secs;
    int64_t                      resync_delay_remaining_ns;

    bool                         dsd_dop_flipper;

    RAAT__OutputSetupCallback    setup_cb;
    void *                       setup_cb_userdata;

    RC__Status                   get_supported_formats_status;
    RAAT__StreamFormat          *supported_formats;
    size_t                       n_supported_formats;

    RAAT__DriftCorrection        drift_correction;

    uv_thread_t                  tid;

    RAAT__OutputSoftwareVolume  *volume;

    json_t                      *info;

    int                          wavidx;
    FILE                        *outputfile;
} CaptureOutputPlugin;

static void close_file(CaptureOutputPlugin *self) {
    if (self->outputfile == NULL) return;
    uint32_t length = (uint32_t)ftell(self->outputfile);

    uint32_t a = length - 8;
    RC__little_endian(&a, 4);
    fseek(self->outputfile, 4, SEEK_SET);
    fwrite(&a, 4, 1, self->outputfile);

    uint32_t b = length - 44;
    RC__little_endian(&b, 4);
    fseek(self->outputfile, 40, SEEK_SET);
    fwrite(&b, 4, 1, self->outputfile);

    fclose(self->outputfile);
    self->outputfile = NULL;
}

static void open_file(CaptureOutputPlugin *self) {
    char filename[1024];
    sprintf(filename, "capture_%04d.wav", self->wavidx++);

    close_file(self);
    self->outputfile = fopen(filename, "wb");

    uint32_t bps      = (uint32_t)(self->pcmformat.bits_per_sample);
    uint32_t channels = (uint32_t)(self->pcmformat.channels);
    uint32_t srate    = (uint32_t)(self->pcmformat.sample_rate);
    uint32_t avgrate  = (uint32_t)(channels * bps / 8 * srate);

    RAAT__TRACE("writing wav header bps=%d channels=%d srate=%d avgrate=%d", bps, channels, srate, avgrate);

    unsigned char hdr[44] = {
        'R',  'I',  'F',  'F',
        '\0', '\0', '\0', '\0',
        'W',  'A',  'V',  'E',
        'f',  'm',  't',  ' ',
        16,   0,    0,    0,            // length of fmt
        1, 0,                           // format = PCM
        (uint8_t)channels, 0,
        (uint8_t)((srate  >>0)&0xff), (uint8_t)((srate  >>8)&0xff), (uint8_t)((srate  >>16)&0xff), (uint8_t)((srate  >>24)&0xff), 
        (uint8_t)((avgrate>>0)&0xff), (uint8_t)((avgrate>>8)&0xff), (uint8_t)((avgrate>>16)&0xff), (uint8_t)((avgrate>>24)&0xff), 
        (uint8_t)(channels * bps / 8), 0,
        (uint8_t)(bps), 0,
        'd',  'a',  't',  'a',
        '\0', '\0', '\0', '\0',
    };
    fwrite(hdr, 44, 1, self->outputfile);
}

static RC__Status output_get_info(void *vself, json_t **out_info) {
    CaptureOutputPlugin *self = (CaptureOutputPlugin*)vself;

    RC__ASSERT(self);
    RC__ASSERT(out_info);

    json_incref(self->info);
    *out_info = self->info;

    return RC__STATUS_SUCCESS;
}

static void update_signal_path(CaptureOutputPlugin *self) {
    json_t *message = json_object();
    json_t *signal_path    = json_array();

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
        json_t *output = json_object();
        json_object_set_new(output, "type", json_string("output"));
        json_object_set_new(output, "method", json_string("other"));
        json_object_set_new(output, "quality", json_string("lossless"));
        json_array_append_new(signal_path, output);
    }

    json_object_set_new(message, "signal_path", signal_path);
    RAAT__output_message_listeners_invoke(&self->message_listeners, message);
    json_decref(message);
}

static int count(const int *arr) {
    int i = 0;
    while (arr[i]) i++;
    return i;
}

static RC__Status output_get_supported_formats(void *vself, RC__Allocator *alloc, size_t *out_nformats, RAAT__StreamFormat/*?*/ **out_formats) {
    int pcmbaserate[]      = { 44100, 48000,       0 };
    int pcmmult[]          = { 1, 2, 4, 8,         0 };
    int channels[]         = { 1, 2,               0 };
    int pcmbitspersample[] = { 16, 24,             0 };
    int dsdrate[]          = { 44100*64, 44100*128, 44100*256, 44100*512, 0 };

    int channel_i, pcmbaserate_i, pcmmult_i, pcmbitspersample_i;
    int idx = 0;

    size_t              nformats = count(channels) * (count(pcmbaserate) * count(pcmmult) * count(pcmbitspersample) + count(dsdrate));
    RAAT__StreamFormat *formats  = RC__new0(alloc, RAAT__StreamFormat, nformats);

    if (formats == NULL) return RC__STATUS_OUT_OF_MEMORY;

    for (channel_i = 0; channels[channel_i]; channel_i++) {
        for (pcmmult_i = 0; pcmmult[pcmmult_i]; pcmmult_i++)
        for (pcmbaserate_i = 0; pcmbaserate[pcmbaserate_i]; pcmbaserate_i++)
        for (pcmbitspersample_i = 0; pcmbitspersample[pcmbitspersample_i]; pcmbitspersample_i++) {
            formats[idx].sample_type     = RAAT__SAMPLE_TYPE_PCM;
            formats[idx].sample_rate     = pcmbaserate[pcmbaserate_i] * pcmmult[pcmmult_i];
            formats[idx].bits_per_sample = pcmbitspersample[pcmbitspersample_i];
            formats[idx].channels        = channels[channel_i];
            idx++;
        }

        /*
        for (dsdrate_i = 0; dsdrate[dsdrate_i]; dsdrate_i++) {
            formats[idx].sample_type     = RAAT__SAMPLE_TYPE_DSD;
            formats[idx].sample_rate     = dsdrate[dsdrate_i];
            formats[idx].bits_per_sample = 1;
            formats[idx].channels        = channels[channel_i];
            idx++;
        }
        */
    }

    *out_nformats = nformats;
    *out_formats  = formats;

    return RC__STATUS_SUCCESS;
}

typedef struct {
    CaptureOutputPlugin *self;
    int                token;
} CaptureOutputThreadState;

static RC__Status output_force_teardown(void *vself, json_t *reason) {
    CaptureOutputPlugin *self = (CaptureOutputPlugin*)vself;

    RC__Status status = RC__STATUS_SUCCESS;
    int        token = RAAT__OUTPUT_TOKEN_INVALID;

    RAAT__OutputLostCallback  old_cb_lost          = NULL;
    void                     *old_cb_lost_userdata = NULL;
    bool                      needs_join           = false;

    uv_mutex_lock(&self->lock);
retry:
    self->current_token = token = ++self->next_token;

    if (self->state != IDLE) {
        RAAT__TRACE("[capture] kicking off old output in force teardown");
        needs_join             = true;
        self->state            = IDLE;
        close_file(self);
        old_cb_lost            = self->cb_lost;
        old_cb_lost_userdata   = self->cb_lost_userdata;
        self->cb_lost          = NULL;
        self->cb_lost_userdata = NULL;
    }
    uv_mutex_unlock(&self->lock);

    if (needs_join) {
        RAAT__TRACE("[capture] joining thread in force teardown");
        uv_thread_join(&self->tid);
    }

    if (old_cb_lost != NULL) {
        old_cb_lost(old_cb_lost_userdata, reason);
    }

    uv_mutex_lock(&self->lock);
    if (self->current_token != token) {
        RAAT__TRACE("[capture] force teardown required a retry");
        goto retry;
    }

    // clean up leftover stream 
    if (self->stream != NULL) {
        RAAT__stream_decref(self->stream);
        self->stream = NULL;
    }

    uv_mutex_unlock(&self->lock);

    return status;
}

static void capture_output_thread(void *vself) {
    CaptureOutputThreadState *threadstate = vself;
    CaptureOutputPlugin      *self        = threadstate->self;

    RAAT__Stream       *stream = NULL;
    RAAT__StreamFormat  origformat;
    RAAT__StreamFormat  pcmformat;
    bool                finished = false;
    uint8_t            *buf      = NULL;

    uv_mutex_lock(&self->lock);
    if (threadstate->token == self->current_token) {
        pcmformat  = self->pcmformat;
        origformat = self->origformat;
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
        origformat_samples_per_buf = self->pcm_samples_per_buf * 16;
    } else {
        RC__ASSERT(false);
    }

    int     ns_per_buf      = (int)RAAT__stream_format_samples_to_ns(&pcmformat, self->pcm_samples_per_buf);
    int     bytes_per_buf   = RAAT__stream_format_compute_buffer_size(&pcmformat, self->pcm_samples_per_buf);

    buf    = RC__alloc(RC__ALLOCATOR_DEFAULT, bytes_per_buf);

    while (true) {
        int64_t now_raw_ns = RC__now_ns();
        int origformat_samples_to_zerofill = 0;

        int64_t delay_ns = RAAT__stream_format_samples_to_ns(&pcmformat, self->pcm_samples_per_buf);

        uv_mutex_lock(&self->lock);
        self->last_monotonic_sample_systime         =  now_raw_ns + delay_ns;
        self->last_monotonic_sample                 += origformat_samples_per_buf;

        int64_t now = RAAT__stream_format_samples_to_ns(&origformat, self->last_monotonic_sample);

        int correction_samples = 0; 

        if (threadstate->token == self->current_token) {
            if (stream != NULL) RAAT__stream_decref(stream);

            if (self->new_stream && self->stream) {
                int64_t start_time = self->start_time;
                if (now + ns_per_buf > start_time) {
                    RAAT__TRACE("[capture] starting playback: now (%lldns) + ns_per_buf(%lldns) = %lldns > %lldns streamsample=%lld", now, ns_per_buf, now+ns_per_buf, start_time, self->start_streamsample);
                    self->new_stream    = false;
                    stream              = self->stream;
                    streamsample        = self->start_streamsample;

                    if (now < start_time) {
                        int ns_to_fill      = start_time - now;
                        origformat_samples_to_zerofill = RAAT__stream_format_ns_to_samples(&origformat, ns_to_fill);
                    } if (now > start_time) {
                        streamsample += RAAT__stream_format_ns_to_samples(&origformat, now - start_time);
                    }
                } else {
                    RAAT__TRACE("[capture] waiting for start time...");
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
            if (origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) buf[i] = 0x69;
            else                                                 buf[i] = 0x00;
        }

        if (stream != NULL && self->resync_delay_remaining_ns <= 0) {
            //if (correction_samples != 0) RAAT__TRACE("[capture] applying %d sample correction", correction_samples);
            RAAT__read_stream_with_drift_correction(stream, streamsample, buf + bytes_to_zerofill, origformat_samples_per_buf - origformat_samples_to_zerofill, correction_samples, NULL, NULL);
            streamsample += origformat_samples_per_buf - origformat_samples_to_zerofill + correction_samples;
        }

        if (self->resync_delay_remaining_ns > 0) {
            self->resync_delay_remaining_ns -= ns_per_buf;
            if (self->resync_delay_remaining_ns <= 0) {
                self->setup_cb(self->setup_cb_userdata, RC__STATUS_SUCCESS, threadstate->token);
            }
        }

        uv_mutex_lock(&self->lock);
        if (self->outputfile) {
            size_t bytes_to_write = bytes_per_buf - bytes_to_zerofill;
            if (bytes_to_write)  {
                fwrite(buf+bytes_to_zerofill, bytes_to_write, 1, self->outputfile);
            }
        }
        uv_mutex_unlock(&self->lock);

        // for capture output, just sleep the time away
        int64_t after_raw_ns = RC__now_ns();
        int64_t target_time = now_raw_ns + ns_per_buf;
        int sleep_us = (int)((target_time - after_raw_ns) / 1000);
        //RAAT__TRACE("sleep %dus now %lld bufns %lld target %lld after_now %lld", sleep_us, now_raw_ns, (int64_t)ns_per_buf, target_time, after_raw_ns);
        RC__usleep(sleep_us);
    }

    RC__free(RC__ALLOCATOR_DEFAULT, buf);
    RC__free(RC__ALLOCATOR_DEFAULT, threadstate);
}

static void output_setup(void *vself, RAAT__StreamFormat *format, RAAT__OutputSetupCallback cb_setup, void *cb_setup_userdata, RAAT__OutputLostCallback cb_lost, void *cb_lost_userdata) {
    CaptureOutputPlugin *self = (CaptureOutputPlugin*)vself;
    RC__Status status = RC__STATUS_SUCCESS;
    int        token = RAAT__OUTPUT_TOKEN_INVALID;

    RAAT__OutputLostCallback  old_cb_lost          = NULL;
    void                     *old_cb_lost_userdata = NULL;
    bool                      needs_join           = false;

    char formatstr[RAAT__STREAM_FORMAT_MAX_STRLEN];
    RAAT__stream_format_to_string(format, formatstr);

    uv_mutex_lock(&self->lock);
    // before we do anything else, kick off anyone currently using the device
    self->current_token = token = ++self->next_token;

    if (self->state != IDLE) {
        RAAT__TRACE("[capture] kicking off old output");
        needs_join             = true;
        self->state            = IDLE;
        close_file(self);
        old_cb_lost            = self->cb_lost;
        old_cb_lost_userdata   = self->cb_lost_userdata;
        self->cb_lost          = NULL;
        self->cb_lost_userdata = NULL;
    }
    uv_mutex_unlock(&self->lock);

    if (needs_join) {
        RAAT__TRACE("[capture] joining thread");
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
        RAAT__TRACE("capture output setup: lost the race");
        status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED;
        goto fail;
    }

    // clean up leftover stream 
    if (self->stream != NULL) {
        RAAT__stream_decref(self->stream);
        self->stream = NULL;
    }

    RAAT__TRACE("capture output setup: format is %s", formatstr);

    RAAT__StreamFormat *formats;
    size_t              nformats;
    status = output_get_supported_formats(self, RC__ALLOCATOR_DEFAULT, &nformats, &formats);
    if (RC__STATUS_IS_SUCCESS(status)) {
        bool found = false;
        size_t i;
                    
        for (i = 0; i < nformats; i++) {
            RAAT__stream_format_to_string(&formats[i], formatstr);
            if (RAAT__stream_format_equals(format, &formats[i])) {
                found = true; break;
            }
        }

        RAAT__StreamFormat pcmformat;
        if (format->sample_type == RAAT__SAMPLE_TYPE_DSD) {
            pcmformat.sample_type     = RAAT__SAMPLE_TYPE_PCM;
            pcmformat.sample_rate     = format->sample_rate / 16;
            pcmformat.bits_per_sample = 24;
            pcmformat.channels        = format->channels;
        } else {
            pcmformat = *format;
        }

        if (found) {
            RAAT__TRACE("[capture] opening %d/%d/%d", pcmformat.sample_rate, pcmformat.bits_per_sample, pcmformat.channels);
            // OK. we are initialized. Now do the non-capture stuff

            self->current_token = token = ++self->next_token;
            status                      = RC__STATUS_SUCCESS;
            self->origformat            = *format;
            self->pcmformat             = pcmformat;
            self->state                 = STOPPED;
            self->pcm_samples_per_buf   = pcmformat.sample_rate / 20;

            RAAT__drift_correction_init(&self->drift_correction, self->log, format);

            self->cb_lost               = cb_lost;
            self->cb_lost_userdata      = cb_lost_userdata;

            CaptureOutputThreadState *threadstate = RC__new0(RC__ALLOCATOR_DEFAULT, CaptureOutputThreadState, 1);
            threadstate->self  = self;
            threadstate->token = token;
            uv_thread_create(&self->tid, capture_output_thread, threadstate);

            update_signal_path(self);
        } else {
            status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED;
        }
    }

fail:
    uv_mutex_unlock(&self->lock);

    // mutexes might not be recursive, so the following happen outside of the lock.
    
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
    CaptureOutputPlugin *self = (CaptureOutputPlugin*)vself;
    RC__Status status = RC__STATUS_UNEXPECTED_ERROR;

    bool needs_join = false;

    uv_mutex_lock(&self->lock);
    if (self->current_token == token) {
        RAAT__TRACE("[capture] teardown");
        self->current_token = RAAT__OUTPUT_TOKEN_INVALID;
        self->state = IDLE;
        close_file(self);
        self->cb_lost = NULL;
        self->cb_lost_userdata = NULL;

        if (self->stream != NULL) {
            RAAT__stream_decref(self->stream);
            self->stream = NULL;
        }

        needs_join = true;

        status = RC__STATUS_SUCCESS;
    } else {
        status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_TOKEN;
    }
    uv_mutex_unlock(&self->lock);

    if (needs_join) {
        uv_thread_join(&self->tid);
    }

    return status;
}

static RC__Status output_stop(void *vself, int token) {
    CaptureOutputPlugin *self = (CaptureOutputPlugin*)vself;
    RC__Status status = RC__STATUS_UNEXPECTED_ERROR;

    uv_mutex_lock(&self->lock);
    if (self->current_token == token) {
        if (self->state == RUNNING) {
            self->state = STOPPED;
            close_file(self);
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
    CaptureOutputPlugin *self = (CaptureOutputPlugin*)vself;
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
            open_file(self);
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

static int64_t LOCKED_get_local_time(CaptureOutputPlugin *self) {
    int64_t now = RC__now_ns();
    int64_t last_wall_sample = self->last_monotonic_sample;
    return RAAT__stream_format_samples_to_ns(&self->origformat, last_wall_sample) + (now - self->last_monotonic_sample_systime);
}

static RC__Status output_get_local_time(void *vself, int token, int64_t *out_time) {
    CaptureOutputPlugin *self = (CaptureOutputPlugin*)vself;
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
    CaptureOutputPlugin *self = (CaptureOutputPlugin*)vself;
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
    CaptureOutputPlugin *self = (CaptureOutputPlugin*)vself;
    return RAAT__output_message_listeners_add(&self->message_listeners, cb, cb_userdata);
}

static RC__Status 
output_remove_message_listener(void *vself, RAAT__OutputMessageCallback cb, void *cb_userdata) {
    CaptureOutputPlugin *self = (CaptureOutputPlugin*)vself;
    return RAAT__output_message_listeners_remove(&self->message_listeners, cb, cb_userdata);
}

static RC__Status 
output_send_message(void *vself, json_t *message) {
    CaptureOutputPlugin *self = (CaptureOutputPlugin*)vself;
    char *s = json_dumps(message, 0);
    RAAT__TRACE("[capture] GOT MESSAGE %s", s);
    free(s);

    return RC__STATUS_SUCCESS;
}

static RC__Status 
output_set_software_volume(void *vself, RAAT__OutputSoftwareVolume *volume) {
    CaptureOutputPlugin *self = (CaptureOutputPlugin*)vself;
    uv_mutex_lock(&self->lock);
    self->volume          = volume;
    uv_mutex_unlock(&self->lock);
    return RC__STATUS_SUCCESS;
}

RC__Status 
RAAT__capture_output_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__OutputPlugin **out_output) {
    alloc = RC__allocator_default(alloc);
    CaptureOutputPlugin *self            = RC__new0(alloc, CaptureOutputPlugin, 1);
    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;

    // initialize the state that is associated with CaptureOutputPlugin 
    self->alloc                        = alloc;
    self->log                          = RAAT__device_get_log(device);
    self->current_token                = RAAT__OUTPUT_TOKEN_INVALID;
    self->next_token                   = 1;
    self->state                        = IDLE;

    RAAT__TRACE("[capture] initializing output");

    json_t *buffer_duration = json_object_get(config, "buffer_duration");
    if (buffer_duration) self->buffer_duration_secs = json_number_value(buffer_duration);
    if (self->buffer_duration_secs == 0) self->buffer_duration_secs = .040;
    RAAT__TRACE("[capture] preferred buffer duration=%fs", self->buffer_duration_secs);

    json_t *custom_signal_path = json_object_get(config, "signal_path");
    if (custom_signal_path) {
        self->custom_signal_path = json_copy(custom_signal_path);
    }

    json_t *resync_delay = json_object_get(config, "resync_delay");
    if (resync_delay) self->resync_delay_secs = json_number_value(resync_delay);
    if (self->resync_delay_secs == 0) self->resync_delay_secs = 0.1;
    RAAT__TRACE("[capture] resync delay=%fs", self->resync_delay_secs);

    json_t *max_dsd_rate = json_object_get(config, "max_dsd_rate");
    if (max_dsd_rate) self->max_dsd_rate = json_number_value(max_dsd_rate);
    if (self->max_dsd_rate == 0) self->max_dsd_rate = 256;
    RAAT__TRACE("[capture] max dsd rate=%d", self->max_dsd_rate);

    const char *dsd_mode_s     = json_string_value(json_object_get(config, "dsd_mode"));
    if      (dsd_mode_s && !strcmp(dsd_mode_s, "dop")) self->dsd_mode = DSD_MODE_DOP;
    else if (dsd_mode_s && !strcmp(dsd_mode_s, "dcs")) self->dsd_mode = DSD_MODE_DCS;
    else                                               self->dsd_mode = DSD_MODE_NONE;

    // set up vtable of plugin functions
    self->plugin.get_info                = output_get_info;
    self->plugin.get_supported_formats   = output_get_supported_formats;
    self->plugin.setup                   = output_setup;
    self->plugin.teardown                = output_teardown;
    self->plugin.start                   = output_start;
    self->plugin.get_local_time          = output_get_local_time;
    self->plugin.set_remote_time         = output_set_remote_time;
    self->plugin.stop                    = output_stop;
    self->plugin.force_teardown          = output_force_teardown;
    self->plugin.send_message            = output_send_message;
    self->plugin.add_message_listener    = output_add_message_listener;
    self->plugin.remove_message_listener = output_remove_message_listener;
    self->plugin.set_software_volume     = output_set_software_volume;

    RAAT__output_message_listeners_init(&self->message_listeners, self->alloc);

    self->info = json_object();
    json_object_set(self->info, "config", config);

    uv_mutex_init(&self->lock);
    RAAT__TRACE("[output/capture] initialized");
    *out_output = &self->plugin;
    return RC__STATUS_SUCCESS;
}

void
RAAT__capture_output_plugin_delete(RAAT__OutputPlugin *output) {
    CaptureOutputPlugin *self = (CaptureOutputPlugin*)output;

    uv_mutex_destroy(&self->lock);
    if (self->info)               json_decref(self->info);
    if (self->custom_signal_path) json_decref(self->custom_signal_path);
    RAAT__output_message_listeners_destroy(&self->message_listeners);
    RC__free(self->alloc, self);
}

