//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_plugin_output_coreaudio.h"
#include "raat_plugin_output_coreaudio.h"
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
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <mach/mach_time.h>
#include <libkern/OSAtomic.h>
#include <CoreServices/CoreServices.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/AudioHardware.h>
#include <AudioToolbox/AudioToolbox.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/audio/IOAudioDefines.h>

#ifdef RAAT__COREAUDIO_NADAC
#include <raat_coreaudio_nadac.h>
#endif

#define RAAT__CURRENT_LOG self->log

/*
 * Output Plugin
 */

typedef enum {
    IDLE                = 0,
    STOPPED             = 1,
    RUNNING             = 2,
} CoreAudioOutputState;

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

    CoreAudioOutputState         state;

    json_t                      *custom_signal_path;
    json_t                      *soft_volume_signal_path;

    int                          next_token;

    RAAT__StreamFormat           origformat;
    RAAT__StreamFormat           pcmformat;  
    int                          hw_bitspersample;
    int                          current_token;
    RAAT__OutputLostCallback     cb_lost;
    void                        *cb_lost_userdata;

    RAAT__Stream                *stream;

    int64_t                      last_monotonic_sample_systime;  // in ns (RC__now_ns())
    int64_t                      last_monotonic_sample;          // in samples, based on counter

    int64_t                      start_streamsample;
    int64_t                      start_time;               
    bool                         new_stream;

    // state for coreaudio
    unsigned                     periods;
    double                       buffer_duration_secs;
    bool                         use_max_buffer_size;
    bool                         power_of_two_buffer_size;
    bool                         force_max_volume;               
    int                          pcm_samples_per_buf;
    DsdMode                      dsd_mode;
    int                          max_dsd_rate;

    double                       resync_delay_secs;
    int64_t                      resync_delay_remaining_ns;

    int64_t                      last_delay_ns;

    bool                         dsd_dop_flipper;

    RAAT__OutputSetupCallback    setup_cb;
    void *                       setup_cb_userdata;

    RC__Status                   get_supported_formats_status;
    RAAT__StreamFormat          *supported_formats;
    size_t                       n_supported_formats;

    RAAT__DriftCorrection        drift_correction;

    json_t                      *info;

    char *                      unique_id; 

    // coreaudio state
    AudioUnit                   outputunit;
    AudioDeviceID               device_id;
    AudioStreamID               stream_id;
    int                         exclusive_mode;
    bool                        integer_mode;
    int                         hw_channels;

    int64_t                     streamsample;
    int                         origformat_samples_per_buf;
    int                         ns_per_buf;
    int                         bytes_per_origbuf;
    int                         bytes_per_pcmbuf;
    int                         bytes_per_hwbuf;

    AudioStreamRangedDescription *formatlist;
    int                           formatlist_len;

    UInt32                      latency_frames;

    RAAT__OutputSoftwareVolume  *volume;

    uint8_t                    *origbuf;
    uint8_t                    *pcmbuf;   
    uint8_t                    *dsdbuf; 

    // for shared mode output, we need to cooperate to provide volume control
    bool                        using_shared_volume;
    float                       shared_volume;
    bool                        shared_mute;

#ifdef RAAT__COREAUDIO_NADAC
    bool                        is_nadac;
#endif
} CoreAudioOutputPlugin;

static void printstatus(CoreAudioOutputPlugin *self, char *str, OSStatus err) {
    if (err != noErr) { 
        int ierr = err; 
        char *cerr = (char*)&ierr; 
        if (isprint(cerr[0]) && isprint(cerr[1]) && isprint(cerr[2]) && isprint(cerr[3])) 
            RAAT__WARNING("Error in %s: %d (0x%x) (%c%c%c%c)", str, (int)err, (int)err, cerr[3], cerr[2], cerr[1], cerr[0]); 
        else  
            RAAT__WARNING("Error in %s: %d (0x%x)", str, (int)err, (int)err); 
    }
}

#ifdef RAAT__COREAUDIO_NADAC
static void nadac_restore_thread(void *vseq) {
    // wait 2s, and if no-one else has set the rate, unhook from NADAC completely and restore settings
    //
    // note that this has no dependency on self--so it doesn't crash if the device goes away. 
    //
    int seq = RC__POINTER_TO_INT(vseq);
    usleep(2000000);
    if (seq == RAAT__coreaudio_nadac_get_seq()) {
        RAAT__coreaudio_nadac_restore();
    }
}
#endif

static void LOCKED_teardown_coreaudio(CoreAudioOutputPlugin *self) {
        OSStatus err;
        AudioOutputUnitStop(self->outputunit);

        err = AudioUnitUninitialize(self->outputunit);
        printstatus(self, "AudioUnitUninitialize()", err);

#ifdef RAAT__COREAUDIO_NADAC
        if (self->is_nadac) {
            uv_thread_t tid;
            uv_thread_create(&tid, nadac_restore_thread, RC__INT_TO_POINTER(RAAT__coreaudio_nadac_get_seq()));
        }
#endif

        if (self->exclusive_mode) {
            pid_t pid = 0;
            err = AudioDeviceSetProperty(self->device_id, NULL, 0, 0, kAudioDevicePropertyHogMode, sizeof(pid), &pid);
            printstatus(self, "AudioDeviceSetProperty(kAudioDevicePropertyHogMode)", err);
        }

        AudioComponentInstanceDispose(self->outputunit);
        self->outputunit = NULL;

}

static void LOCKED_force_max_volume(CoreAudioOutputPlugin *self) {
    Float32 vol = 1.0f;
    if (self->exclusive_mode) {
        RAAT__TRACE("[output/coreaudio] forcing max volume (exclusive mode)");
        OSStatus err = AudioDeviceSetProperty(self->device_id, NULL, 0, 0, kAudioDevicePropertyVolumeScalar, sizeof(vol), &vol);
        if (err) {
            AudioDeviceSetProperty(self->device_id, NULL, 0, 0, kAudioHardwareServiceDeviceProperty_VirtualMasterVolume, sizeof(vol), &vol);
        }
    } else {
        RAAT__TRACE("[output/coreaudio] forcing max volume (shared mode)");
        AudioUnitSetParameter(self->outputunit, kHALOutputParam_Volume, kAudioUnitScope_Global, 0, vol, 0);
    }
}

static void LOCKED_update_shared_volume(CoreAudioOutputPlugin *self) {
    if (self->outputunit && self->using_shared_volume) {
        float vol = self->shared_volume;
        if (self->shared_mute) vol = 0;
        AudioUnitSetParameter(self->outputunit, kHALOutputParam_Volume, kAudioUnitScope_Global, 0, vol, 0);
    }
}

void
RAAT__coreaudio_output_set_shared_volume(RAAT__OutputPlugin *vself, float volume, bool mute) {
    CoreAudioOutputPlugin *self = (CoreAudioOutputPlugin*)vself;

    uv_mutex_lock(&self->lock);
    self->using_shared_volume = true;
    self->shared_volume       = volume;
    self->shared_mute         = mute;
    LOCKED_update_shared_volume(self);
    uv_mutex_unlock(&self->lock);
}

static RC__Status output_get_info(void *vself, json_t **out_info) {
    CoreAudioOutputPlugin *self = (CoreAudioOutputPlugin*)vself;

    RC__ASSERT(self);
    RC__ASSERT(out_info);

    json_incref(self->info);
    *out_info = self->info;

    return RC__STATUS_SUCCESS;
}

OSStatus
set_best_output_frames_per_buffer(CoreAudioOutputPlugin *self, const AudioDeviceID device, UInt32 *actual_value) {
    UInt32 afpb;
    UInt32 propsize = sizeof(UInt32);
    OSStatus err;
    int best = -1; /*the best blocksize*/
    AudioValueRange range;

    UInt32 requested_value;

    if (self->use_max_buffer_size) {
        requested_value = (UInt32)self->pcmformat.sample_rate;
    }  else {
        requested_value = (UInt32)(self->pcmformat.sample_rate * self->buffer_duration_secs);
    }

    if (actual_value == NULL)
        actual_value = &afpb;

    // try and set exact requested value
    err = AudioDeviceSetProperty(device, NULL, 0, FALSE,
                                 kAudioDevicePropertyBufferFrameSize,
                                 propsize, &requested_value);
    err = AudioDeviceGetProperty(device, 0, FALSE,
                                 kAudioDevicePropertyBufferFrameSize,
                                 &propsize, actual_value);
    printstatus(self, "AudioDeviceGetProperty(kAudioDevicePropertyBufferFrameSize)", err);
    if (err)
        return err;
    if (*actual_value == requested_value)
        return noErr; /* we are done */

    // fetch available block sizes 
    propsize = sizeof(AudioValueRange);
    err = AudioDeviceGetProperty(device, 0, FALSE,
                                 kAudioDevicePropertyBufferFrameSizeRange,
                                 &propsize, &range);
    printstatus(self, "AudioDeviceGetPropertyInfo(kAudioDevicePropertyBufferFrameSizeRange)", err);
    if (err) return err;


    if (requested_value < range.mMinimum) best = (int)range.mMinimum;
    if (requested_value > range.mMaximum) best = (int)range.mMaximum;
    if (best == -1) best = (int)(((range.mMaximum - range.mMinimum) / 2) + range.mMinimum);

    RAAT__DEBUG("[output/coreaudio] Requested buffer size %d, Available %d-%d, Best %d", requested_value, (int)range.mMinimum, (int)range.mMaximum, best);

    if (self->power_of_two_buffer_size) {
        int best_pow2 = 1;
        while (best_pow2 * 2 < best) { best_pow2 *= 2; }
        if (best_pow2 >= (int)range.mMinimum && best_pow2 <= (int)range.mMaximum) {
            RAAT__DEBUG("[output/coreaudio] Constraining buffer size to power of 2: Best %d -> %d", best, best_pow2);
            best = best_pow2;
        }
    }

    // set the buffer size (ignore errors) 
    requested_value = (UInt32) best;
    propsize        = sizeof(UInt32);
    err = AudioDeviceSetProperty(device, NULL, 0, FALSE,
                                 kAudioDevicePropertyBufferFrameSize,
                                 propsize, &requested_value);
    // read the property to check that it was set 
    err = AudioDeviceGetProperty(device, 0, FALSE,
                                 kAudioDevicePropertyBufferFrameSize,
                                 &propsize, actual_value);

    printstatus(self, "AudioDeviceGetProperty(kAudioDevicePropertyBufferSize)", err);
    if (err)
        return err;

    return noErr;
}

static OSStatus propertyProc(
                      AudioDeviceID inDevice, 
                      UInt32 inChannel, 
                      Boolean isInput, 
                      AudioDevicePropertyID inPropertyID, 
                      void* inClientData)
{
    return noErr;
}

/* sets the value of the given property and waits for the change to 
 be acknowledged, and returns the final value, which is not guaranteed
 by this function to be the same as the desired value. Obviously, this
 function can only be used for data whose input and output are the
 same size and format, and their size and format are known in advance.
 whether or not the call succeeds, if the data is successfully read,
 it is returned in outPropertyData. If it is not read successfully,
 outPropertyData is zeroed, which may or may not be useful in
 determining if the property was read. */
OSStatus AudioDeviceSetPropertyNowAndWaitForChange(CoreAudioOutputPlugin *self,
                                                  AudioDeviceID inDevice,
                                                  UInt32 inChannel, 
                                                  Boolean isInput, 
                                                  AudioDevicePropertyID inPropertyID,
                                                  UInt32 inPropertyDataSize, 
                                                  const void *inPropertyData,
                                                  void *outPropertyData)
{
    OSStatus err;
    UInt32 outPropertyDataSize = inPropertyDataSize;
    
    /* First, see if it already has that value. If so, return. */
    err = AudioDeviceGetProperty(inDevice, inChannel,
                                    isInput, inPropertyID, 
                                    &outPropertyDataSize, outPropertyData);
    printstatus(self, "AudioDeviceGetProperty(0) in AudioDeviceSetPropertyNowAndWaitForChange", err);
    if (err) {
        memset(outPropertyData, 0, inPropertyDataSize);
        goto fail;
    }
    if (inPropertyDataSize!=outPropertyDataSize) {
        RAAT__WARNING("AudioDeviceSetPropertyNowAndWaitForChange: size mismatch %d != %d", (int)inPropertyDataSize, (int)outPropertyDataSize);
        return -1;
    }
    if (0==memcmp(outPropertyData, inPropertyData, outPropertyDataSize))
        return noErr;
    
    /* Ideally, we'd use a condition variable to determine changes.
     we could set that up here. */
    
    /* If we were using a cond variable, we'd do something useful here,
     but for now, this is just to make 10.6 happy. */
    err = AudioDeviceAddPropertyListener(inDevice, inChannel, isInput,
                                            inPropertyID, propertyProc,
                                            NULL); 
    printstatus(self, "AudioDeviceAddPropertyListener(0) in AudioDeviceSetPropertyNowAndWaitForChange", err);
    if (err)
    /* we couldn't add a listener. */
        goto fail;
    
    /* set property */
    err  = AudioDeviceSetProperty(inDevice, NULL, inChannel,
                                     isInput, inPropertyID,
                                     inPropertyDataSize, inPropertyData);
    printstatus(self, "AudioDeviceSetProperty(0) in AudioDeviceSetPropertyNowAndWaitForChange", err);
    if (err)
        goto fail;
    
    struct timeval tv1, tv2;
    gettimeofday(&tv1, NULL);
    memcpy(&tv2, &tv1, sizeof(struct timeval));
    while (tv2.tv_sec - tv1.tv_sec < 30) {
        err = AudioDeviceGetProperty(inDevice, inChannel, isInput, inPropertyID, &outPropertyDataSize, outPropertyData);
        printstatus(self, "AudioDeviceGetProperty(1) in AudioDeviceSetPropertyNowAndWaitForChange", err);
        if (err) {
            memset(outPropertyData, 0, inPropertyDataSize);
            goto fail;
        }
        /* and compare... */
        if (0 == memcmp(outPropertyData, inPropertyData, outPropertyDataSize)) {
            AudioDeviceRemovePropertyListener(inDevice, inChannel, isInput, inPropertyID, propertyProc);
            return noErr;
        }
        /* No match yet, so let's sleep and try again. */
        usleep(100);
        gettimeofday(&tv2, NULL);
    }
    RAAT__WARNING("Timeout waiting for device setting.");
    
    AudioDeviceRemovePropertyListener(inDevice, inChannel, isInput, inPropertyID, propertyProc);
    return noErr;
    
fail:
    AudioDeviceRemovePropertyListener(inDevice, inChannel, isInput, inPropertyID, propertyProc);
    return err;
}

/*
 * Sets the sample rate the HAL device.
 *
 * otherwise      : set the exact sample rate.
 *             If that fails, check for available sample rates, and choose one
 *             higher than the requested rate. If there isn't a higher one,
 *             just use the highest available.
 */
OSStatus
set_sample_rate(CoreAudioOutputPlugin *self, const Float64 desired_rate)
{
    const bool isInput = FALSE;
    Float64 srate;
    UInt32 propsize = sizeof(Float64);
    OSStatus err;
    AudioValueRange *ranges;
    int i=0;
    Float64 max  = -1; /*the maximum rate available*/
    Float64 best = -1; /*the lowest sample rate still greater than desired rate*/
    
    RAAT__DEBUG("Setting sample rate for device %ld to %g.",self->device_id,(float)desired_rate);
    
    // try setting the sample rate 
    srate = 0;
    err = AudioDeviceSetPropertyNowAndWaitForChange(self,
                                                    self->device_id, 0, isInput,
                                                    kAudioDevicePropertyNominalSampleRate,
                                                    propsize, &desired_rate, &srate);
    
    printstatus(self, "AudioDeviceSetPropertyNowAndWaitForChange(kAudioDevicePropertyNominalSampleRate)", err);
    if (srate != 0 && srate == desired_rate) return noErr; // if the rate agrees, and was changed, we are done 
    if (!err && srate == desired_rate)       return noErr; // if the rate agrees, and we got no errors, we are done 
    if (self->exclusive_mode)                     return -1;    // we've failed if the rates disagree and we are setting input 
    
    // generate a list of available sample rates 
    err = AudioDeviceGetPropertyInfo(self->device_id, 0, isInput,
                                     kAudioDevicePropertyAvailableNominalSampleRates,
                                     &propsize, NULL);
    printstatus(self, "AudioDeviceGetPropertyInfo(kAudioDevicePropertyAvailableNominalSampleRates)", err);
    if (err) return err;

    ranges = (AudioValueRange *)calloc(1, propsize);
    if (!ranges) return -1;

    err = AudioDeviceGetProperty(self->device_id, 0, isInput,
                                 kAudioDevicePropertyAvailableNominalSampleRates,
                                 &propsize, ranges);
    printstatus(self, "AudioDeviceGetProperty(kAudioDevicePropertyAvailableNominalSampleRates)", err);
    if (err) {
        free(ranges);
        return err;
    }
    RAAT__DEBUG("Requested sample rate of %g was not available.", (float)desired_rate);
    RAAT__DEBUG("%lu Available Sample Rates are:",propsize/sizeof(AudioValueRange));
    for (i=0; i<propsize/sizeof(AudioValueRange); ++i)
        RAAT__DEBUG  ("\t%g-%g", (float) ranges[i].mMinimum, (float) ranges[i].mMaximum);
    RAAT__DEBUG("-----");
    
    // now pick the best available sample rate 
    for (i=0; i<propsize/sizeof(AudioValueRange); ++i) {
        if (ranges[i].mMaximum > max) max = ranges[i].mMaximum;
        if (ranges[i].mMinimum > desired_rate) {
            if (best < 0)
                best = ranges[i].mMinimum;
            else if (ranges[i].mMinimum < best)
                best = ranges[i].mMinimum;
        }
    }
    if (best < 0)
        best = max;
    free(ranges);
    
    // set the sample rate 
    propsize = sizeof(best);
    srate = 0;
    err = AudioDeviceSetPropertyNowAndWaitForChange(self,
                                                    self->device_id, 0, isInput,
                                                    kAudioDevicePropertyNominalSampleRate,
                                                    propsize, &best, &srate);
    printstatus(self, "AudioDeviceSetPropertyNowAndWaitForChange(kAudioDevicePropertyNominalSampleRate)", err);
    
    // if the set rate matches, we are done 
    if (srate != 0 && srate == best) return noErr;
    
    if (err) return err;
    return -2;
}

static OSStatus
cb_buffer_underrun                              (AudioDeviceID inDevice,
                                                 UInt32 inChannel,
                                                 Boolean isInput,
                                                 AudioDevicePropertyID inPropertyID,
                                                 void* inClientData)
{
    //CoreAudioOutputPlugin *self = inClientData;
    //RAAT__WARNING("buffer underrun!");
    return 0;
}



static void LOCKED_update_signal_path(CoreAudioOutputPlugin *self) {
    json_t *message     = json_object();
    json_t *signal_path = json_array();

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
        json_t *output = json_object();
        json_object_set_new(output, "type", json_string("output"));

        if (self->origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) {
            switch (self->dsd_mode) {
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

        if (self->exclusive_mode) {
            json_object_set_new(output, "quality", json_string("lossless"));
            json_object_set_new(output, "method", json_string("coreaudio_exclusive"));
        } else {
            json_object_set_new(output, "quality", json_string("high"));
            json_object_set_new(output, "method", json_string("coreaudio_shared"));
        }

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

static bool supports_pcm_format(CoreAudioOutputPlugin *self, int samplerate, int bitspersample, int channels) {
    int j;
    for (j = 0; j < self->formatlist_len; j++) {
        AudioStreamRangedDescription *format = &self->formatlist[j];

        bool rate_ok          = false;
        bool channels_ok      = false;
        bool bitspersample_ok = false;

        if ((format->mFormat.mFormatFlags & kLinearPCMFormatFlagIsFloat) && bitspersample == 32) continue;       // don't accept float32 
        if (format->mFormat.mSampleRate == samplerate) rate_ok = true;
        if (samplerate >= format->mSampleRateRange.mMinimum && samplerate <= format->mSampleRateRange.mMaximum) rate_ok = true;
        if (channels <= format->mFormat.mChannelsPerFrame) channels_ok = true;
        if (bitspersample <= format->mFormat.mBitsPerChannel) bitspersample_ok = true;
        if (bitspersample_ok && rate_ok && channels_ok) return true;
    }
    return false;
}

static RC__Status output_get_supported_formats(void *vself, RC__Allocator *alloc, size_t *out_nformats, RAAT__StreamFormat/*?*/ **out_formats) {
    CoreAudioOutputPlugin *self = vself;

    int pcmbaserate[]      = { 44100, 48000,       0 };
    int pcmmult[]          = { 1, 2, 4, 8, 16,     0 };
    int channels[]         = { 1, 2, 3, 4, 5, 6, 7, 8, 0 };
    int pcmbitspersample[] = { 16, 24, 32,         0 };
    int dsdrate[]          = { 44100*64, 44100*128, 44100*256, 44100*512, 0 };

    int channel_i, pcmbaserate_i, pcmmult_i, pcmbitspersample_i, dsdrate_i;
    int idx = 0;

    size_t              nformats = count(channels) * (count(pcmbaserate) * count(pcmmult) * count(pcmbitspersample) + count(dsdrate));
    RAAT__StreamFormat *formats  = RC__new0(alloc, RAAT__StreamFormat, nformats);

    if (formats == NULL) return RC__STATUS_OUT_OF_MEMORY;

    for (channel_i = 0; channels[channel_i]; channel_i++) {
        for (pcmmult_i = 0; pcmmult[pcmmult_i]; pcmmult_i++)
        for (pcmbaserate_i = 0; pcmbaserate[pcmbaserate_i]; pcmbaserate_i++)
        for (pcmbitspersample_i = 0; pcmbitspersample[pcmbitspersample_i]; pcmbitspersample_i++) {
            if (supports_pcm_format(self, pcmbaserate[pcmbaserate_i] * pcmmult[pcmmult_i], 
                                          pcmbitspersample[pcmbitspersample_i],
                                          channels[channel_i])) {
                formats[idx].sample_type     = RAAT__SAMPLE_TYPE_PCM;
                formats[idx].sample_rate     = pcmbaserate[pcmbaserate_i] * pcmmult[pcmmult_i];
                formats[idx].bits_per_sample = pcmbitspersample[pcmbitspersample_i];
                formats[idx].channels        = channels[channel_i];
                if (!self->volume || (RC__STATUS_SUCCESS == self->volume->supports_format(self->volume->userdata, &formats[idx]))) {
                    idx++;
                }
            }
        }

        if (self->dsd_mode != DSD_MODE_NONE) {
            for (dsdrate_i = 0; dsdrate[dsdrate_i]; dsdrate_i++) {
                if (supports_pcm_format(self, dsdrate[dsdrate_i] / 16,
                                              24,
                                              channels[channel_i])) {
                    formats[idx].sample_type     = RAAT__SAMPLE_TYPE_DSD;
                    formats[idx].sample_rate     = dsdrate[dsdrate_i];
                    formats[idx].bits_per_sample = 1;
                    formats[idx].channels        = channels[channel_i];
                    if (!self->volume || (RC__STATUS_SUCCESS == self->volume->supports_format(self->volume->userdata, &formats[idx]))) {
                        idx++;
                    }
                }
            }
        }
    }

    *out_nformats = idx;
    *out_formats  = formats;

    return RC__STATUS_SUCCESS;
}

typedef struct {
    CoreAudioOutputPlugin *self;
    int                   token;
} CallbackState;

void pack_hw_samples(CoreAudioOutputPlugin *self, uint8_t *input_buf, uint8_t *output_buf, int nsamples, DsdMode dsd_mode, bool dsd_flipper) {
    RAAT__StreamFormat *format = &self->pcmformat;
    int samplevalues         = nsamples * format->channels;
    int input_stride         = format->bits_per_sample / 8;
    int output_stride        = self->hw_bitspersample / 8;
    int channels             = format->channels;
    int unused_channels      = self->hw_channels - channels;
    int unused_channels_size = unused_channels * output_stride;
    int i,ch;

    if (input_stride == output_stride && unused_channels_size == 0) {
        memcpy(output_buf, input_buf, input_stride * samplevalues);
        return;
    }

    if (output_stride < input_stride) RC__ASSERT(0);

    for (i = 0; i < nsamples; i++) {
        for (ch = 0; ch < channels; ch++) {
            switch (output_stride - input_stride) {
                case 2: *output_buf++ = 0;  /* fall through */
                case 1: *output_buf++ = 0;  /* fall through */
                case 0: break;
                default: RC__ASSERT(0); break;
            }
            switch (format->bits_per_sample) {
                case 32: *output_buf++ = *input_buf++; /* fall through */
                case 24: *output_buf++ = *input_buf++; /* fall through */
                case 16: *output_buf++ = *input_buf++; 
                         *output_buf++ = *input_buf++; break;
                default: RC__ASSERT(0); break;
            }
        }

        if (unused_channels) {
            if (dsd_mode == DSD_MODE_NONE) {
                memset(output_buf, 0, unused_channels_size);
                output_buf += unused_channels_size;

            } else if (dsd_mode == DSD_MODE_DOP) {
                uint8_t marker = dsd_flipper ? 0x5 : 0xfa; 
                dsd_flipper = !dsd_flipper;
                if (output_stride == 3) {
                    for (ch = 0; ch < unused_channels; ch++) {
                        *output_buf++ = 0x69;
                        *output_buf++ = 0x69;
                        *output_buf++ = marker;
                    }
                } else if (output_stride == 4) {
                    for (ch = 0; ch < unused_channels; ch++) {
                        *output_buf++ = 0x69;
                        *output_buf++ = 0x69;
                        *output_buf++ = marker;
                        *output_buf++ = 0x00;
                    }
                } else {
                    RC__ASSERT(0);
                } 
            } else if (dsd_mode == DSD_MODE_DCS) {
                uint8_t marker = 0xaa;
                if (output_stride == 3) {
                    for (ch = 0; ch < unused_channels; ch++) {
                        *output_buf++ = 0x69;
                        *output_buf++ = 0x69;
                        *output_buf++ = marker;
                    }
                } else if (output_stride == 4) {
                    for (ch = 0; ch < unused_channels; ch++) {
                        *output_buf++ = 0x69;
                        *output_buf++ = 0x69;
                        *output_buf++ = marker;
                        *output_buf++ = 0x00;
                    }
                } else {
                    RC__ASSERT(0);
                } 
            }
        }

        /*
        if (i == 0) {
            uint8_t *orig = output_buf - self->hw_channels * output_stride;
            for (ch = 0; ch < self->hw_channels; ch++) {
                int j;
                for (j = 0; j < output_stride; j++) {
                    printf("%02x ", orig[ch*output_stride+j]);
                }
                printf(" ");
            }
            printf(" channels=%d unused=%d\n", channels, unused_channels);
        }
        */
    }
}

static OSStatus
ev_audiounits_datacb                        (void                        *inRefCon,
                                             AudioUnitRenderActionFlags  *ioActionFlags,
                                             const AudioTimeStamp        *inTimeStamp,
                                             UInt32                      inBusNumber,
                                             UInt32                      inNumberFrames,
                                             AudioBufferList             *ioData)
{
    CallbackState         *cbstate = inRefCon;
    CoreAudioOutputPlugin *self    = cbstate->self;

    uint8_t *hwbuf = (uint8_t*) ioData->mBuffers[0].mData;
    int      left  = ioData->mBuffers[0].mDataByteSize;

    RAAT__Stream       *stream = NULL;
    RAAT__StreamFormat  origformat;
    RAAT__StreamFormat  pcmformat;

    uv_mutex_lock(&self->lock);

    if (cbstate->token == self->current_token) {
        pcmformat  = self->pcmformat;
        origformat = self->origformat;
    } else {
        memset(hwbuf, 0, left);         // XXX: DSD zero-fill? how do we know we're DSD if self->* are not in good state. Ugh.
        return 0;
    }

    int bytes_per_hw_frame = self->hw_bitspersample / 8 * self->hw_channels;
    //RAAT__TRACE("byte per hw frame %d", bytes_per_hw_frame);
    int origformat_samples_per_buf;
    int pcm_samples_per_buf = left / bytes_per_hw_frame;
    if (self->origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) {
        origformat_samples_per_buf = left / bytes_per_hw_frame * 16;
    } else {
        origformat_samples_per_buf = left / bytes_per_hw_frame;
    }
    int ns_per_buf                 = RAAT__stream_format_samples_to_ns(&origformat, origformat_samples_per_buf);

    int64_t now_raw_ns = inTimeStamp->mHostTime;

    int origformat_samples_to_zerofill = 0;

    /*
    RAAT__TRACE("TIMESTAMP");
    RAAT__TRACE("    mHostTime      %lld", inTimeStamp->mHostTime);
    RAAT__TRACE("    mSampleTime    %f", inTimeStamp->mSampleTime);
    RAAT__TRACE("    mRateScalar    %f", inTimeStamp->mRateScalar);
    RAAT__TRACE("    mWordClockTime %lld", inTimeStamp->mWordClockTime);
    */

    int64_t delay_ns = RAAT__stream_format_samples_to_ns(&pcmformat, self->latency_frames);
    self->last_delay_ns = delay_ns;

    self->last_monotonic_sample_systime         =  now_raw_ns + delay_ns;
    self->last_monotonic_sample                 += origformat_samples_per_buf;

    int64_t now = RAAT__stream_format_samples_to_ns(&origformat, self->last_monotonic_sample);

    int correction_samples = 0; 

    if (self->new_stream && self->stream) {
        int64_t start_time = self->start_time;
        if (now + ns_per_buf > start_time) {
            RAAT__TRACE("[coreaudio] starting playback: now (%lldns) + ns_per_buf(%lldns) = %lldns > %lldns streamsample=%lld", now, ns_per_buf, now+ns_per_buf, start_time, self->start_streamsample);
            self->new_stream    = false;
            stream              = self->stream;
            self->streamsample  = self->start_streamsample;

            if (now < start_time) {
                int ns_to_fill      = start_time - now;
                origformat_samples_to_zerofill = RAAT__stream_format_ns_to_samples(&origformat, ns_to_fill);
            } if (now > start_time) {
                self->streamsample += RAAT__stream_format_ns_to_samples(&origformat, now - start_time);
            }
        } else {
            RAAT__TRACE("[coreaudio] waiting for start time...");
        }
    } else {
        stream              = self->stream;
        origformat_samples_to_zerofill = 0;
    }

    correction_samples = RAAT__drift_correction_compute_correction(&self->drift_correction, origformat_samples_per_buf);

    if (stream != NULL) RAAT__stream_incref(stream);

    uv_mutex_unlock(&self->lock);

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
        if (origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) self->origbuf[i] = 0x69;
        else                                                 self->origbuf[i] = 0x00;
    }

    if (stream != NULL && self->resync_delay_remaining_ns <= 0) {
        //if (correction_samples != 0) RAAT__TRACE("[coreaudio] applying %d sample correction", correction_samples);
        RAAT__read_stream_with_drift_correction(stream, self->streamsample, self->origbuf + bytes_to_zerofill, origformat_samples_per_buf - origformat_samples_to_zerofill, correction_samples, NULL, NULL);
        self->streamsample += origformat_samples_per_buf - origformat_samples_to_zerofill + correction_samples;
    }

    if (self->resync_delay_remaining_ns > 0) {
        self->resync_delay_remaining_ns -= ns_per_buf;
        if (self->resync_delay_remaining_ns <= 0) {
            self->setup_cb(self->setup_cb_userdata, RC__STATUS_SUCCESS, cbstate->token);
        }
    }

    uint8_t *buf = self->origbuf;

    if (origformat.sample_type == RAAT__SAMPLE_TYPE_PCM && pcmformat.bits_per_sample > origformat.bits_per_sample) {
        // repack PCM into wider PCM. This case is most relevant when performing digital volume adjustments. We may have
        // opened the audio device at a wider bits-per-sample than the stream, so that we can preserve as much data as possible after
        // volume attenuation
        RAAT__stream_format_repack(&origformat, buf, &pcmformat, self->pcmbuf, origformat_samples_per_buf);
        buf = self->pcmbuf;
    }

    if (self->volume) {
        self->volume->process(self->volume->userdata, 0, buf, origformat_samples_per_buf);
    }

    // DSD must be repacked
    if (origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) {
        bool flipper_state = self->dsd_dop_flipper;
        if (self->dsd_mode == DSD_MODE_DOP) {
            RAAT__pack_dop_samples(&origformat, buf, self->dsdbuf, origformat_samples_per_buf, &self->dsd_dop_flipper);
        } else if (self->dsd_mode == DSD_MODE_DCS) {
            RAAT__pack_dcs_samples(&origformat, buf, self->dsdbuf, origformat_samples_per_buf);
        } else {
            RC__ASSERT(false);
        }
        pack_hw_samples(self, self->dsdbuf, hwbuf, pcm_samples_per_buf, self->dsd_mode, flipper_state);
    } else {
        pack_hw_samples(self, buf, hwbuf, pcm_samples_per_buf, DSD_MODE_NONE, false);
    }

    if (stream != NULL) RAAT__stream_decref(stream);

    return 0;
}

void LOCKED__teardown_volume(CoreAudioOutputPlugin *self) {
    if (self->volume) {
        self->volume->teardown(self->volume->userdata);
    }
}


static RC__Status 
create_au(CoreAudioOutputPlugin *self) {
    OSStatus err = noErr;

    UInt32 size;

    //
    // Set up a HAL audio unit to talk to the device
    //
    AudioComponent            comp;
    AudioComponentDescription desc;

    desc.componentType         = kAudioUnitType_Output;
    bool is_default_output = self->unique_id == NULL || !strcmp(self->unique_id, "default");
    if (is_default_output) {
        // get the default output device id
        size = sizeof(AudioDeviceID);
        err = AudioHardwareGetProperty(kAudioHardwarePropertyDefaultOutputDevice, &size, &self->device_id);
        printstatus(self, "AudioHardwareGetProperty(kAudioHardwarePropertyDefaultOutputDevice)", err);
        if (err) return RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED;
        desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    } else {
        desc.componentSubType = kAudioUnitSubType_HALOutput;
    }
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags        = 0;
    desc.componentFlagsMask    = 0;

    comp = AudioComponentFindNext(NULL, &desc);
    if (comp == NULL) { return RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED; }

    // create output unit
    err = AudioComponentInstanceNew(comp, &self->outputunit);
    printstatus(self, "AudioComponentInstanceNew", err);
    if (err) return RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED;

    // disable input
    UInt32 enableIO = 0;
    AudioUnitSetProperty(self->outputunit,
            kAudioOutputUnitProperty_EnableIO,
            kAudioUnitScope_Input,
            1/*input element*/, 
            &enableIO,
            sizeof(enableIO));

    // enable output
    enableIO = 1;
    AudioUnitSetProperty(self->outputunit,
            kAudioOutputUnitProperty_EnableIO,
            kAudioUnitScope_Output,
            0/*output element*/,
            &enableIO,
            sizeof(enableIO));

    if (!is_default_output) {
        // set current device on the audiounit
        err = AudioUnitSetProperty(self->outputunit,
                kAudioOutputUnitProperty_CurrentDevice, 
                kAudioUnitScope_Global, 
                0, 
                &self->device_id, 
                sizeof(self->device_id));
        printstatus(self, "AudioUnitSetProperty(kAudioOutputUnitProperty_CurrentDevice)", err);
        if (err) return RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED;
    }

    CallbackState *cbstate = RC__new0(RC__ALLOCATOR_DEFAULT, CallbackState, 1);
    cbstate->self  = self;
    cbstate->token = self->current_token;
    // XXX: free cbstate at some point

    // Set up a callback function to generate output to the output unit
    AURenderCallbackStruct input;
    input.inputProc       = ev_audiounits_datacb;
    input.inputProcRefCon = cbstate;

    err = AudioUnitSetProperty (self->outputunit,
                                kAudioUnitProperty_SetRenderCallback,
                                kAudioUnitScope_Input,
                                0,
                                &input,
                                sizeof(input));
    printstatus(self, "AudioUnitSetProperty(kAudioUnitProperty_SetRenderCallback)", err);
    if (err) return RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED;;

    if (self->exclusive_mode) {
#ifdef RAAT__COREAUDIO_NADAC
        if (self->is_nadac) {
            if (!RAAT__coreaudio_nadac_set_sample_rate(self->origformat.sample_rate)) {
                return RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED;
            }
        } else 
#endif
        {
            err = set_sample_rate(self, (Float64)self->pcmformat.sample_rate);
            printstatus(self, "set_sample_rate", err);
            if (err) { 
                RAAT__WARNING("set_sample_rate(samplerate=%d, exclusive_mode=%d) failed.", (int)self->pcmformat.sample_rate, (int)self->exclusive_mode);
                return RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED;
            }
        }

        pid_t pid = getpid();
        err = AudioDeviceSetProperty(self->device_id, NULL, 0, 0, kAudioDevicePropertyHogMode, sizeof(pid), &pid);
        if (err) { 
            printstatus(self, "AudioDeviceSetProperty(kAudioDevicePropertyHogMode)", err);
            return RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_IN_USE;
        }
    }

    // Set Buffer sizes
    UInt32 actual_value;
    err = set_best_output_frames_per_buffer(self, self->device_id, &actual_value);
    printstatus(self, "set_best_output_frames_per_buffer", err);
    if (err) { return RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED; }
    RAAT__DEBUG("Debug:AudioDevice buffer set to %ld", actual_value);
    
    // add listener for dropouts 
    err = AudioDeviceAddPropertyListener(self->device_id,
                                         0,
                                         FALSE,
                                         kAudioDeviceProcessorOverload,
                                         cb_buffer_underrun,
                                         self);

    if (err == kAudioHardwareIllegalOperationError) {
        err = noErr;
    } else if (err) { 
        printstatus(self, "AudioDeviceAddPropertyListener", err);
        return RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED;
    }

    size = sizeof(actual_value);
    err =  AudioUnitSetProperty(self->outputunit,
                                kAudioUnitProperty_MaximumFramesPerSlice,
                                kAudioUnitScope_Input,
                                0,
                                &actual_value,
                                size);
    printstatus(self, "AudioUnitSetProperty(kAudioUnitProperty_MaximumFramesPerSlice)", err);
    if (err) { return err; }

    err = AudioUnitGetProperty(self->outputunit,
                               kAudioUnitProperty_MaximumFramesPerSlice,
                               kAudioUnitScope_Global,
                               0,
                               &actual_value,
                               &size);
    printstatus(self, "AudioUnitGetProperty(kAudioUnitProperty_MaximumFramesPerSlice)", err);
    if (err) return err;
    
    RAAT__DEBUG("Set kAudioUnitProperty_MaximumFramesPerSlice to %d", actual_value);

    int significant_bitspersample;

    int i;
    AudioStreamRangedDescription *goodformat = NULL;

    // first look for exact match
    for (i = 0; i < self->formatlist_len; i++) {
        AudioStreamRangedDescription *format = &self->formatlist[i];
        bool rate_ok          = false;
        bool channels_ok      = false;
        bool bitspersample_ok = false;

        if ((format->mFormat.mFormatFlags & kLinearPCMFormatFlagIsFloat) && self->pcmformat.bits_per_sample == 32) continue;       // don't accept float32 
        if (format->mFormat.mSampleRate == self->pcmformat.sample_rate) rate_ok = true;
        if (self->pcmformat.sample_rate >= format->mSampleRateRange.mMinimum && self->pcmformat.sample_rate <= format->mSampleRateRange.mMaximum) rate_ok = true;
        if (self->pcmformat.channels <= format->mFormat.mChannelsPerFrame) channels_ok = true;
        if (self->pcmformat.bits_per_sample == format->mFormat.mBitsPerChannel) bitspersample_ok = true;

        if (bitspersample_ok && rate_ok && channels_ok) {
            goodformat = format;
            break;
        }
    }

    if (goodformat == NULL) {
        // then look for compatible match
        for (i = 0; i < self->formatlist_len; i++) {
            AudioStreamRangedDescription *format = &self->formatlist[i];
            bool rate_ok          = false;
            bool channels_ok      = false;
            bool bitspersample_ok = false;

            if ((format->mFormat.mFormatFlags & kLinearPCMFormatFlagIsFloat) && self->pcmformat.bits_per_sample == 32) continue;       // don't accept float32 
            if (format->mFormat.mSampleRate == self->pcmformat.sample_rate) rate_ok = true;
            if (self->pcmformat.sample_rate >= format->mSampleRateRange.mMinimum && self->pcmformat.sample_rate <= format->mSampleRateRange.mMaximum) rate_ok = true;
            if (self->pcmformat.channels <= format->mFormat.mChannelsPerFrame) channels_ok = true;
            if (self->pcmformat.bits_per_sample <= format->mFormat.mBitsPerChannel) bitspersample_ok = true;

            if (bitspersample_ok && rate_ok && channels_ok) {
                goodformat = format;
                break;
            }
        }
    }

    self->hw_bitspersample = goodformat->mFormat.mBitsPerChannel; 
    RAAT__TRACE("set hw bitspersample to %d", self->hw_bitspersample);

    UInt32 format_flags;
    switch (self->hw_bitspersample) {
        case 16:
            format_flags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
            break;
        case 24:
            format_flags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
            break;
        case 32:
            format_flags = kAudioFormatFlagIsSignedInteger;
            break;
        default:
            RAAT__WARNING("Unknown format bps {%i}", self->pcmformat.bits_per_sample);
            return RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED;
    }

    if (self->integer_mode) {
        format_flags = kAudioFormatFlagIsNonMixable | format_flags;
    }

    // set stream format for HAL
    int frames_per_packet  = 1;
    int bytes_per_frame    = self->hw_bitspersample / 8 * self->hw_channels;
    int bytes_per_packet   = frames_per_packet * bytes_per_frame;

    AudioStreamBasicDescription streamFormat = {0,};
    streamFormat.mSampleRate       = self->pcmformat.sample_rate;   //  the sample rate of the audio stream
    streamFormat.mFormatID         = kAudioFormatLinearPCM;         //  the specific encoding type of audio stream
    streamFormat.mFormatFlags      = format_flags;                  //  flags specific to each format
    streamFormat.mBytesPerPacket   = bytes_per_packet;
    streamFormat.mFramesPerPacket  = frames_per_packet;
    streamFormat.mBytesPerFrame    = bytes_per_frame;
    streamFormat.mChannelsPerFrame = self->hw_channels;
    streamFormat.mBitsPerChannel   = goodformat->mFormat.mBitsPerChannel; 

    err = AudioUnitSetProperty(self->outputunit,
                               kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input,
                               0,
                               &streamFormat,
                               sizeof(AudioStreamBasicDescription));

    if (err == kAudioDeviceUnsupportedFormatError && self->integer_mode) {
        RAAT__DEBUG("got unsupported format in integer mode. trying without integer mode");
        streamFormat.mFormatFlags = streamFormat.mFormatFlags & ~ kAudioFormatFlagIsNonMixable;
        err = AudioUnitSetProperty(self->outputunit,
                                   kAudioUnitProperty_StreamFormat,
                                   kAudioUnitScope_Input,
                                   0,
                                   &streamFormat,
                                   sizeof(AudioStreamBasicDescription));
        printstatus(self, "AudioUnitSetProperty(kAudioUnitProperty_StreamFormat)(0)", err);
    }
    if (err) return RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED;

    /*
    RAAT__TRACE("[output/coreaudio]     %s srate=%d formatid=%08x formatflags=%08x bytesperpacket=%d framesperpacket=%d bytesperframe=%d channelsperframe=%d bitsperchannel=%d",
            "goodformat", (int)goodformat->mFormat.mSampleRate, (int)goodformat->mFormat.mFormatID, (int)goodformat->mFormat.mFormatFlags, (int)goodformat->mFormat.mBytesPerPacket, (int)goodformat->mFormat.mFramesPerPacket, (int)goodformat->mFormat.mBytesPerPacket, (int)goodformat->mFormat.mChannelsPerFrame, (int)goodformat->mFormat.mBitsPerChannel);
    RAAT__TRACE("[output/coreaudio]     %s srate=%d formatid=%08x formatflags=%08x bytesperpacket=%d framesperpacket=%d bytesperframe=%d channelsperframe=%d bitsperchannel=%d",
            "streamFormat", (int)streamFormat.mSampleRate, (int)streamFormat.mFormatID, (int)streamFormat.mFormatFlags, (int)streamFormat.mBytesPerPacket, (int)streamFormat.mFramesPerPacket, (int)streamFormat.mBytesPerPacket, (int)streamFormat.mChannelsPerFrame, (int)streamFormat.mBitsPerChannel);
    */

    if (self->exclusive_mode) {
        err = AudioDeviceSetProperty(self->device_id, NULL, 0, FALSE, kAudioDevicePropertyStreamFormat, sizeof(AudioStreamBasicDescription), &streamFormat);
        printstatus(self, "AudioUnitSetProperty(kAudioUnitProperty_StreamFormat)(1)", err);

        AudioObjectPropertyAddress propertyAddress = { 
            kAudioStreamPropertyPhysicalFormat, 
            kAudioObjectPropertyScopeGlobal, 
            kAudioObjectPropertyElementMaster 
        };
        err = AudioObjectSetPropertyData (self->stream_id, &propertyAddress, 0, NULL, sizeof(AudioStreamBasicDescription), &goodformat->mFormat);
        printstatus(self, "AudioObjectSetPropertyData(kAudioStreamPropertyPhysicalFormat)", err);
    }

    UInt32 device_latency = 0;
    UInt32 safety_offset  = 0;
    UInt32 stream_latency = 0;
    UInt32 buffer_latency = 0;
    AudioStreamID streamid;

    size = sizeof(streamid);
    err  = AudioDeviceGetProperty(self->device_id, 0, false, kAudioDevicePropertyStreams, &size, &streamid);
    printstatus(self, "AudioDeviceGetProperty(kAudioDevicePropertyStreams)", err);
    if (err) return RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED;
    if (size == sizeof(AudioStreamID)) {
        size = sizeof(UInt32);
        err  = AudioStreamGetProperty(streamid, 0, kAudioStreamPropertyLatency, &size, &stream_latency);
        if (err) {
            stream_latency = 0;
        }
    }

    size = sizeof(UInt32);
    err = AudioDeviceGetProperty(self->device_id, 0, false, kAudioDevicePropertyLatency, &size, &device_latency);
    if (err) return RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED;

    size = sizeof(UInt32);
    err = AudioDeviceGetProperty(self->device_id, 0, false, kAudioDevicePropertySafetyOffset, &size, &safety_offset);
    printstatus(self, "AudioDeviceGetProperty(kAudioDevicePropertySafetyOffset)", err);
    if (err) return RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED;

    size = sizeof(UInt32);
    err = AudioDeviceGetProperty(self->device_id, 0, false, kAudioDevicePropertyBufferFrameSize, &size, &buffer_latency);
    printstatus(self, "AudioDeviceGetProperty(kAudioDevicePropertyBufferFrameSize)", err);
    if (err) return RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED;

    self->latency_frames = device_latency + stream_latency + safety_offset;// + buffer_latency;
    
    RAAT__DEBUG("Sample rate: %f, bits: %d, channels: %d, bytes per frame: %d, bytes per packet: %d, frames per packet: %d device latency: %d stream latency: %d, safety offset: %d, buffer latency: %d, latency: %d",
            streamFormat.mSampleRate, (int)streamFormat.mBitsPerChannel, (int)streamFormat.mChannelsPerFrame,
            (int)streamFormat.mBytesPerFrame, (int)streamFormat.mBytesPerPacket, (int)streamFormat.mFramesPerPacket, 
            device_latency, stream_latency, safety_offset, buffer_latency,
            self->latency_frames);

    self->origformat_samples_per_buf = 0;
    self->streamsample               = 0;

    // set up streaming stuff
    self->pcm_samples_per_buf   = actual_value;

    if (self->origformat.sample_type == RAAT__SAMPLE_TYPE_PCM) {
        self->origformat_samples_per_buf = self->pcm_samples_per_buf;
    } else if (self->origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) {
        self->origformat_samples_per_buf = self->pcm_samples_per_buf * 16;
    } else {
        RC__ASSERT(false);
    }

    self->ns_per_buf        = (int)RAAT__stream_format_samples_to_ns(&self->pcmformat, self->pcm_samples_per_buf);
    self->bytes_per_origbuf = RAAT__stream_format_compute_buffer_size(&self->origformat, self->origformat_samples_per_buf);
    self->bytes_per_pcmbuf  = RAAT__stream_format_compute_buffer_size(&self->pcmformat, self->pcm_samples_per_buf);
    self->bytes_per_hwbuf   = self->hw_channels * self->hw_bitspersample / 8 * self->pcm_samples_per_buf;
    self->streamsample      = 0;

    //RAAT__TRACE("samples per buf %d ns per buf %d samplerate %d", self->pcm_samples_per_buf, ns_per_buf, format.sample_rate);
    RAAT__TRACE("%d samples per buf, %d bytes per origbuf, %d bytes per pcmbuf, %d bytes per hwbuf", self->pcm_samples_per_buf, self->bytes_per_origbuf, self->bytes_per_pcmbuf, self->bytes_per_hwbuf);

    if (self->pcmbuf != NULL) {
        RC__free(self->alloc, self->pcmbuf);
        self->pcmbuf = NULL;
    }
    if (self->origbuf != NULL) {
        RC__free(self->alloc, self->origbuf);
        self->origbuf = NULL;
    }
    if (self->dsdbuf != NULL) {
        RC__free(self->alloc, self->dsdbuf);
        self->dsdbuf = NULL;
    }
    self->origbuf    = RC__alloc(RC__ALLOCATOR_DEFAULT, self->bytes_per_origbuf);
    self->pcmbuf     = RC__alloc(RC__ALLOCATOR_DEFAULT, self->bytes_per_pcmbuf);

    if (self->origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) {
        // Both DoP and Dcs packing requires 3 bytes of 24bit PCM data for every 2 bytes of DSD data
        self->dsdbuf = RC__alloc(RC__ALLOCATOR_DEFAULT, self->bytes_per_origbuf * 3 / 2);
    }

    return RC__STATUS_SUCCESS;
}

static RC__Status output_force_teardown(void *vself, json_t *reason) {
    CoreAudioOutputPlugin *self = (CoreAudioOutputPlugin*)vself;

    RC__Status status = RC__STATUS_SUCCESS;
    int        token = RAAT__OUTPUT_TOKEN_INVALID;

    RAAT__OutputLostCallback  old_cb_lost          = NULL;
    void                     *old_cb_lost_userdata = NULL;

    uv_mutex_lock(&self->lock);
retry:
    self->current_token = token = ++self->next_token;

    if (self->state != IDLE) {
        RAAT__TRACE("[coreaudio] kicking off old output in force teardown");
        self->state            = IDLE;
        old_cb_lost            = self->cb_lost;
        old_cb_lost_userdata   = self->cb_lost_userdata;
        self->cb_lost          = NULL;
        self->cb_lost_userdata = NULL;
        LOCKED_teardown_coreaudio(self);
        LOCKED__teardown_volume(self);
    }
    uv_mutex_unlock(&self->lock);

    if (old_cb_lost != NULL) {
        old_cb_lost(old_cb_lost_userdata, reason);
    }

    uv_mutex_lock(&self->lock);
    if (self->current_token != token) {
        RAAT__TRACE("[coreaudio] force teardown required a retry");
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

#if 0
static void coreaudio_output_thread(void *vself) {
}
#endif

static void output_setup(void *vself, RAAT__StreamFormat *format, RAAT__OutputSetupCallback cb_setup, void *cb_setup_userdata, RAAT__OutputLostCallback cb_lost, void *cb_lost_userdata) {
    CoreAudioOutputPlugin *self = (CoreAudioOutputPlugin*)vself;
    RC__Status status = RC__STATUS_SUCCESS;
    int        token = RAAT__OUTPUT_TOKEN_INVALID;

    RAAT__OutputLostCallback  old_cb_lost          = NULL;
    void                     *old_cb_lost_userdata = NULL;

    char formatstr[RAAT__STREAM_FORMAT_MAX_STRLEN];

    uv_mutex_lock(&self->lock);
    // before we do anything else, kick off anyone currently using the device
    self->current_token = token = ++self->next_token;

    if (self->state != IDLE) {
        RAAT__TRACE("[coreaudio] kicking off old output");
        self->state            = IDLE;
        old_cb_lost            = self->cb_lost;
        old_cb_lost_userdata   = self->cb_lost_userdata;
        self->cb_lost          = NULL;
        self->cb_lost_userdata = NULL;
        LOCKED_teardown_coreaudio(self);
        LOCKED__teardown_volume(self);
    }
    uv_mutex_unlock(&self->lock);

    if (old_cb_lost != NULL) {
        json_t *reason = json_object();
        json_object_set_new(reason, "reason", json_string("setup"));
        old_cb_lost(old_cb_lost_userdata, reason);
        json_decref(reason);
    }

    uv_mutex_lock(&self->lock);

    if (self->current_token != token) {
        RAAT__TRACE("[output/coreaudio] setup: lost the race");
        status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED;
        goto fail;
    }

    // clean up leftover stream 
    if (self->stream != NULL) {
        RAAT__stream_decref(self->stream);
        self->stream = NULL;
    }

    RAAT__stream_format_to_string(format, formatstr);
    RAAT__TRACE("[output/coreaudio] setup: format is %s", formatstr);

    RAAT__StreamFormat *formats;
    size_t              nformats;
    RAAT__StreamFormat  pcmformat;
    status = output_get_supported_formats(self, RC__ALLOCATOR_DEFAULT, &nformats, &formats);
    if (RC__STATUS_IS_SUCCESS(status)) {
        bool found = false;
        size_t i;

        bool prefer_larger_samples = self->volume != NULL && format->sample_type == RAAT__SAMPLE_TYPE_PCM && format->bits_per_sample < 32;

        if (prefer_larger_samples) {
            RAAT__StreamFormat format32 = *format; format32.bits_per_sample = 32;
            RAAT__StreamFormat format24 = *format; format24.bits_per_sample = 24;
            switch (format->bits_per_sample) {
                case 24:
                    for (i = 0; !found && i < nformats; i++) {
                        if (RAAT__stream_format_equals(&format32, &formats[i])) { found = true; pcmformat = format32; break; }
                    }
                    break;
                case 16:        
                    for (i = 0; !found && i < nformats; i++) {
                        if (RAAT__stream_format_equals(&format32, &formats[i])) { found = true; pcmformat = format32; break; }
                    }
                    for (i = 0; !found && i < nformats; i++) {
                        if (RAAT__stream_format_equals(&format24, &formats[i])) { found = true; pcmformat = format24; break; }
                    }
                    break;
            }
        }

        for (i = 0; !found && i < nformats; i++) {
            if (RAAT__stream_format_equals(format, &formats[i])) { found = true; pcmformat = *format; break; }
        }

        RC__free(RC__ALLOCATOR_DEFAULT, formats);

        if (found) {
            if (pcmformat.sample_type == RAAT__SAMPLE_TYPE_DSD) {
                pcmformat.sample_type     = RAAT__SAMPLE_TYPE_PCM;
                pcmformat.sample_rate     = format->sample_rate / 16;
                pcmformat.bits_per_sample = 24;
                pcmformat.channels        = format->channels;
            }

            RAAT__TRACE("[coreaudio] opening %d/%d/%d", pcmformat.sample_rate, pcmformat.bits_per_sample, pcmformat.channels);

            self->pcmformat             = pcmformat;
            self->hw_bitspersample      = self->pcmformat.bits_per_sample;
            self->current_token = token = ++self->next_token;
            self->origformat            = *format;
            self->state                 = STOPPED;

            status = create_au(self);
            if (!RC__STATUS_IS_SUCCESS(status)) goto fail;
                
            OSStatus err;
            RAAT__TRACE("starting output");
            err = AudioUnitInitialize(self->outputunit);
            printstatus(self, "AudioUnitInitialize", err);
            if (err) { status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED; goto fail; }

            RAAT__drift_correction_init(&self->drift_correction, self->log, format);
            self->cb_lost               = cb_lost;
            self->cb_lost_userdata      = cb_lost_userdata;
            status                      = RC__STATUS_SUCCESS;

            LOCKED_update_shared_volume(self);

            if (self->force_max_volume) {
                LOCKED_force_max_volume(self);
            }

            err = AudioOutputUnitStart(self->outputunit);
            printstatus(self, "AudioOutputUnitStart", err);
            if (err) { status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED; goto fail; }

            LOCKED_update_signal_path(self);
        } else {
            status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED;
        }
    }

fail:

    if (status == RC__STATUS_SUCCESS) {
        self->resync_delay_remaining_ns = self->resync_delay_secs * 1000000000LL;
        self->setup_cb          = cb_setup;
        self->setup_cb_userdata = cb_setup_userdata;
    } 
    uv_mutex_unlock(&self->lock);
    // mutexes might not be recursive, so the following happen outside of the lock.

    if (self->volume && status == RC__STATUS_SUCCESS) {
        if (format->sample_type == RAAT__SAMPLE_TYPE_DSD) {
            self->volume->setup(self->volume->userdata, format, NULL);
        } else {
            self->volume->setup(self->volume->userdata, &pcmformat, NULL);
        }
    }

    if (status != RC__STATUS_SUCCESS) {
        // call our setup callback
        cb_setup(cb_setup_userdata, status, token);
    }
}

static RC__Status output_teardown(void *vself, int token) {
    CoreAudioOutputPlugin *self = (CoreAudioOutputPlugin*)vself;
    RC__Status status = RC__STATUS_UNEXPECTED_ERROR;

    uv_mutex_lock(&self->lock);
    if (self->current_token == token) {
        RAAT__TRACE("[coreaudio] teardown");
        self->current_token = RAAT__OUTPUT_TOKEN_INVALID;
        self->state = IDLE;
        self->cb_lost = NULL;
        self->cb_lost_userdata = NULL;

        if (self->stream != NULL) {
            RAAT__stream_decref(self->stream);
            self->stream = NULL;
        }

        LOCKED_teardown_coreaudio(self);
        LOCKED__teardown_volume(self);

        status = RC__STATUS_SUCCESS;
    } else {
        status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_TOKEN;
    }
    uv_mutex_unlock(&self->lock);

    return status;
}

/*
static int pausePlayback(CoreAudioOutputPlugin *self) {
    OSStatus err;
    AudioOutputUnitStop(self->outputunit);

    err = AudioUnitUninitialize(self->outputunit);
    printstatus(self, "AudioUnitUninitialize()", err);
    if (err) return 1;

    if (self->exclusive_mode) {
        pid_t pid = 0;
        err = AudioDeviceSetProperty(self->device_id, NULL, 0, 0, kAudioDevicePropertyHogMode, sizeof(pid), &pid);
        printstatus(self, "AudioDeviceSetProperty(kAudioDevicePropertyHogMode)", err);
    }

    return 0;
}

static int unpausePlayback(CoreAudioOutputPlugin *self) {
    return 0;
}
*/


static RC__Status output_stop(void *vself, int token) {
    CoreAudioOutputPlugin *self = (CoreAudioOutputPlugin*)vself;
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
    CoreAudioOutputPlugin *self = (CoreAudioOutputPlugin*)vself;
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

static RC__Status output_get_output_delay(void *vself, int token, int64_t *out_delay) {
    CoreAudioOutputPlugin *self = (CoreAudioOutputPlugin*)vself;
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

static int64_t LOCKED_get_local_time(CoreAudioOutputPlugin *self) {
    int64_t now = RC__now_ns();
    int64_t last_wall_sample = self->last_monotonic_sample;
    return RAAT__stream_format_samples_to_ns(&self->origformat, last_wall_sample) + (now - self->last_monotonic_sample_systime);
}

static RC__Status output_get_local_time(void *vself, int token, int64_t *out_time) {
    CoreAudioOutputPlugin *self = (CoreAudioOutputPlugin*)vself;
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
    CoreAudioOutputPlugin *self = (CoreAudioOutputPlugin*)vself;
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
    CoreAudioOutputPlugin *self = (CoreAudioOutputPlugin*)vself;
    return RAAT__output_message_listeners_add(&self->message_listeners, cb, cb_userdata);
}

static RC__Status 
output_remove_message_listener(void *vself, RAAT__OutputMessageCallback cb, void *cb_userdata) {
    CoreAudioOutputPlugin *self = (CoreAudioOutputPlugin*)vself;
    return RAAT__output_message_listeners_remove(&self->message_listeners, cb, cb_userdata);
}

static RC__Status 
output_send_message(void *vself, json_t *message) {
    CoreAudioOutputPlugin *self = (CoreAudioOutputPlugin*)vself;
    char *s = json_dumps(message, 0);
    RAAT__TRACE("[coreaudio] GOT MESSAGE %s", s);
    free(s);

    return RC__STATUS_SUCCESS;
}

static RC__Status 
output_set_software_volume_signal_path(void *vself, json_t *soft_volume_signal_path) {
    CoreAudioOutputPlugin *self = (CoreAudioOutputPlugin*)vself;
    uv_mutex_lock(&self->lock);
    if (self->soft_volume_signal_path) json_decref(self->soft_volume_signal_path);
    self->soft_volume_signal_path = json_incref(soft_volume_signal_path);
    LOCKED_update_signal_path(self);
    uv_mutex_unlock(&self->lock);
    return RC__STATUS_SUCCESS;
}

static RC__Status 
output_set_software_volume(void *vself, RAAT__OutputSoftwareVolume *volume) {
    CoreAudioOutputPlugin *self = (CoreAudioOutputPlugin*)vself;
    uv_mutex_lock(&self->lock);
    self->volume          = volume;
    uv_mutex_unlock(&self->lock);
    return RC__STATUS_SUCCESS;
}

RC__Status 
RAAT__coreaudio_output_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__OutputPlugin **out_output) {
    OSStatus err;
    UInt32 size;

    alloc = RC__allocator_default(alloc);
    CoreAudioOutputPlugin *self            = RC__new0(alloc, CoreAudioOutputPlugin, 1);
    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;

    const char *unique_id = json_string_value(json_object_get(config, "device"));
    if (unique_id == NULL) unique_id = "default";


    /*
     * XXX is this really needed? or just for volume notifications
    CFRunLoopRef theRunLoop = CFRunLoopGetCurrent();
    AudioObjectPropertyAddress theAddress = { kAudioHardwarePropertyRunLoop, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster };
    err = AudioObjectSetPropertyData (kAudioObjectSystemObject, &theAddress, 0, NULL, sizeof(CFRunLoopRef), &theRunLoop);
    printstatus(self, "AudioUnitSetParameter(AudioObjectSetPropertyData(kAudioObjectPropertyElementMaster))", err);
    */

    if (!strcmp(unique_id, "default")) {
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

    // initialize the state that is associated with CoreAudioOutputPlugin 
    self->alloc                        = alloc;
    self->log                          = RAAT__device_get_log(device);
    self->current_token                = RAAT__OUTPUT_TOKEN_INVALID;
    self->next_token                   = 1;
    self->state                        = IDLE;
    self->unique_id                    = unique_id == NULL ? NULL : RC__allocator_strdup(alloc, unique_id);

    RAAT__TRACE("[output/coreaudio] initializing %s", self->unique_id ? self->unique_id : "System Output");

#ifdef RAAT__COREAUDIO_NADAC
    self->is_nadac = RAAT__coreaudio_nadac_is_nadac(self->device_id);
    if (self->is_nadac) RAAT__TRACE("[output/coreaudio] device is NADAC");
#endif

    json_t *buffer_duration = json_object_get(config, "buffer_duration");
    if (buffer_duration) self->buffer_duration_secs = json_number_value(buffer_duration);
    if (self->buffer_duration_secs == 0) self->buffer_duration_secs = .050;
    RAAT__TRACE("[output/coreaudio] preferred buffer duration=%fs", self->buffer_duration_secs);

    if (json_is_true(json_object_get(config, "use_max_buffer_size")))
        self->use_max_buffer_size = true;
    RAAT__TRACE("[output/coreaudio] use max buffer size=%d", self->use_max_buffer_size);
    
    if (json_is_true(json_object_get(config, "power_of_two_buffer_size")))
        self->power_of_two_buffer_size = true;
    RAAT__TRACE("[output/coreaudio] power of two buffer size=%d", self->power_of_two_buffer_size);

    if (json_is_true(json_object_get(config, "force_max_volume")))
        self->force_max_volume = true;
    RAAT__TRACE("[output/coreaudio] force_max_volume=%d", self->force_max_volume);

    json_t *custom_signal_path = json_object_get(config, "signal_path");
    if (custom_signal_path) {
        self->custom_signal_path = json_copy(custom_signal_path);
    }

    json_t *resync_delay = json_object_get(config, "resync_delay");
    if (resync_delay) self->resync_delay_secs = json_number_value(resync_delay);
    if (self->resync_delay_secs == 0) self->resync_delay_secs = 0.1;
    RAAT__TRACE("[output/coreaudio] resync delay=%f", self->resync_delay_secs);

    json_t *exclusive_mode = json_object_get(config, "exclusive_mode");
    if (exclusive_mode) self->exclusive_mode = json_boolean_value(exclusive_mode);
    RAAT__TRACE("[output/coreaudio] exclusivemode=%d", self->exclusive_mode);

    json_t *integer_mode = json_object_get(config, "integer_mode");
    if (integer_mode) self->integer_mode = json_boolean_value(integer_mode);
    RAAT__TRACE("[output/coreaudio] integermode=%d", self->integer_mode);

    json_t *max_dsd_rate = json_object_get(config, "max_dsd_rate");
    if (max_dsd_rate) self->max_dsd_rate = json_number_value(max_dsd_rate);
    if (self->max_dsd_rate == 0) self->max_dsd_rate = 512;
    RAAT__TRACE("[output/coreaudio] max dsd rate=%d", self->max_dsd_rate);

    if (self->exclusive_mode) {
        const char *dsd_mode_s     = json_string_value(json_object_get(config, "dsd_mode"));
        if (dsd_mode_s)
        RAAT__TRACE("[output/coreaudio] dsd_mode_s=%s", dsd_mode_s);
        if      (dsd_mode_s && !strcmp(dsd_mode_s, "dop")) self->dsd_mode = DSD_MODE_DOP;
        else if (dsd_mode_s && !strcmp(dsd_mode_s, "dcs")) self->dsd_mode = DSD_MODE_DCS;
        else                                               self->dsd_mode = DSD_MODE_NONE;
        RAAT__TRACE("[output/coreaudio] dsd_mode=%d", self->dsd_mode);
    }

    // get streams list 
    do {
        OSStatus err;
        UInt32 dataSize = 0;

        AudioObjectPropertyAddress propertyAddress = { 
            kAudioHardwarePropertyDevices, 
            kAudioObjectPropertyScopeGlobal, 
            kAudioObjectPropertyElementMaster 
        };

        dataSize = 0;
        propertyAddress.mSelector = kAudioDevicePropertyStreams;
        err = AudioObjectGetPropertyDataSize(self->device_id, &propertyAddress, 0, NULL, &dataSize);
        printstatus(self, "AudioObjectGetPropertyDataSize (kAudioDevicePropertyStreams)", err);
        if (kAudioHardwareNoError != err) break;

        AudioStreamID *streamlist = (AudioStreamID*)malloc(dataSize);
        err = AudioObjectGetPropertyData(self->device_id, &propertyAddress, 0, NULL, &dataSize, streamlist);
        printstatus(self, "AudioObjectGetPropertyData (kAudioDevicePropertyStreams)", err);
        if (kAudioHardwareNoError != err) break;


        /*
         * Get channel count by looking at stream configuration
         */
        AudioBufferList     *localOutputBuffer; 
        Boolean writable;
        err = AudioDeviceGetPropertyInfo(self->device_id, 0, false, kAudioDevicePropertyStreamConfiguration, &size, &writable);
        printstatus(self, "AudioObjectGetPropertyDataSize (kAudioDevicePropertyStreamConfiguration)", err);
        if (kAudioHardwareNoError != err) break;

        localOutputBuffer = (AudioBufferList*)malloc(size); 
        err = AudioDeviceGetProperty(self->device_id, 0, false, kAudioDevicePropertyStreamConfiguration, &size, localOutputBuffer);
        printstatus(self, "AudioObjectGetPropertyDataSize (kAudioDevicePropertyStreamConfiguration)", err);
        if (kAudioHardwareNoError != err) break;

        self->hw_channels = 0;
        int buf;
        for (buf = 0; buf < localOutputBuffer->mNumberBuffers; buf++) {
            self->hw_channels += localOutputBuffer->mBuffers[buf].mNumberChannels;
        }
        RAAT__TRACE("[output/coreaudio] got %d channels", self->hw_channels);
        free(localOutputBuffer);


        int i;
        int nstreamids = dataSize / sizeof(AudioStreamID);
        for (i = 0; i < nstreamids; i++) {
            int j;

            self->stream_id = streamlist[i];

            /*
             * Physical Formats
             */

            if (self->exclusive_mode) {
                dataSize = 0;
                propertyAddress.mSelector = kAudioStreamPropertyAvailablePhysicalFormats;
                err = AudioObjectGetPropertyDataSize(streamlist[i], &propertyAddress, 0, NULL, &dataSize);
                printstatus(self, "AudioObjectGetPropertyDataSize (kAudioStreamPropertyAvailablePhysicalFormats)", err);
                if (kAudioHardwareNoError != err) break;

                self->formatlist = (AudioStreamRangedDescription*)RC__alloc(alloc, dataSize);
                err = AudioObjectGetPropertyData(streamlist[i], &propertyAddress, 0, NULL, &dataSize, self->formatlist);
                printstatus(self, "AudioObjectGetPropertyData (kAudioStreamPropertyAvailablePhysicalFormats)", err);
                if (kAudioHardwareNoError != err) continue;
            } else {
                dataSize = 0;
                propertyAddress.mSelector = kAudioStreamPropertyAvailableVirtualFormats;
                err = AudioObjectGetPropertyDataSize(streamlist[i], &propertyAddress, 0, NULL, &dataSize);
                printstatus(self, "AudioObjectGetPropertyDataSize (kAudioStreamPropertyAvailableVirtualFormats)", err);
                if (kAudioHardwareNoError != err) break;

                self->formatlist = (AudioStreamRangedDescription*)RC__alloc(alloc, dataSize);
                err = AudioObjectGetPropertyData(streamlist[i], &propertyAddress, 0, NULL, &dataSize, self->formatlist);
                printstatus(self, "AudioObjectGetPropertyData (kAudioStreamPropertyAvailableVirtualFormats)", err);
                if (kAudioHardwareNoError != err) continue;
            }

            self->formatlist_len = dataSize / sizeof(AudioStreamRangedDescription);

            for (j = 0; j < self->formatlist_len; j++) {
                AudioStreamRangedDescription *format = &self->formatlist[j];

                format->mFormat.mChannelsPerFrame = self->hw_channels;

                if (format->mFormat.mChannelsPerFrame == 1) {
                    RAAT__TRACE("[output/coreaudio] HACK: treating a 1-ch stream as a 2-ch stream");
                    format->mFormat.mChannelsPerFrame = 2;
                }
                
                RAAT__TRACE("[output/coreaudio]     %s srate min=%d max=%d srate=%d formatid=%08x formatflags=%08x bytesperpacket=%d framesperpacket=%d bytesperframe=%d channelsperframe=%d bitsperchannel=%d",
                        self->exclusive_mode ? "(phys)" : "(virt)",
                        (int)format->mSampleRateRange.mMinimum,
                        (int)format->mSampleRateRange.mMaximum,
                        (int)format->mFormat.mSampleRate,
                        (int)format->mFormat.mFormatID,
                        (int)format->mFormat.mFormatFlags,
                        (int)format->mFormat.mBytesPerPacket,
                        (int)format->mFormat.mFramesPerPacket,
                        (int)format->mFormat.mBytesPerPacket,
                        (int)format->mFormat.mChannelsPerFrame,
                        (int)format->mFormat.mBitsPerChannel);
            }

            break;              // Roon only uses stream 0 (i know, i know)
        }
    } while (0);

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

    uv_mutex_init(&self->lock);
    RAAT__TRACE("[output/coreaudio] initialized");
    *out_output = &self->plugin;
    return RC__STATUS_SUCCESS;
}

void
RAAT__coreaudio_output_plugin_delete(RAAT__OutputPlugin *output) {
    CoreAudioOutputPlugin *self = (CoreAudioOutputPlugin*)output;

    uv_mutex_lock(&self->lock);
    if (self->state != IDLE) {
        LOCKED_teardown_coreaudio(self);
        LOCKED__teardown_volume(self);
    }
    if (self->stream != NULL) {
        RAAT__stream_decref(self->stream);
        self->stream = NULL;
    }
    uv_mutex_unlock(&self->lock);

    uv_mutex_destroy(&self->lock);

    RC__free(self->alloc, self->formatlist);

#ifdef RAAT__COREAUDIO_NADAC
    if (self->is_nadac) RAAT__coreaudio_nadac_restore();
#endif

    if (self->info)                    json_decref(self->info);
    if (self->custom_signal_path)      json_decref(self->custom_signal_path);
    if (self->soft_volume_signal_path) json_decref(self->soft_volume_signal_path);

    RAAT__output_message_listeners_destroy(&self->message_listeners);
    RC__free(self->alloc, self);
}

bool RAAT__coreaudio_output_get_usb_id(const char *device_uid, char *out_usb_id/*[10]*/) {
    CFMutableDictionaryRef matchingDict = NULL;
    CFMutableDictionaryRef props        = NULL;
    io_iterator_t iter;
    kern_return_t kr;
    io_service_t device;
    bool found = false;
    
    CFStringRef desired_unique_id = CFStringCreateWithCString(kCFAllocatorDefault, device_uid, kCFStringEncodingUTF8);

    {
        /* set up a matching dictionary for the class */
        matchingDict = IOServiceMatching(kIOAudioEngineClassName);
        if (matchingDict == NULL) {
            goto cleanup;
        }

        /* Now we have a dictionary, get an iterator.*/
        kr = IOServiceGetMatchingServices(kIOMasterPortDefault, matchingDict, &iter);
        if (kr != KERN_SUCCESS) {
            goto cleanup;
        }

        /* iterate */
        while ((device = IOIteratorNext(iter))) {
            kr = IORegistryEntryCreateCFProperties(device, &props, kCFAllocatorDefault, kNilOptions);
            if(kr == KERN_SUCCESS) {
                bool matched = false;
                const void *unique_id = CFDictionaryGetValue(props, CFSTR("IOAudioEngineGlobalUniqueID"));

                if (unique_id && CFGetTypeID(unique_id) == CFStringGetTypeID() && kCFCompareEqualTo == CFStringCompare(unique_id, desired_unique_id, 0)) {
                    matched = true;
                    const void *vendor_id  = CFDictionaryGetValue(props, CFSTR("idVendor"));
                    const void *product_id = CFDictionaryGetValue(props, CFSTR("idProduct"));
                    if (vendor_id && product_id && CFGetTypeID(vendor_id) == CFNumberGetTypeID() && CFGetTypeID(product_id) == CFNumberGetTypeID()) {
                        int vendor_id_num = 0, product_id_num = 0;
                        if (CFNumberGetValue(vendor_id,  kCFNumberSInt32Type, (void*)&vendor_id_num) && 
                            CFNumberGetValue(product_id, kCFNumberSInt32Type, (void*)&product_id_num)) {
                            snprintf(out_usb_id, 10, "%04x:%04x", vendor_id_num, product_id_num);
                            found = true;
                        }
                    }
                }

                CFRelease(props);
                if (matched) goto cleanup;
            }

            /* And free the reference taken before continuing to the next item */
            IOObjectRelease(device);
        }

        /* Done, release the iterator */
        IOObjectRelease(iter);
    }

cleanup:
    if (desired_unique_id) CFRelease(desired_unique_id);
    return found;
}

