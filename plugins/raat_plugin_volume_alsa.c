//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#define _GNU_SOURCE

#include "raat_plugin_volume_alsa.h"
#include "rc_list.h"

#include <uv.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <math.h>
#include <sys/time.h>

#include <alsa/asoundlib.h>

#define RAAT__CURRENT_LOG self->log

/*
 * Volume Plugin
 */

typedef enum {
    NUMBER      = 0,
    DB          = 1,
} VolumeMode;

typedef struct {
    RAAT__VolumePlugin          plugin;          // must be first item in struct
    RC__Allocator              *alloc;
    RAAT__Log                  *log;
    RAAT__VolumeStateListeners  state_listeners;
    uv_mutex_t                  lock;
    json_t                     *info;
    json_t                     *config;
    bool                        mute;
    bool                        has_mute_control;
    bool                        events_active;
    double                      volume;
    double                      min_volume;
    double                      max_volume;

    // settings
    VolumeMode                  mode;
    bool                        has_db_min;
    double                      db_min;
    bool                        has_db_step;
    double                      db_step;
    bool                        has_db_max;
    double                      db_max;

    double                      db_min_for_number, db_max_for_number;

    uv_thread_t                 tid;            // thread for mixer events

    snd_mixer_t                *mixer;  
    snd_mixer_elem_t           *elem;
    snd_mixer_elem_t           *mute_elem;
} AlsaVolumePlugin;

typedef struct {
    RAAT__VolumeStateCallback    cb;
    void                        *userdata;
} AlsaVolumeListener;

static RC__Status init(AlsaVolumePlugin *self);

static void print_snd_error(AlsaVolumePlugin *self, int rc, const char *api) {
    if (rc == 0) return;
    RAAT__ERROR("error in %s: %s (%d)", api, snd_strerror(rc), rc);
}

static RC__Status volume_get_info(void *vself, json_t **out_info) {
    AlsaVolumePlugin *self = (AlsaVolumePlugin*)vself;

    RC__ASSERT(self);
    RC__ASSERT(out_info);

    json_incref(self->info);
    *out_info = self->info;

    return RC__STATUS_SUCCESS;
}

static RC__Status volume_add_state_listener(void *vself, RAAT__VolumeStateCallback cb, void *userdata) {
    AlsaVolumePlugin *self = (AlsaVolumePlugin*)vself;
    return RAAT__volume_state_listeners_add(&self->state_listeners, cb, userdata);
}

static RC__Status volume_remove_state_listener(void *vself, RAAT__VolumeStateCallback cb, void *userdata) {
    AlsaVolumePlugin *self = (AlsaVolumePlugin*)vself;
    return RAAT__volume_state_listeners_remove(&self->state_listeners, cb, userdata);
}

static void LOCKED_get_state(AlsaVolumePlugin *self, RAAT__VolumeState *out_state) {
    memset(out_state, 0, sizeof(RAAT__VolumeState));

    if (self->mode == NUMBER) {
        out_state->volume_type  = RAAT__VOLUME_TYPE_NUMBER;
        out_state->db_min_volume = self->db_min_for_number;
        out_state->db_max_volume = self->db_max_for_number;
        out_state->min_volume    = self->min_volume;
        out_state->max_volume    = self->max_volume;
        out_state->volume_value  = self->volume;
        out_state->mute_value    = self->mute;
        out_state->volume_step   = 0.0;
    } else if (self->mode == DB) {
        out_state->volume_type   = RAAT__VOLUME_TYPE_DB;
        out_state->db_min_volume = self->min_volume;
        out_state->db_max_volume = self->max_volume;
        out_state->volume_step   = self->db_step;
        out_state->min_volume    = self->min_volume;
        out_state->max_volume    = self->max_volume;
        out_state->volume_value  = self->volume;
        out_state->mute_value    = self->mute;
        if (out_state->volume_value < out_state->min_volume) out_state->volume_value = out_state->min_volume;
        if (out_state->volume_value > out_state->max_volume) out_state->volume_value = out_state->max_volume;
    } else {
        RC__ASSERT(0);
    }
}

static RC__Status volume_get_state(void *vself, RAAT__VolumeState *out_state) {
    AlsaVolumePlugin *self = (AlsaVolumePlugin*)vself;

    RC__ASSERT(self != NULL);
    RC__ASSERT(out_state != NULL);

    uv_mutex_lock(&self->lock);
    LOCKED_get_state(self, out_state);
    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

static inline bool use_linear_dB_scale(long dBmin, long dBmax) {
    return dBmax - dBmin <= 2400;
}

static double get_volume_number(AlsaVolumePlugin *self, snd_mixer_selem_channel_id_t channel) {
    long min, max, value;
    double normalized, min_norm;
    int err;

    err = snd_mixer_selem_get_playback_dB_range(self->elem, &min, &max);
    if (err == 0) {
        self->db_min_for_number = min/100.0;
        self->db_max_for_number = max/100.0;
    } else {
        self->db_min_for_number = 0;
        self->db_max_for_number = 0;
    }

    if (err < 0 || min >= max) {
        err = snd_mixer_selem_get_playback_volume_range(self->elem, &min, &max);
        if (err < 0 || min == max) return 0.0;
        err = snd_mixer_selem_get_playback_volume(self->elem, channel, &value);
        if (err < 0) return 0.0;
        return (value - min) / (double)(max - min);
    }

    err = snd_mixer_selem_get_playback_dB(self->elem, channel, &value);
    if (err < 0) return 0.0;

    if (use_linear_dB_scale(min, max))
        return (value - min) / (double)(max - min);

    normalized = exp10((value - max) / 6000.0);
    if (min != SND_CTL_TLV_DB_GAIN_MUTE) {
        min_norm = exp10((min - max) / 6000.0);
        normalized = (normalized - min_norm) / (1 - min_norm);
    }

    return normalized;
}

static double get_volume_db(AlsaVolumePlugin *self, snd_mixer_selem_channel_id_t channel) {
    long value;
    int err = snd_mixer_selem_get_playback_dB(self->elem, channel, &value);
    if (err < 0) {
        return self->min_volume;
    }
    return (double)value/100.0;
}

static int set_volume_number(AlsaVolumePlugin *self, double volume) {
    long min, max, value;
    double min_norm;
    int err;

    err = snd_mixer_selem_get_playback_dB_range(self->elem, &min, &max);
    if (err < 0 || min >= max) {
        err = snd_mixer_selem_get_playback_volume_range(self->elem, &min, &max);
        if (err < 0) return err;

        value = lrint(volume * (max - min)) + min;
        return snd_mixer_selem_set_playback_volume_all(self->elem, value);
    }

    if (use_linear_dB_scale(min, max)) {
        value = lrint(volume * (max - min)) + min;
        return snd_mixer_selem_set_playback_dB_all(self->elem, value, 0);
    }

    if (min != SND_CTL_TLV_DB_GAIN_MUTE) {
        min_norm = exp10((min - max) / 6000.0);
        volume = volume * (1 - min_norm) + min_norm;
    }
    value = lrint(6000.0 * log10(volume)) + max;
    return snd_mixer_selem_set_playback_dB_all(self->elem, value, 0);
}

static int set_volume_db(AlsaVolumePlugin *self, double volume) {
    RAAT__TRACE("set volume to %f", volume);
    return snd_mixer_selem_set_playback_dB_all(self->elem, (long)(volume*100), 0);
}

static RC__Status LOCKED_update_state_from_alsa(AlsaVolumePlugin *self) {
    RC__Status status = RC__STATUS_SUCCESS;
    int rc;

    //
    // Read initial state
    //
    if (self->mode == NUMBER) {
        double left_vol  = get_volume_number(self, SND_MIXER_SCHN_FRONT_LEFT);
        double right_vol = get_volume_number(self, SND_MIXER_SCHN_FRONT_RIGHT);
        double avg_vol   = (left_vol + right_vol) / 2;
        self->min_volume = 0;
        self->max_volume = 100;
        self->volume     = (avg_vol * 100.0);
    } else if (self->mode == DB) {
        double left_vol  = get_volume_db(self, SND_MIXER_SCHN_FRONT_LEFT);
        double right_vol = get_volume_db(self, SND_MIXER_SCHN_FRONT_RIGHT);
        double avg_vol   = (left_vol + right_vol) / 2;
        long min, max;
        rc = snd_mixer_selem_get_playback_dB_range(self->elem, &min, &max);
        if (0 != rc) { print_snd_error(self, rc, "snd_mixer_selem_get_playback_dB_range()"); status = RAAT__VOLUME_PLUGIN_STATUS_DEVICE_INIT_FAILED; goto fail; }
        self->min_volume = min/100.0;
        self->max_volume = max/100.0;
        if (self->has_db_min && self->min_volume < self->db_min) self->min_volume = self->db_min;
        if (self->has_db_max && self->max_volume > self->db_max) self->max_volume = self->db_max;
        self->volume     = avg_vol;
    } else {
        RC__ASSERT(0);
    }

    int switch_left, switch_right;

    if (self->mute_elem) {
        if (snd_mixer_selem_has_playback_switch(self->mute_elem)) {
            rc = snd_mixer_selem_get_playback_switch(self->mute_elem, SND_MIXER_SCHN_FRONT_LEFT, &switch_left);
            if (0 != rc) { print_snd_error(self, rc, "snd_mixer_selem_get_playback_switch(LEFT)"); status = RAAT__VOLUME_PLUGIN_STATUS_DEVICE_INIT_FAILED; goto fail; }
            rc = snd_mixer_selem_get_playback_switch(self->mute_elem, SND_MIXER_SCHN_FRONT_RIGHT, &switch_right);
            if (0 != rc) { print_snd_error(self, rc, "snd_mixer_selem_get_playback_switch(RIGHT)"); status = RAAT__VOLUME_PLUGIN_STATUS_DEVICE_INIT_FAILED; goto fail; }
            self->has_mute_control = true;
            self->mute       = !(switch_left || switch_right);

        } else {
            self->mute       = false;
        }

        // sync up left/right playback switch if they were different
        if (snd_mixer_selem_has_playback_switch(self->mute_elem)) {
            snd_mixer_selem_set_playback_switch_all(self->mute_elem, (int)!self->mute);
        }
    } else {
        self->mute       = false;
    }

    RAAT__TRACE("[volume/alsa] read values: min=%f, max=%f, value=%f, mute=%d", self->min_volume, self->max_volume, self->volume, self->mute);

fail: 
    return status;
}


static RC__Status volume_set_volume(void *vself, double volume_value) {
    AlsaVolumePlugin *self = (AlsaVolumePlugin*)vself;
    RAAT__VolumeState state = {0,};
    bool changed = false;
    bool is_retry = false;

retry:
    if (is_retry) {
        RC__Status status = init(self);
        if (!RC__STATUS_IS_SUCCESS(status)) return status;
    }

    uv_mutex_lock(&self->lock);
    if (self->volume != volume_value) {
        self->volume = volume_value;
        RAAT__TRACE("[volume/alsa] volume => %f", volume_value);
        int rc;

        if (self->mode == NUMBER) 
            rc = set_volume_number(self, (double)self->volume / (double)100.0);
        else
            rc = set_volume_db(self, (double)self->volume);

        if (0 != rc) {
            if (is_retry) {
                print_snd_error(self, rc, "snd_mixer_selem_set_playback_switch_all");
            } else {
                is_retry = true; 
                uv_mutex_unlock(&self->lock);
                goto retry;
            }
        }

        changed = true;
        LOCKED_get_state(self, &state);
    }
    uv_mutex_unlock(&self->lock);

    if (changed) RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);

    return RC__STATUS_SUCCESS;
}

static RC__Status volume_set_mute(void *vself, bool mute_value) {
    AlsaVolumePlugin *self = (AlsaVolumePlugin*)vself;
    RAAT__VolumeState state;
    bool changed = false;
    bool is_retry = false;

retry:
    if (is_retry) {
        RC__Status status = init(self);
        if (!RC__STATUS_IS_SUCCESS(status)) return status;
    }

    uv_mutex_lock(&self->lock);
    if (self->mute_elem) {
        if (self->mute != mute_value) {
            self->mute = mute_value;
            RAAT__TRACE("[volume/alsa] mute => %d", mute_value);
            int rc = snd_mixer_selem_set_playback_switch_all(self->mute_elem, (int)!self->mute);
            if (0 != rc) { 
                if (0 != rc) {
                    if (is_retry) {
                        print_snd_error(self, rc, "snd_mixer_selem_set_playback_switch_all");
                    } else {
                        is_retry = true; 
                        uv_mutex_unlock(&self->lock);
                        goto retry;
                    }
                }
            }
            changed = true;
            LOCKED_get_state(self, &state);
        }
    }
    uv_mutex_unlock(&self->lock);

    if (changed) RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);

    return RC__STATUS_SUCCESS;
}

static int mixer_element_callback(snd_mixer_elem_t *elem, unsigned int mask) {
    AlsaVolumePlugin *self = snd_mixer_elem_get_callback_private(elem);
    if (!self->events_active) return 0;
    RAAT__INFO("[volume/alsa] changed!");
    bool state_ok = false;

    uv_mutex_lock(&self->lock);
    RAAT__VolumeState state;
    if (RC__STATUS_IS_SUCCESS(LOCKED_update_state_from_alsa(self))) {
        LOCKED_get_state(self, &state);
        state_ok = true;
    }
    uv_mutex_unlock(&self->lock);

    if (state_ok) {
        RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);
    }
    return 0;
}

static void mixer_events_thread(void *state) {
    AlsaVolumePlugin *self = state;
    while (self->events_active) {
        snd_mixer_handle_events(self->mixer);
        snd_mixer_wait(self->mixer, 500);
    }
}

static RC__Status init(AlsaVolumePlugin *self) {
    RC__Status status = RC__STATUS_SUCCESS;
    int rc = 0;

    if (self->events_active) {
        self->events_active = false;
        uv_thread_join(&self->tid);
    }

    uv_mutex_lock(&self->lock);

    if (self->mixer) {
        snd_mixer_close(self->mixer);
        self->mixer     = NULL;
        self->elem      = NULL;
        self->mute_elem = NULL;
        RAAT__INFO("[volume/alsa] re-initializing");
    } else {
        RAAT__INFO("[volume/alsa] initializing");
    }

    //
    // Open the device and find the mixer element
    //
    const char *raw_unique_id = json_string_value(json_object_get(self->config, "device"));
    if (raw_unique_id == NULL) return RAAT__VOLUME_PLUGIN_STATUS_INVALID_CONFIG;

    // remove device spec
    char unique_id[1024];
    strcpy(unique_id, raw_unique_id);
    char *comma = strchr(unique_id, ',');
    if (comma) *comma = '\0';

    json_t *json_index          = json_object_get(self->config, "index");
    json_t *json_name           = json_object_get(self->config, "name");
    json_t *json_mode           = json_object_get(self->config, "mode");
    json_t *json_db_min         = json_object_get(self->config, "db_min");
    json_t *json_db_max         = json_object_get(self->config, "db_max");
    json_t *json_db_step        = json_object_get(self->config, "db_step");

    if (json_mode) {
        if (!strcmp(json_string_value(json_mode), "number")) {
            self->mode = NUMBER;
        } else if (!strcmp(json_string_value(json_mode), "db")) {
            self->mode = DB;
        } else {
            RAAT__TRACE("[volume/alsa] [%s] invalid value for 'mode': '%s'", unique_id, json_string_value(json_mode));
            return RAAT__VOLUME_PLUGIN_STATUS_INVALID_CONFIG;
        }
    }

    if (json_db_min) {
        self->has_db_min = true;
        self->db_min = json_number_value(json_db_min);
    }

    if (json_db_max) {
        self->has_db_max = true;
        self->db_max = json_number_value(json_db_max);
    }

    if (json_db_step) {
        self->has_db_step = true;
        self->db_step = json_number_value(json_db_step);
    }
                             RAAT__TRACE("[volume/alsa] [%s] Settings", unique_id);
    if (json_index)          RAAT__TRACE("[volume/alsa] [%s]     mixer index:    %d", unique_id, (int)json_integer_value(json_index));
    if (json_name)           RAAT__TRACE("[volume/alsa] [%s]     element name:   %s", unique_id, json_string_value(json_name));
    if (json_mode)           RAAT__TRACE("[volume/alsa] [%s]     mode:           %s", unique_id, json_string_value(json_mode));
    if (json_db_min)         RAAT__TRACE("[volume/alsa] [%s]     db_min:         %d", unique_id, json_number_value(json_db_min));
    if (json_db_max)         RAAT__TRACE("[volume/alsa] [%s]     db_max:         %d", unique_id, json_number_value(json_db_max));
    if (json_db_step)        RAAT__TRACE("[volume/alsa] [%s]     db_step:        %d", unique_id, json_number_value(json_db_step));

    rc = snd_mixer_open(&self->mixer, 0);
    if (0 != rc) { print_snd_error(self, rc, "snd_mixer_open"); status = RAAT__VOLUME_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail; }

    rc = snd_mixer_attach(self->mixer, unique_id);
    if (0 != rc) { print_snd_error(self, rc, "snd_mixer_attach"); status = RAAT__VOLUME_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail; }

    rc = snd_mixer_selem_register(self->mixer, NULL, NULL);
    if (0 != rc) { print_snd_error(self, rc, "snd_mixer_selem_register"); status = RAAT__VOLUME_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail; }

    rc = snd_mixer_load(self->mixer);
    if (0 != rc) { print_snd_error(self, rc, "snd_mixer_load"); status = RAAT__VOLUME_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail; }

    snd_mixer_selem_id_t *sid;
    snd_mixer_selem_id_alloca(&sid);

    int mixer_index = 0;
    const char *mixer_name = NULL;


    /*
     * Probe for known/important models and tweak their settings
     */
    bool is_pcm512x = false;
    {
        snd_ctl_t               *ctl;
        snd_ctl_card_info_t     *info;    snd_ctl_card_info_alloca(&info);
        snd_pcm_info_t          *pcminfo; snd_pcm_info_alloca(&pcminfo);
        int rc = snd_ctl_open(&ctl, unique_id, 0);
        if (!rc) {
            if ((rc = snd_ctl_card_info(ctl, info)) < 0) goto cancel_model_probe;
            int dev = -1;
            while (1) {
                if (snd_ctl_pcm_next_device(ctl, &dev) < 0) goto cancel_model_probe;
                if (dev < 0) break;
                snd_pcm_info_set_device(pcminfo, dev);
                snd_pcm_info_set_subdevice(pcminfo, 0);
                snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_PLAYBACK);
                if ((rc = snd_ctl_pcm_info(ctl, pcminfo)) < 0) continue;
                const char *id = snd_pcm_info_get_id(pcminfo);
                if (strstr(id, "pcm512x")) {
                    RAAT__TRACE("[volume/alsa] model probe found pcm512x");
                    is_pcm512x = true;
                }
            }
        }
cancel_model_probe:
        snd_ctl_close(ctl);
    } 

    if (json_index && json_name) {
        // look up a specific mixer element
        mixer_index = json_integer_value(json_index);
        mixer_name = json_string_value(json_name);
        snd_mixer_selem_id_set_index(sid, mixer_index);
        snd_mixer_selem_id_set_name(sid, mixer_name);
        self->elem      = snd_mixer_find_selem(self->mixer, sid);
        self->mute_elem = self->elem;
        if (!self->elem) { RAAT__ERROR("[volume/alsa] couldn't find mixer element"); status = RAAT__VOLUME_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail; }

    } else if (is_pcm512x) {
        /*
         * For HiFiBerry, IQAudIO, use the "Digital" control for mute/volume.
         *
         * alsamixer for these devices is a war zone of confusing decisions. We can't find it automatically.
         */
        snd_mixer_selem_id_set_index(sid, 0);
        snd_mixer_selem_id_set_name(sid, "Digital");
        self->elem      = snd_mixer_find_selem(self->mixer, sid);
        self->mute_elem = self->elem;
        if (!self->elem) { RAAT__ERROR("[volume/alsa] couldn't find mixer element"); status = RAAT__VOLUME_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail; }

    } else {
        RAAT__TRACE("[volume/alsa] searching for volume control element");
        snd_mixer_elem_t *elem = NULL;

        for (elem = snd_mixer_first_elem(self->mixer); elem; elem = snd_mixer_elem_next(elem)) {
            RAAT__TRACE("[volume/alsa]     card has element %d, %s", snd_mixer_selem_get_index(elem), snd_mixer_selem_get_name(elem));

            if (json_index) {
                int mixer_index = json_integer_value(json_index);
                if (mixer_index != snd_mixer_selem_get_index(elem))  {
                    RAAT__TRACE("[volume/alsa]         (skipping: element doesn't match 'index' from JSON configuration)");
                    continue;
                }
            }

            if (!snd_mixer_selem_has_playback_volume(elem)) {
                RAAT__TRACE("[volume/alsa]         (skipping: element doesn't support playback volume)");
                continue;
            }


            if (self->elem) {
                RAAT__TRACE("[volume/alsa]         (skipping: already chose element)");
                continue;
            }

            if (!self->elem) {
                RAAT__TRACE("[volume/alsa]         (using this mixer element)");
                self->elem = elem;
            }

            self->elem = elem;
        }
        if (!self->elem) { RAAT__ERROR("[volume/alsa] couldn't find mixer element"); status = RAAT__VOLUME_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail; }

        RAAT__TRACE("[volume/alsa] searching for mute element");
        snd_mixer_elem_t *mute_elem = NULL;

        for (mute_elem = snd_mixer_first_elem(self->mixer); mute_elem; mute_elem = snd_mixer_elem_next(mute_elem)) {
            RAAT__TRACE("[volume/alsa]     card has element %d, %s", snd_mixer_selem_get_index(mute_elem), snd_mixer_selem_get_name(mute_elem));

            if (json_index) {
                int mixer_index = json_integer_value(json_index);
                if (mixer_index != snd_mixer_selem_get_index(mute_elem))  {
                    RAAT__TRACE("[volume/alsa]         (skipping: element doesn't match 'index' from JSON configuration)");
                    continue;
                }
            }

            if (!snd_mixer_selem_has_playback_switch(mute_elem)) {
                RAAT__TRACE("[volume/alsa]         (skipping: element doesn't support playback switch)");
                continue;
            }


            if (self->mute_elem) {
                RAAT__TRACE("[volume/alsa]         (skipping: already chose element)");
                continue;
            }

            if (!self->mute_elem) {
                RAAT__TRACE("[volume/alsa]         (using this mixer element)");
                self->mute_elem = mute_elem;
            }

            self->mute_elem = mute_elem;
        }
    }

    if (self->mode == DB) {
        long min,max;
        if (snd_mixer_selem_get_playback_dB_range(self->elem, &min, &max)) {
            RAAT__WARNING("[volume/alsa] switching to number mode because we snd_mixer_selem_get_playback_dB_range failed");
            self->mode = NUMBER;
        }
    }

    // set up events
    snd_mixer_elem_set_callback_private(self->elem, self);
    snd_mixer_elem_set_callback(self->elem, mixer_element_callback);

    if (self->mute_elem) {
        snd_mixer_elem_set_callback_private(self->mute_elem, self);
        snd_mixer_elem_set_callback(self->mute_elem, mixer_element_callback);
    }

    status = LOCKED_update_state_from_alsa(self);
    if (!RC__STATUS_IS_SUCCESS(status)) goto fail;

    RAAT__TRACE("[volume/alsa] initialized min_volume=%d max_volume=%d volume=%d mute=%d", self->min_volume, self->max_volume, self->volume, self->mute);

    // broadcast state in case this is a re-init
    RAAT__VolumeState state;
    LOCKED_get_state(self, &state);
    RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);

fail:
    if (RC__STATUS_IS_SUCCESS(status)) {
        self->events_active = true;
        uv_thread_create(&self->tid, mixer_events_thread, self);
    }
    uv_mutex_unlock(&self->lock);

    return status;
}

RC__Status 
RAAT__alsa_volume_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__VolumePlugin **out_volume) { 
    RC__Status status = RC__STATUS_UNEXPECTED_ERROR;

    alloc = RC__allocator_default(alloc);
    AlsaVolumePlugin *self            = RC__new0(alloc, AlsaVolumePlugin, 1);
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

    self->config = json_deep_copy(config);

    self->info = json_object();
    json_object_set(self->info, "config", config);

    if (RC__STATUS_IS_SUCCESS(init(self))) {
        *out_volume = &self->plugin;
        return RC__STATUS_SUCCESS;
    } else {
        RAAT__alsa_volume_plugin_delete(&self->plugin);
        return status;
    }
}

void
RAAT__alsa_volume_plugin_delete(RAAT__VolumePlugin *volume) {
    AlsaVolumePlugin *self = (AlsaVolumePlugin*)volume;

    if (self->events_active) {
        self->events_active = false;
        uv_thread_join(&self->tid);
    }

    if (self->mixer) snd_mixer_close(self->mixer);
    uv_mutex_destroy(&self->lock);
    if (self->info) json_decref(self->info);
    json_decref(self->config);
    RAAT__volume_state_listeners_destroy(&self->state_listeners);

    if (self->events_active) {
        self->events_active = false;
        uv_thread_join(&self->tid);
    }

    RC__free(self->alloc, self);
}

