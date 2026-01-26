//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_plugin_volume_coreaudio.h"
#include "rc_list.h"

#include <uv.h>
#include <unistd.h>

#include <mach/mach_time.h>
#include <libkern/OSAtomic.h>

#include <CoreServices/CoreServices.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreAudio/AudioHardware.h>
#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>

#define RAAT__CURRENT_LOG self->log

#define SHARED_VOLUME_RANGE_DB 60

typedef enum {
    VOLUME_MODE_NORMAL      = 0,
    VOLUME_MODE_DB          = 1,
} volume_mode_t;

/*
 * Volume Plugin
 */
typedef struct {
    RAAT__VolumePlugin           plugin;          // must be first item in struct
    RC__Allocator               *alloc;
    RAAT__Log                   *log;
    RAAT__Device                *device;
    RAAT__VolumeStateListeners   state_listeners;
    uv_mutex_t                   lock;
    int                          mute;
    bool                         exclusive_mode;
    AudioDeviceID                device_id;
    char                        *unique_id;
    json_t                      *info;

    Float32                      volume;
    Float32                      actual_volume;

    double                       db_min_volume;
    double                       db_max_volume;

    volume_mode_t                volume_mode;
    RAAT__OutputPlugin          *output_plugin;
} CoreAudioVolumePlugin;

/*
 * In shared mode, we apply volume control at the HAL. This requires cooperation from the output plugin.
 *
 * We don't publicly prototype this function, since it's just a friend-level interface between these two plugins
 */
extern void RAAT__coreaudio_output_set_shared_volume(RAAT__OutputPlugin *output, float volume, bool mute);

/*
 * CoreAudio is the worst. It can't accurately round-trip a volume value--meaning, that after SetVolume(X), GetVolume() might not
 * equal X. Instead it's a rounded/truncated version of X.
 *
 * This causes problems, since we can't use CoreAudio to keep track of the volume state. The typical symptom is volume values 
 * "creeping" downwards when the app restarts, or devices refuse to take on certain values.
 *
 * The strategy here is as follows: whenever Roon sets the volume to X, we note what X was and what CoreAudio actually persisted in a 
 * file in ~/Library/Caches.
 *
 * When we later read back the volume from CoreAudio, we check the file. If the CoreAudio volume hasn't actually changed, then we ignore
 * it and act as if it is still the last set value. Otherwise, we convert back into "roon-space". 
 *
 */
bool load_cached_data(CoreAudioVolumePlugin *self) {
    char *home = getenv("HOME");
    if (!home) return false;
    char path[32768];
    if (self->exclusive_mode) {
        snprintf(path, sizeof(path), "%s/Library/Caches/com.roonlabs.raat/%s.exclusivemode.raatcachev2", home, self->unique_id ? self->unique_id : "default");
    } else {
        snprintf(path, sizeof(path), "%s/Library/Caches/com.roonlabs.raat/%s.raatcachev2", home, self->unique_id ? self->unique_id : "default");
    }
    FILE *f = fopen(path, "r");
    bool ret = false;
    if (f) {
        ret = fscanf(f, "%f,%f,%d", &self->volume, &self->actual_volume, &self->mute) == 3;
        if (self->volume_mode == VOLUME_MODE_DB) {
            if (self->volume > self->db_min_volume) { self->volume = self->db_min_volume; self->actual_volume = -1; }
            if (self->volume < self->db_max_volume) { self->volume = self->db_max_volume; self->actual_volume = -1; }
        } else {
            // if volume is messed up, default to 100
            if (self->volume > 100) { self->volume = 100; self->actual_volume = -1; }
            if (self->volume <   0) { self->volume = 100; self->actual_volume = -1; }
        }
        fclose(f);
    }
    return ret;
}

void save_cached_data(CoreAudioVolumePlugin *self) {
    char *home = getenv("HOME");
    if (!home) return;
    char path[32768], data[32768];
    snprintf(path, sizeof(path), "%s/Library/Caches/com.roonlabs.raat", home); 
    mkdir(path, 0755);
    if (self->exclusive_mode) {
        snprintf(path, sizeof(path), "%s/Library/Caches/com.roonlabs.raat/%s.exclusivemode.raatcachev2", home, self->unique_id ? self->unique_id : "default");
    } else {
        snprintf(path, sizeof(path), "%s/Library/Caches/com.roonlabs.raat/%s.raatcachev2", home, self->unique_id ? self->unique_id : "default");
    }
    snprintf(data, sizeof(data), "%f,%f,%d", self->volume, self->actual_volume, self->mute);
    RAAT__TRACE("[coreaudio/volume] saved cache roon_volume=%d actual_volume=%f mute=%d", self->volume, self->actual_volume, self->mute);
    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(data, strlen(data), 1, f);
        fclose(f);
    }
}

static RC__Status volume_get_info(void *vself, json_t **out_info) {
    CoreAudioVolumePlugin *self = (CoreAudioVolumePlugin*)vself;

    RC__ASSERT(self);
    RC__ASSERT(out_info);

    json_incref(self->info);
    *out_info = self->info;

    return RC__STATUS_SUCCESS;
}

static RC__Status volume_add_state_listener(void *vself, RAAT__VolumeStateCallback cb, void *userdata) {
    CoreAudioVolumePlugin *self = (CoreAudioVolumePlugin*)vself;
    return RAAT__volume_state_listeners_add(&self->state_listeners, cb, userdata);
}

static RC__Status volume_remove_state_listener(void *vself, RAAT__VolumeStateCallback cb, void *userdata) {
    CoreAudioVolumePlugin *self = (CoreAudioVolumePlugin*)vself;
    return RAAT__volume_state_listeners_remove(&self->state_listeners, cb, userdata);
}

static void LOCKED_get_state(CoreAudioVolumePlugin *self, RAAT__VolumeState *out_state) {
    memset(out_state, 0, sizeof(RAAT__VolumeState));
    if (self->volume_mode == VOLUME_MODE_DB) {
        out_state->volume_type   = RAAT__VOLUME_TYPE_DB;
        out_state->min_volume    = self->db_min_volume;
        out_state->max_volume    = self->db_max_volume;
        out_state->volume_step   = 0.5;
    } else {
        out_state->volume_type   = RAAT__VOLUME_TYPE_NUMBER;
        out_state->min_volume    = 0;
        out_state->max_volume    = 100;
        out_state->volume_step   = 1.0;
    }
    out_state->volume_value  = self->volume;
    out_state->mute_value    = self->mute;
    out_state->db_min_volume = self->db_min_volume;
    out_state->db_max_volume = self->db_max_volume;
}

static RC__Status volume_get_state(void *vself, RAAT__VolumeState *out_state) {
    CoreAudioVolumePlugin *self = (CoreAudioVolumePlugin*)vself;

    RC__ASSERT(self != NULL);
    RC__ASSERT(out_state != NULL);

    uv_mutex_lock(&self->lock);
    LOCKED_get_state(self, out_state);
    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

static void printstatus(CoreAudioVolumePlugin *self, const char *str, OSStatus err) {
    if (err != noErr) { 
        int ierr = err; 
        char *cerr = (char*)&ierr; 
        if (isprint(cerr[0]) && isprint(cerr[1]) && isprint(cerr[2]) && isprint(cerr[3])) 
            RAAT__WARNING("[volume/coreaudio] Error in %s: %d (0x%x) (%c%c%c%c)", str, (int)err, (int)err, cerr[3], cerr[2], cerr[1], cerr[0]); 
        else  
            RAAT__WARNING("[volume/coreaudio] Error in %s: %d (0x%x)", str, (int)err, (int)err); 
    } 
}

static float 
volume_to_gain(int volume) {
    return volume / 100.0;
}

static int 
gain_to_volume(float volume) {
    int result = (int) (round(volume * 100));
    if (result > 100) result = 100;
    if (result < 0)   result = 0;
    return result;
}

static bool load_coreaudio_state(CoreAudioVolumePlugin *self) {
    UInt32 size;
    Float32 vol;
    int mute;
    OSStatus err;

    bool volume_supported = false;

    bool changed = false;
    RAAT__VolumeState state;

    uv_mutex_lock(&self->lock);

    if (self->exclusive_mode) {
        if (self->volume_mode == VOLUME_MODE_DB) {
            size = sizeof(vol);
            err  = AudioDeviceGetProperty(self->device_id, 0, 0, kAudioDevicePropertyVolumeDecibels, &size, &vol);
            if (!err) {
                if (self->actual_volume != vol) {
                    self->actual_volume = vol;
                    self->volume        = vol;
                    RAAT__TRACE("[coreaudio/volume] load coreaudio volume actual=%f roon=%d", self->actual_volume, self->volume);
                    changed = true;
                }
                volume_supported = true;
            } else {
                printstatus(self, "Failed to Get kAudioDevicePropertyVolumeDecibels", err);
            }
        } else {
            size = sizeof(vol);
            err  = AudioDeviceGetProperty(self->device_id, 0, 0, kAudioDevicePropertyVolumeScalar, &size, &vol);
            if (err == kAudioHardwareUnknownPropertyError) {
                RAAT__TRACE("[volume/coreaudio] AudioDeviceGetProperty(kAudioDevicePropertyVolumeScalar) failed. Falling back to using kAudioHardwareServiceDeviceProperty_VirtualMasterVolume");
                size = sizeof(vol);
                err  = AudioDeviceGetProperty(self->device_id, 0, 0, kAudioHardwareServiceDeviceProperty_VirtualMasterVolume, &size, &vol);
                printstatus(self, "AudioDevicegetProperty(kAudioHardwareServiceDeviceProperty_VirtualMasterVolume)", err);
                if (!err) {
                    if (self->actual_volume != vol) {
                        self->actual_volume = vol;
                        self->volume        = gain_to_volume(vol);
                        RAAT__TRACE("[coreaudio/volume] loaded coreaudio volume actual=%f roon=%d", self->actual_volume, self->volume);
                        changed = true;
                    }
                    volume_supported = true;
                }
            } else if (!err) {
                if (self->actual_volume != vol) {
                    self->actual_volume = vol;
                    self->volume        = gain_to_volume(vol);
                    RAAT__TRACE("[coreaudio/volume] load coreaudio volume actual=%f roon=%d", self->actual_volume, self->volume);
                    changed = true;
                }
                volume_supported = true;
            } else {
                printstatus(self, "Failed to Get kAudioDevicePropertyVolumeScalar", err);
            }
        }

        size = sizeof(mute);
        err = AudioDeviceGetProperty(self->device_id, 0, 0, kAudioDevicePropertyMute, &size, &mute);
        printstatus(self, "AudioDevicegetProperty(kAudioDevicePropertyMute)", err);
        if (!err) {
            if (self->mute != mute) {
                self->mute = mute;
                changed = true;
                LOCKED_get_state(self, &state);
            }
        }
    } else {
        volume_supported = true;
    }

    if (changed) {
        LOCKED_get_state(self, &state);
        save_cached_data(self);
    }

    uv_mutex_unlock(&self->lock);

    if (changed) RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);

    return volume_supported;
}

static void LOCKED_update_coreaudio(CoreAudioVolumePlugin *self) {
    OSStatus err;

    if (self->exclusive_mode) {
        int mute = self->mute ? 1 : 0;

        err = AudioDeviceSetProperty(self->device_id, NULL, 0, 0, kAudioDevicePropertyMute, sizeof(mute), &mute);
        printstatus(self, "AudioDeviceSetProperty(kAudioDevicePropertyMute)", err);
        //if (!err) { RAAT__TRACE("Set hardware mute successfully <= %d", mute); }

        UInt32 mute_size = sizeof(self->mute);
        err = AudioDeviceGetProperty(self->device_id, 0, 0, kAudioDevicePropertyMute, &mute_size, &self->mute);
        printstatus(self, "AudioDeviceGetProperty(kAudioDevicePropertyMute)", err);
        //if (!err){ RAAT__TRACE("Got hardware mute successfully => %d", self->mute); }

        if (mute) return;       


        if (self->volume_mode == VOLUME_MODE_DB) {
            Float32 vol = self->volume;
            // in exclusive mode, set the device volume. This will be applied in hardware.
            //RAAT__DEBUG("set exclusive volume to %f", (double)vol);
            err = AudioDeviceSetProperty(self->device_id, NULL, 0, 0, kAudioDevicePropertyVolumeDecibels, sizeof(vol), &vol);
            if (err) { 
                printstatus(self, "AudioUnitSetProperty(kAudioDevicePropertyVolumeDecibels)", err);
            } else {
                // success. read-back the value
                Float32 actual;
                UInt32 size = sizeof(actual);
                err = AudioDeviceGetProperty(self->device_id, 0, 0, kAudioDevicePropertyVolumeDecibels, &size, &actual);
                if (err == noErr) {
                    self->actual_volume = actual;
                    //RAAT__DEBUG("got exclusive volume %f", (double)actual);
                } else {
                    printstatus(self, "AudioUnitGetProperty(kAudioDevicePropertyVolumeDecibels)", err);
                }
            }
        } else {
            Float32 vol = volume_to_gain(self->volume);
            // in exclusive mode, set the device volume. This will be applied in hardware.
            //RAAT__DEBUG("set exclusive volume to %f", (double)vol);
            err = AudioDeviceSetProperty(self->device_id, NULL, 0, 0, kAudioDevicePropertyVolumeScalar, sizeof(vol), &vol);
            if (err == kAudioHardwareUnknownPropertyError) {
                RAAT__DEBUG("[volume/coreaudio] AudioDeviceSetProperty(kAudioDevicePropertyVolumeScalar) failed. Falling back to using kAudioHardwareServiceDeviceProperty_VirtualMasterVolume");
                err = AudioDeviceSetProperty(self->device_id, NULL, 0, 0, kAudioHardwareServiceDeviceProperty_VirtualMasterVolume, sizeof(vol), &vol);
                if (err) { 
                    printstatus(self, "AudioUnitSetProperty(kAudioHardwareServiceDeviceProperty_VirtualMasterVolume)", err);
                } else {
                    // success. read-back the value
                    Float32 actual;
                    UInt32 size = sizeof(actual);
                    err = AudioDeviceGetProperty(self->device_id, 0, 0, kAudioHardwareServiceDeviceProperty_VirtualMasterVolume, &size, &actual);
                    if (err == noErr) self->actual_volume = actual;
                }
            } else if (err) { 
                printstatus(self, "AudioUnitSetProperty(kAudioDevicePropertyVolumeScalar)", err);
            } else {
                // success. read-back the value
                Float32 actual;
                UInt32 size = sizeof(actual);
                err = AudioDeviceGetProperty(self->device_id, 0, 0, kAudioDevicePropertyVolumeScalar, &size, &actual);
                if (err == noErr) self->actual_volume = actual;
            }
        }
    } else {
        // in shared mode, set the HAL volume. This requires cooperation from the output plugin
        Float32 vol = volume_to_gain(self->volume);
        RAAT__coreaudio_output_set_shared_volume(RAAT__device_get_output_plugin(self->device), vol, self->mute);
    }

    save_cached_data(self);
}

static RC__Status volume_set_volume(void *vself, double volume_value) {
    CoreAudioVolumePlugin *self = (CoreAudioVolumePlugin*)vself;
    RAAT__VolumeState state;
    bool changed = false;

    uv_mutex_lock(&self->lock);
    if (self->volume != volume_value) {
        self->volume = volume_value;
        RAAT__TRACE("[volume/coreaudio] volume => %f", self->volume);
        changed = true;
        LOCKED_update_coreaudio(self);
        LOCKED_get_state(self, &state);
    }
    uv_mutex_unlock(&self->lock);

    if (changed) RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);

    return RC__STATUS_SUCCESS;
}

static RC__Status volume_set_mute(void *vself, bool mute_value) {
    CoreAudioVolumePlugin *self = (CoreAudioVolumePlugin*)vself;
    RAAT__VolumeState state;
    bool changed = false;

    uv_mutex_lock(&self->lock);
    if (self->mute != mute_value) {
        self->mute = mute_value;
        RAAT__TRACE("[volume/coreaudio] mute => %d", mute_value);
        changed = true;
        LOCKED_update_coreaudio(self);
        LOCKED_get_state(self, &state);
    }
    uv_mutex_unlock(&self->lock);
    
    if (changed) RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);

    return RC__STATUS_SUCCESS;
}

static OSStatus
on_prop_change(AudioObjectID                         inObjectID,
               UInt32                                inNumberAddresses,
               const AudioObjectPropertyAddress      inAddresses[],
               void                                  *inClientData)
{
    CoreAudioVolumePlugin *self = (CoreAudioVolumePlugin*)inClientData;
    load_coreaudio_state(self);
    return noErr;
}

char *copy_cfstring(CoreAudioVolumePlugin *self, CFStringRef ref) {
    char* cp = NULL;
    if (ref != NULL) {
        /* use max size + 1 so malloc works for empty string */
        CFIndex size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(ref), kCFStringEncodingUTF8) + 1;
        cp = (char*)RC__alloc(self->alloc, size);
        if (cp != NULL) {
            if (!CFStringGetCString(ref, cp, size, kCFStringEncodingUTF8)) {
                RC__free(self->alloc, cp); 
                cp = NULL;
            }
        }
    }
    return cp;
}

RC__Status 
RAAT__coreaudio_volume_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__VolumePlugin **out_volume) { 
    OSStatus err;
    UInt32 size;
    alloc = RC__allocator_default(alloc);
    CoreAudioVolumePlugin *self            = RC__new0(alloc, CoreAudioVolumePlugin, 1);
    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;

    self->alloc                        = alloc;
    self->log                          = RAAT__device_get_log(device);
    self->output_plugin                = RAAT__device_get_output_plugin(device);

    const char *unique_id = json_string_value(json_object_get(config, "device"));
    RAAT__TRACE("[volume/coreaudio] initializing %s", unique_id ? unique_id : "System Output");
    if (unique_id == NULL) {
        // get the default output device id
        size = sizeof(AudioDeviceID);
        err = AudioHardwareGetProperty(kAudioHardwarePropertyDefaultOutputDevice, &size, &self->device_id);
        printstatus(self, "AudioHardwareGetProperty(kAudioHardwarePropertyDefaultOutputDevice)", err);
        if (err) {
            RC__free(alloc, self);
            return RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED;
        }

    } else {
        // translate unique id string to AudioDeviceID
        CFStringRef           uniqueid_cfstring = CFStringCreateWithCString(kCFAllocatorDefault, unique_id, kCFStringEncodingUTF8);
        AudioValueTranslation trans;

        trans.mInputData      = &uniqueid_cfstring;
        trans.mInputDataSize  = sizeof(CFStringRef);
        trans.mOutputData     = &self->device_id;
        trans.mOutputDataSize = sizeof(self->device_id);

        size = sizeof(AudioValueTranslation);
        AudioObjectPropertyAddress propertyAddress = {
            kAudioHardwarePropertyDeviceForUID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMaster
        };

        err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, 0, &size, &trans);
        CFRelease(uniqueid_cfstring);
        printstatus(self, "AudioHardwareGetPropertyData(kAudioHardwarePropertyDeviceForUID)", err);
        if (err) { 
            RC__free(alloc, self);
            return RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED; 
        }
    }

    // determine whether we are in exclusive mode
    json_t *exclusive_mode = json_object_get(config, "exclusive_mode");
    if (exclusive_mode) self->exclusive_mode = json_boolean_value(exclusive_mode);
    RAAT__TRACE("[volume/coreaudio] exclusivemode=%d", self->exclusive_mode);

    self->plugin.get_info              = volume_get_info;
    self->plugin.add_state_listener    = volume_add_state_listener;
    self->plugin.remove_state_listener = volume_remove_state_listener;
    self->plugin.get_state             = volume_get_state;
    self->plugin.set_volume            = volume_set_volume;
    self->plugin.set_mute              = volume_set_mute;

    self->unique_id = unique_id ? RC__allocator_strdup(alloc, unique_id) : NULL;
    self->mute      = false;
    self->volume    = 100;
    self->device    = device;
    uv_mutex_init(&self->lock);
    RAAT__volume_state_listeners_init(&self->state_listeners, self->alloc);

    if (self->exclusive_mode) {
        //
        // Might open this up as a setting later, but for now, detect exaSound and set it up right.
        //
        CFStringRef deviceName = NULL;
        UInt32 dataSize = sizeof(deviceName);
        AudioObjectPropertyAddress propertyAddress = { 
            kAudioHardwarePropertyDevices, 
            kAudioObjectPropertyScopeOutput, 
            kAudioObjectPropertyElementMaster 
        };
        propertyAddress.mSelector = kAudioObjectPropertyName;
        RAAT__TRACE("getting device name");
        OSStatus err = AudioObjectGetPropertyData(self->device_id, &propertyAddress, 0, NULL, &dataSize, &deviceName);
        if (err == noErr) {
            char *name = copy_cfstring(self, deviceName);
            RAAT__TRACE("[volume/coreaudio] got device name %s", name);
            if (strstr(name, "exaSound")) {
                RAAT__TRACE("[volume/coreaudio] setting dB mode for exaSound device");
                self->volume_mode = VOLUME_MODE_DB;
            }
            RC__free(self->alloc, name);
            CFRelease(deviceName);
        }
    } else {
        float linear_gain = (self->volume == 0) ? 0.0 : powf (10, (float) SHARED_VOLUME_RANGE_DB * (self->volume - 100) / 100 / 20);
        RAAT__coreaudio_output_set_shared_volume(RAAT__device_get_output_plugin(self->device), linear_gain, self->mute);
    }

    if (load_cached_data(self)) {
        RAAT__TRACE("[volume/coreaudio] loaded cached data");
    } else {
        RAAT__TRACE("[volume/coreaudio] starting with fresh state");
        self->volume            = 100;
        self->actual_volume     = -1;
        self->mute              = 0;
    }

    bool volume_supported = load_coreaudio_state(self);
    if (!volume_supported) {
        RAAT__TRACE("[volume/coreaudio] this device doesn't support volume control. Aborting");
        RAAT__coreaudio_volume_plugin_delete((RAAT__VolumePlugin*)self);
        return RAAT__VOLUME_PLUGIN_STATUS_VOLUME_NOT_SUPPORTED;
    } else {
        // Some devices (but not many) support a master channel
        AudioObjectPropertyAddress propertyAddress = { 
            kAudioDevicePropertyVolumeScalar, 
            kAudioDevicePropertyScopeOutput,
            kAudioObjectPropertyElementMaster 
        };

        if(AudioObjectHasProperty(self->device_id, &propertyAddress)) {
            AudioObjectAddPropertyListener(self->device_id, &propertyAddress, on_prop_change, self);
        } else {
            // Typically the L and R channels are 1 and 2 respectively, but could be different
            propertyAddress.mElement = 1;
            AudioObjectAddPropertyListener(self->device_id, &propertyAddress, on_prop_change, self);
            propertyAddress.mElement = 2;
            AudioObjectAddPropertyListener(self->device_id, &propertyAddress, on_prop_change, self);
        }

        AudioObjectPropertyAddress mutePropertyAddress = { 
            kAudioDevicePropertyMute, 
            kAudioDevicePropertyScopeOutput,
            kAudioObjectPropertyElementMaster 
        };
        if(AudioObjectHasProperty(self->device_id, &mutePropertyAddress)) {
            AudioObjectAddPropertyListener(self->device_id, &mutePropertyAddress, on_prop_change, self);
        }
    }

    if (self->exclusive_mode) {
        AudioValueRange range = {0,};
        size = sizeof(range);
        err  = AudioDeviceGetProperty(self->device_id, 0, 0, kAudioDevicePropertyVolumeRangeDecibels, &size, &range);
        if (!err) {
            RAAT__TRACE("[volume/coreaudio] Volume Range: %fdB-%fdB", range.mMinimum, range.mMaximum);
            self->db_min_volume = range.mMinimum;
            self->db_max_volume = range.mMaximum;
        } else {
            RAAT__TRACE("[volume/coreaudio] Couldn't load volume range");
            self->db_min_volume = 0;
            self->db_max_volume = 0;
        }

    } else {
        self->db_min_volume = -SHARED_VOLUME_RANGE_DB;
        self->db_max_volume = 0.0;
        // set saved value to coreaudio
        LOCKED_update_coreaudio(self);
    }

    self->info = json_object();
    json_object_set(self->info, "config", config);

    *out_volume = &self->plugin;
    return RC__STATUS_SUCCESS;
}

void
RAAT__coreaudio_volume_plugin_delete(RAAT__VolumePlugin *volume) {
    CoreAudioVolumePlugin *self = (CoreAudioVolumePlugin*)volume;

    // Some devices (but not many) support a master channel
    AudioObjectPropertyAddress propertyAddress = { 
        kAudioDevicePropertyVolumeScalar, 
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMaster 
    };

    if(AudioObjectHasProperty(self->device_id, &propertyAddress)) {
        AudioObjectRemovePropertyListener(self->device_id, &propertyAddress, on_prop_change, self);
    } else {
        propertyAddress.mElement = 1;
        AudioObjectRemovePropertyListener(self->device_id, &propertyAddress, on_prop_change, self);
        propertyAddress.mElement = 2;
        AudioObjectRemovePropertyListener(self->device_id, &propertyAddress, on_prop_change, self);
    }

    AudioObjectPropertyAddress mutePropertyAddress = { 
        kAudioDevicePropertyMute, 
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMaster 
    };
    if(AudioObjectHasProperty(self->device_id, &mutePropertyAddress)) {
        AudioObjectRemovePropertyListener(self->device_id, &mutePropertyAddress, on_prop_change, self);
    }

    uv_mutex_destroy(&self->lock);
    if (self->info) json_decref(self->info);
    RAAT__volume_state_listeners_destroy(&self->state_listeners);
    RC__free(self->alloc, self);

    if (!self->exclusive_mode && self->output_plugin) {
        // update volume at output plugin
        Float32 vol = volume_to_gain(self->volume);
        RAAT__coreaudio_output_set_shared_volume(self->output_plugin, vol, self->mute);
    }
}

