//#define SUPPORTS_DSD

//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_plugin_volume_software.h"
#include "rc_list.h"
#include "raat_device.h"
#include "raat_plugin_output.h"
#include "raat_stream.h"
#include "raat_dsp.h"

#include <uv.h>
#include <string.h>

// for prototyping DSD gain
#ifdef SUPPORTS_DSD
#include "raat_dsd.h"
#endif

#define RAAT__CURRENT_LOG self->log

/*
 * Volume Plugin
 */
typedef struct {
    RAAT__VolumePlugin           plugin;          // must be first item in struct
    RC__Allocator               *alloc;
    RAAT__Log                   *log;
    RAAT__VolumeStateListeners   state_listeners;
    uv_mutex_t                   lock;
    bool                         mute;
    int                          volume;
    int                          max_attenuation;
    json_t                      *info;
    RAAT__StreamFormat           format;
    RAAT__Device                *device;
#ifdef SUPPORTS_DSD
    RAAT__DsdGain                dsdgain;
#endif

    RAAT__OutputSoftwareVolume   output_volume;
} SoftwareVolumePlugin;

static RC__Status volume_get_info(void *vself, json_t **out_info) {
    SoftwareVolumePlugin *self = (SoftwareVolumePlugin*)vself;

    RC__ASSERT(self);
    RC__ASSERT(out_info);

    json_incref(self->info);
    *out_info = self->info;

    return RC__STATUS_SUCCESS;
}

static RC__Status volume_add_state_listener(void *vself, RAAT__VolumeStateCallback cb, void *userdata) {
    SoftwareVolumePlugin *self = (SoftwareVolumePlugin*)vself;
    return RAAT__volume_state_listeners_add(&self->state_listeners, cb, userdata);
}

static RC__Status volume_remove_state_listener(void *vself, RAAT__VolumeStateCallback cb, void *userdata) {
    SoftwareVolumePlugin *self = (SoftwareVolumePlugin*)vself;
    return RAAT__volume_state_listeners_remove(&self->state_listeners, cb, userdata);
}

static void LOCKED_get_state(SoftwareVolumePlugin *self, RAAT__VolumeState *out_state) {
    out_state->volume_type     = RAAT__VOLUME_TYPE_DB;
    out_state->min_volume      = self->max_attenuation;
    out_state->max_volume      = 0;
    out_state->volume_value    = self->volume;
    out_state->mute_value      = self->mute;
    out_state->db_min_volume   = self->max_attenuation;
    out_state->db_max_volume   = 0;
    out_state->volume_step     = 1.0;
}

static RC__Status volume_get_state(void *vself, RAAT__VolumeState *out_state) {
    SoftwareVolumePlugin *self = (SoftwareVolumePlugin*)vself;

    RC__ASSERT(self != NULL);
    RC__ASSERT(out_state != NULL);

    uv_mutex_lock(&self->lock);
    LOCKED_get_state(self, out_state);
    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

static void LOCKED_update_signal_path(SoftwareVolumePlugin *self) {
    RAAT__OutputPlugin *output = RAAT__device_get_output_plugin(self->device);
    if (output->set_software_volume_signal_path) {
        json_t *path = json_array();

        path = json_array();
        json_t *elem = json_object();
        json_object_set_new(elem, "type",           json_string("digital_volume"));
        json_object_set_new(elem, "gain",           json_real(self->volume));
        json_object_set_new(elem, "is_muted",       self->mute ? json_true() : json_false());

        if (self->volume == 0)
            json_object_set_new(elem, "quality",        json_string("lossless"));
        else
            json_object_set_new(elem, "quality",        json_string("high"));

        json_object_set_new(elem, "is_passthrough", self->volume == 0 ? json_true() : json_false());
        json_array_append_new(path, elem);

        output->set_software_volume_signal_path(output, path);
        if (path)
            json_decref(path);
    }
}

static RC__Status volume_set_volume(void *vself, double volume_value) {
    SoftwareVolumePlugin *self = (SoftwareVolumePlugin*)vself;
    RAAT__VolumeState state;
    bool changed = false;

    uv_mutex_lock(&self->lock);
    if (self->volume != (int)volume_value) {
        self->volume = (int)volume_value;
        RAAT__TRACE("[volume/software] volume => %d", (int)volume_value);
        changed = true;
        LOCKED_get_state(self, &state);
        LOCKED_update_signal_path(self);
    }
    uv_mutex_unlock(&self->lock);

    if (changed) RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);

    return RC__STATUS_SUCCESS;
}

static RC__Status volume_set_mute(void *vself, bool mute_value) {
    SoftwareVolumePlugin *self = (SoftwareVolumePlugin*)vself;
    RAAT__VolumeState state;
    bool changed = false;

    uv_mutex_lock(&self->lock);
    if (self->mute != mute_value) {
        self->mute = mute_value;
        RAAT__TRACE("[volume/software] mute => %d", mute_value);
        changed = true;
        LOCKED_get_state(self, &state);
        LOCKED_update_signal_path(self);
    }
    uv_mutex_unlock(&self->lock);
    
    if (changed) RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);

    return RC__STATUS_SUCCESS;
}

static RC__Status softvol_supports_format(void *userdata, RAAT__StreamFormat *format) {
#ifdef SUPPORTS_DSD
    //SoftwareVolumePlugin *self = userdata;
    return RC__STATUS_SUCCESS;
#else
    if (format->sample_type == RAAT__SAMPLE_TYPE_PCM && (format->bits_per_sample == 16 || format->bits_per_sample == 24 || format->bits_per_sample == 32))
        return RC__STATUS_SUCCESS;
    else
        return RAAT__VOLUME_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED;
#endif
}

static RC__Status softvol_setup(void *userdata, RAAT__StreamFormat *format, int *out_delay) {
    SoftwareVolumePlugin *self = userdata;

#ifndef SUPPORTS_DSD
    if (format->sample_type == RAAT__SAMPLE_TYPE_DSD) {
        return RAAT__VOLUME_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED;
    }
#endif

    uv_mutex_lock(&self->lock);
    self->format    = *format;
    RAAT__TRACE("[volume/software] init bitspersample=%d", self->format.bits_per_sample);
    
    LOCKED_update_signal_path(self);

#ifdef SUPPORTS_DSD
    if (format->sample_type == RAAT__SAMPLE_TYPE_DSD) {
        RAAT__dsd_gain_init(&self->dsdgain, format->sample_rate, format->channels);
    }
#endif

    if (out_delay) *out_delay = 0;
    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

static RC__Status softvol_process(void *userdata, double extra_db, uint8_t *data, int nsamples) {
    SoftwareVolumePlugin *self = userdata;

    double gaindb = extra_db + self->volume;
    double gain   = RAAT__db_attenuation_to_linear_gain(gaindb);

    if (self->mute) {
        memset(data, 0, RAAT__stream_format_compute_buffer_size(&self->format, nsamples));
    } else {
        //RAAT__TRACE("[volume/software] process %d samples bps=%d channels=%d gaindb=%f gain=%f", (int)nsamples, (int)self->format.bits_per_sample, self->format.channels, gaindb, gain);
        int64_t before = RC__now_ns();
        if (self->format.sample_type == RAAT__SAMPLE_TYPE_DSD) {
#ifdef SUPPORTS_DSD
            RAAT__dsd_gain_process(&self->dsdgain, data, gain, nsamples);
#endif
        } else if (self->format.bits_per_sample == 16) {
            RAAT__pcm_gain_16(data, gain, nsamples * self->format.channels);
        } else if (self->format.bits_per_sample == 24) {
            RAAT__pcm_gain_24(data, gain, nsamples * self->format.channels);
        } else if (self->format.bits_per_sample == 32) {
            RAAT__pcm_gain_32(data, gain, nsamples * self->format.channels);
        }
        int64_t after = RC__now_ns();
        double walltime = (after-before)/1000000.0;
        double sampletime = 1000.0 * (double)nsamples / self->format.sample_rate;
        if (self->format.sample_type == RAAT__SAMPLE_TYPE_DSD) {
            RAAT__TRACE("[volume/software] [DSD] processed %fms in %fms (%f%% CPU)", sampletime, walltime, walltime/sampletime*100);
        }
    }
    
    return RC__STATUS_SUCCESS;
}

static RC__Status softvol_teardown(void *userdata) {
    SoftwareVolumePlugin *self = userdata;

    uv_mutex_lock(&self->lock);
    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

RC__Status 
RAAT__software_volume_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__VolumePlugin **out_volume) { 
    RAAT__OutputPlugin *output = RAAT__device_get_output_plugin(device);
    if (output == NULL) {
        return RAAT__VOLUME_PLUGIN_STATUS_OUTPUT_PLUGIN_NOT_FOUND;
    }
    if (!output->set_software_volume) {
        return RAAT__VOLUME_PLUGIN_STATUS_OUTPUT_PLUGIN_NOT_SUPPORTED;
    }

    alloc = RC__allocator_default(alloc);
    SoftwareVolumePlugin *self            = RC__new0(alloc, SoftwareVolumePlugin, 1);

    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;
    self->alloc                        = alloc;
    self->device                       = device;
    self->log                          = RAAT__device_get_log(device);
    self->plugin.get_info              = volume_get_info;
    self->plugin.add_state_listener    = volume_add_state_listener;
    self->plugin.remove_state_listener = volume_remove_state_listener;
    self->plugin.get_state             = volume_get_state;
    self->plugin.set_volume            = volume_set_volume;
    self->plugin.set_mute              = volume_set_mute;

    self->output_volume.userdata = self;
    self->output_volume.supports_format = softvol_supports_format;
    self->output_volume.setup           = softvol_setup;
    self->output_volume.process         = softvol_process;
    self->output_volume.teardown        = softvol_teardown;

    RC__Status status = output->set_software_volume(output, &self->output_volume);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        RC__free(alloc, self);
        RAAT__ERROR("[volume/software] failed to set_software_volume on output plugin: %s", RC__status_to_string(status));
        return status;
    }

    self->info = json_object();
    json_object_set(self->info, "config", config);

    json_object_set_new(self->info, "requires_external_persistence", json_true()); // tells Roon that this plugin doesn't persist state internally
    json_object_set_new(self->info, "supports_dsd", json_false());                 // tells Roon that this plugin can not perform gain adjustment on DSD content

    self->mute     = false;
    self->volume   = 0;
    uv_mutex_init(&self->lock);
    RAAT__volume_state_listeners_init(&self->state_listeners, self->alloc);

    json_t *max_attenuation = json_object_get(config, "max_attenuation");
    if (max_attenuation) self->max_attenuation= json_number_value(max_attenuation);
    if (self->max_attenuation == 0) self->max_attenuation= -80;
    RAAT__TRACE("[volume/software] preferred max attenuation=%ds", self->max_attenuation);
    
    RAAT__TRACE("[volume/software] initialized");

    *out_volume = &self->plugin;
    return RC__STATUS_SUCCESS;
}

void
RAAT__software_volume_plugin_delete(RAAT__VolumePlugin *volume) {
    SoftwareVolumePlugin *self = (SoftwareVolumePlugin*)volume;
    uv_mutex_destroy(&self->lock);
    if (self->info) json_decref(self->info);
    RAAT__volume_state_listeners_destroy(&self->state_listeners);
    RC__free(self->alloc, self);
}

