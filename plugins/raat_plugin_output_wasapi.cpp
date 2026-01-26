//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_plugin_output_wasapi.h"
#include "raat_dsp.h"
#include "rc_list.h"

#include <uv.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

#define INITGUID
#include <Audioclient.h>
#include <Audiopolicy.h>
#include <Avrt.h>
#include <Mmdeviceapi.h>
#include <Mfapi.h>
#include <Mfidl.h>
#include <Endpointvolume.h>
#include <Propsys.h>
#include <FunctionDiscoveryKeys_devpkey.h>

#define RAAT__CURRENT_LOG self->log

/*
 * Output Plugin
 */

typedef enum {
    IDLE = 0,
    STOPPED = 1,
    RUNNING = 2,
} WasapiOutputState;

/*
 * constants
 */
const int PUSH_BUFS_PER_SEC = 4;
const int REFTIMES_PER_MILLISEC = 10000;

/*
 * Utilities
 */
template <class T> void SafeRelease(T **ppT) {
    if (*ppT) {
        (*ppT)->Release();
        *ppT = NULL;
    }
}

class WasapiOutput_DefaultDeviceNotifications;

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

    WasapiOutputState            state;

    json_t                      *custom_signal_path;

    int                          next_token;
    int                          current_token;
    RAAT__OutputLostCallback     cb_lost;
    void                        *cb_lost_userdata;

    RAAT__Stream                *stream;

    int64_t                      last_monotonic_sample_systime;  // in ns (RC__now_ns())
    int64_t                      last_monotonic_sample;          // in samples, based on counter

    int64_t                      start_streamsample;
    int64_t                      start_time;
    bool                         new_stream;

    int64_t                      last_delay_ns;

    DsdMode                      dsd_mode;
    int                          max_dsd_rate;

    double                         resync_delay_secs;
    double                         buffer_duration_secs;

    RAAT__DriftCorrection        drift_correction;
    RAAT__OutputSoftwareVolume  *volume;
    json_t                      *info;

        bool                         force_max_volume;               

    // WASAPI data
    IMMDeviceEnumerator            *device_enumerator;
    IMMDevice                    *device;
    IPropertyStore                *deviceprops;

    // output thread
    uv_thread_t                     tid;

    char                        *device_id;
    char                        *device_name;
    wchar_t                     *device_id_wide;
    bool                         event_driven_mode;
    bool                         nasty_format_probe;
    bool                         exclusive_mode;
    bool                         default_device;

    RAAT__StreamFormat             origformat;
    RAAT__StreamFormat             pcmformat;

    // format probe data
    RAAT__StreamFormat          *supported_formats;
    WAVEFORMATEXTENSIBLE        *supported_waveformats;        // parallel with supported_formats
    int                          n_supported_formats;

    // for handling default device changes (only when "device": "default")
    WasapiOutput_DefaultDeviceNotifications  *default_device_notifications;
} WasapiOutputPlugin;


static RC__Status output_get_info(void *vself, json_t **out_info) {
    WasapiOutputPlugin *self = (WasapiOutputPlugin*)vself;

    RC__ASSERT(self);
    RC__ASSERT(out_info);

    json_incref(self->info);
    *out_info = self->info;

    return RC__STATUS_SUCCESS;
}

static void force_max_volume(WasapiOutputPlugin *self) {
    IMMDevice            *device                   = NULL;
    IAudioEndpointVolume *volume                   = NULL;
    IMMDeviceEnumerator  *device_enumerator        = NULL;

    HRESULT hr;

    CoInitialize(NULL);

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&device_enumerator);
    if (!SUCCEEDED(hr)) {
        goto failexit;
    }
    hr = device_enumerator->GetDevice(self->device_id_wide, &device);
    if (!SUCCEEDED(hr)) {
        goto failexit;
    }
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&volume);
    if (!SUCCEEDED(hr)) {
        goto failexit;
    }

    volume->SetMasterVolumeLevelScalar(1.0, NULL);
    volume->SetMute(FALSE, NULL);

failexit:
    SafeRelease(&device);
    SafeRelease(&device_enumerator);
    SafeRelease(&volume);
}


static void update_signal_path(WasapiOutputPlugin *self) {
    json_t *message = json_object();
    json_t *signal_path = json_array();

    if (self->custom_signal_path) {
        size_t index;
        json_t *value;

        json_array_foreach(self->custom_signal_path, index, value) {
            json_t *condition = json_object_get(value, "condition");
            if (condition) {
                json_t *j_sample_rate = json_object_get(condition, "sample_rate");
                if (j_sample_rate && json_integer_value(j_sample_rate) != self->origformat.sample_rate)
                    continue;

                json_t *j_bits_per_sample = json_object_get(condition, "bits_per_sample");
                if (j_bits_per_sample && json_integer_value(j_bits_per_sample) != self->origformat.bits_per_sample)
                    continue;

                json_t *j_channels = json_object_get(condition, "channels");
                if (j_channels && json_integer_value(j_channels) != self->origformat.channels)
                    continue;

                json_t *copy = json_deep_copy(value);
                json_object_del(copy, "condition");
                json_array_append_new(signal_path, copy);

            }
            else {
                json_array_append_new(signal_path, json_deep_copy(value));
            }
        }
    }
    else {
        if (self->origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) {
            switch (self->dsd_mode) {
            case DSD_MODE_NONE: break;

            case DSD_MODE_DOP: {
                json_t *encapsulate = json_object();
                json_object_set_new(encapsulate, "type", json_string("dsd_encapsulate"));
                json_object_set_new(encapsulate, "quality", json_string("high"));
                json_object_set_new(encapsulate, "method", json_string("dop"));
                json_array_append_new(signal_path, encapsulate);
            } break;

            case DSD_MODE_DCS: {
                json_t *encapsulate = json_object();
                json_object_set_new(encapsulate, "type", json_string("dsd_encapsulate"));
                json_object_set_new(encapsulate, "quality", json_string("high"));
                json_object_set_new(encapsulate, "method", json_string("dcs"));
                json_array_append_new(signal_path, encapsulate);
            } break;
            }
        }

        json_t *output = json_object();
        json_object_set_new(output, "type", json_string("output"));
        
        if (self->exclusive_mode) {
            json_object_set_new(output, "method", json_string("wasapi_exclusive"));
            json_object_set_new(output, "quality", json_string("lossless"));
        } else {
            json_object_set_new(output, "method", json_string("wasapi_shared"));
            json_object_set_new(output, "quality", json_string("high"));
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

static RC__Status output_get_supported_formats(void *vself, RC__Allocator *alloc, size_t *out_nformats, RAAT__StreamFormat/*?*/ **out_formats) {
    WasapiOutputPlugin *self = (WasapiOutputPlugin*)vself;
    RAAT__StreamFormat *formats = RC__new0(alloc, RAAT__StreamFormat, self->n_supported_formats);
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
    *out_formats = formats;

    return RC__STATUS_SUCCESS;
}

static RC__Status output_force_teardown(void *vself, json_t *reason) {
    WasapiOutputPlugin *self = (WasapiOutputPlugin*)vself;

    RC__Status status = RC__STATUS_SUCCESS;
    int        token = RAAT__OUTPUT_TOKEN_INVALID;

    RAAT__OutputLostCallback  old_cb_lost = NULL;
    void                     *old_cb_lost_userdata = NULL;
    bool                      needs_join = false;

    uv_mutex_lock(&self->lock);
retry:
    self->current_token = token = ++self->next_token;

    if (self->state != IDLE) {
        RAAT__TRACE("[output/wasapi] kicking off old output in force teardown");
        needs_join = true;
        self->state = IDLE;
        old_cb_lost = self->cb_lost;
        old_cb_lost_userdata = self->cb_lost_userdata;
        self->cb_lost = NULL;
        self->cb_lost_userdata = NULL;
    }
    uv_mutex_unlock(&self->lock);

    if (needs_join) {
        RAAT__TRACE("[output/wasapi] joining thread in force teardown");
        uv_thread_join(&self->tid);
    }

    if (old_cb_lost != NULL) {
        old_cb_lost(old_cb_lost_userdata, reason);
    }

    uv_mutex_lock(&self->lock);
    if (self->current_token != token) {
        RAAT__TRACE("[output/wasapi] force teardown required a retry");
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

static RC__Status on_output_lost(void *vself, json_t *reason) {
    WasapiOutputPlugin *self = (WasapiOutputPlugin*)vself;

    RC__Status status = RC__STATUS_SUCCESS;
    int        token = RAAT__OUTPUT_TOKEN_INVALID;

    RAAT__OutputLostCallback  old_cb_lost = NULL;
    void                     *old_cb_lost_userdata = NULL;
    bool                      needs_join = false;

    uv_mutex_lock(&self->lock);
    if (self->state != IDLE) {
        self->state = IDLE;
        old_cb_lost = self->cb_lost;
        old_cb_lost_userdata = self->cb_lost_userdata;
        self->cb_lost = NULL;
        self->cb_lost_userdata = NULL;
        if (self->stream != NULL) {
            RAAT__stream_decref(self->stream);
            self->stream = NULL;
        }
    }
    uv_mutex_unlock(&self->lock);

    if (old_cb_lost != NULL) {
        old_cb_lost(old_cb_lost_userdata, reason);
    }

    return status;
}

typedef struct {
    WasapiOutputPlugin            *self;

    WAVEFORMATEXTENSIBLE         waveformat;

    IMMDevice                    *device;
    int                              token;
    HANDLE                         start_event;
    HRESULT                         start_hr;

    RAAT__StreamFormat             pcmformat;
    RAAT__StreamFormat             origformat;
    UINT                         pcm_samples_per_buf;

    double                         resync_delay_secs;
    int64_t                         resync_delay_remaining_ns;

    bool                         dsd_dop_flipper;

    RAAT__OutputSetupCallback    setup_cb;
    void *                       setup_cb_userdata;

    int64_t                         latency_ns;
    int64_t                         streamsample;

    uint8_t                     *origbuf;
    uint8_t                     *pcmbuf;
    uint8_t                     *dsdbuf;

    RAAT__OutputLostCallback     cb_lost;
    void                        *cb_lost_userdata;

    int                             volume_delay_samples;

} WasapiOutputThreadState;

void GetSilence(WasapiOutputPlugin *self, WasapiOutputThreadState *threadstate, BYTE *buf, int off, int frames) {
    buf = buf + off;
    int bytesperframe = threadstate->pcmformat.bits_per_sample * threadstate->pcmformat.channels / 8;
    if (threadstate->origformat.sample_type == RAAT__SAMPLE_TYPE_DSD && self->dsd_mode == DSD_MODE_DCS) {
        if (threadstate->pcmformat.bits_per_sample == 24) {
            for (int i = 0; i < frames; i++) {
                buf[i * 6 + 0] = 0x69; buf[i * 6 + 1] = 0x69; buf[i * 6 + 2] = 0xaa;
                buf[i * 6 + 3] = 0x69; buf[i * 6 + 4] = 0x69; buf[i * 6 + 5] = 0xaa;
            }
        }
        else if (threadstate->pcmformat.bits_per_sample == 32) {
            for (int i = 0; i < frames; i++) {
                buf[i * 8 + 1] = 0x69; buf[i * 8 + 2] = 0x69; buf[i * 8 + 3] = 0xaa;
                buf[i * 8 + 5] = 0x69; buf[i * 8 + 6] = 0x69; buf[i * 8 + 7] = 0xaa;
            }
        }
        else {
            for (int i = 0; i < bytesperframe * frames; i++) buf[i] = 0;
        }
    }
    else if (threadstate->origformat.sample_type == RAAT__SAMPLE_TYPE_DSD && self->dsd_mode == DSD_MODE_DOP) {
        if (threadstate->pcmformat.bits_per_sample == 24) {
            for (int i = 0; i < frames; i++) {
                buf[i * 6 + 0] = 0x69; buf[i * 6 + 1] = 0x69; buf[i * 6 + 2] = threadstate->dsd_dop_flipper ? 0x05 : 0xfa;
                buf[i * 6 + 3] = 0x69; buf[i * 6 + 4] = 0x69; buf[i * 6 + 5] = threadstate->dsd_dop_flipper ? 0x05 : 0xfa;
                threadstate->dsd_dop_flipper = !threadstate->dsd_dop_flipper;
            }
        }
        else if (threadstate->pcmformat.bits_per_sample == 32) {
            for (int i = 0; i < frames; i++) {
                buf[i * 8 + 1] = 0x69; buf[i * 8 + 2] = 0x69; buf[i * 8 + 3] = threadstate->dsd_dop_flipper ? 0x05 : 0xfa;
                buf[i * 8 + 5] = 0x69; buf[i * 8 + 6] = 0x69; buf[i * 8 + 7] = threadstate->dsd_dop_flipper ? 0x05 : 0xfa;
                threadstate->dsd_dop_flipper = !threadstate->dsd_dop_flipper;
            }
        }
        else {
            for (int i = 0; i < bytesperframe * frames; i++) buf[i] = 0;
        }
    }
    else {
        for (int i = 0; i < bytesperframe * frames; i++) buf[i] = 0;
    }
}

static bool fill_buffer(WasapiOutputPlugin *self, WasapiOutputThreadState *threadstate, IAudioClock *clock, uint8_t *hwbuf, int hw_samples) {

    RAAT__Stream       *stream = NULL;
    RAAT__StreamFormat  origformat = threadstate->origformat;
    RAAT__StreamFormat  pcmformat = threadstate->pcmformat;

    uv_mutex_lock(&self->lock);

    if (threadstate->token != self->current_token) {
        uv_mutex_unlock(&self->lock);
        return true;
    }

    int bytes_per_hwbuf = RAAT__stream_format_compute_buffer_size(&pcmformat, hw_samples);

    int bytes_per_frame = threadstate->pcmformat.bits_per_sample / 8 * self->pcmformat.channels;
    int origformat_samples_per_buf = threadstate->origformat.sample_type == RAAT__SAMPLE_TYPE_DSD ? hw_samples * 16 : hw_samples;
    int ns_per_buf = RAAT__stream_format_samples_to_ns(&origformat, origformat_samples_per_buf);

    int origformat_samples_to_zerofill = 0;

    int64_t latency_ns = threadstate->latency_ns;

    UINT64 position, qpc, freq;
    HRESULT hr = clock->GetPosition(&position,&qpc);
    if (SUCCEEDED(hr)) {
        HRESULT hr = clock->GetFrequency(&freq);
        if (SUCCEEDED(hr)) {
            int64_t position_ns = (UINT64)((double)position/(double)freq*1000000000.0);
            uint64_t ns = RAAT__stream_format_samples_to_ns(&origformat, self->last_monotonic_sample + origformat_samples_per_buf);
            //RAAT__TRACE("latency %fms", (ns-position_ns)/1000000.0);
            latency_ns = ns - position_ns;
        }
    }

    int64_t now_raw_ns = RC__now_ns();
    self->last_monotonic_sample_systime = now_raw_ns + latency_ns /* XXX: threadstate->volume_delay_samples */;
    self->last_monotonic_sample += origformat_samples_per_buf;
    self->last_delay_ns = latency_ns;

    int64_t now = RAAT__stream_format_samples_to_ns(&origformat, self->last_monotonic_sample);

    int correction_samples = 0;

    if (self->new_stream && self->stream) {
        int64_t start_time = self->start_time;
        if (now + ns_per_buf > start_time) {
            RAAT__TRACE("[output/wasapi] [%s] starting playback: now (%lldns) + ns_per_buf(%d) = %lldns > %lldns streamsample=%lld", self->device_name, now, ns_per_buf, now + ns_per_buf, start_time, self->start_streamsample);
            self->new_stream = false;
            stream = self->stream;
            threadstate->streamsample = self->start_streamsample;

            // streamsample must fall on an 8-bit boundary or bad things will happen
            if (origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) 
                threadstate->streamsample -= threadstate->streamsample % 8;

            if (now < start_time) {
                int ns_to_fill = start_time - now;
                origformat_samples_to_zerofill = RAAT__stream_format_ns_to_samples(&origformat, ns_to_fill);
            } if (now > start_time) {
                threadstate->streamsample += RAAT__stream_format_ns_to_samples(&origformat, now - start_time);
            }
        }
        else {
            RAAT__TRACE("[output/wasapi] [%s] waiting for start time... now=%lld start_time=%lld", self->device_name, now, start_time);
        }
    }
    else {
        stream = self->stream;
        origformat_samples_to_zerofill = 0;
    }

    correction_samples = RAAT__drift_correction_compute_correction(&self->drift_correction, origformat_samples_per_buf);

    if (stream != NULL) RAAT__stream_incref(stream);

    uv_mutex_unlock(&self->lock);

    if (stream == NULL || threadstate->resync_delay_remaining_ns > 0) {
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
        if (origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) threadstate->origbuf[i] = 0x69;
        else                                                 threadstate->origbuf[i] = 0x00;
    }

    if (stream != NULL && threadstate->resync_delay_remaining_ns <= 0) {
        //if (correction_samples != 0) RAAT__TRACE("[output/wasapi] applying %d sample correction", correction_samples);
        RAAT__read_stream_with_drift_correction(stream, threadstate->streamsample, threadstate->origbuf + bytes_to_zerofill, origformat_samples_per_buf - origformat_samples_to_zerofill, correction_samples, NULL, NULL);
        threadstate->streamsample += origformat_samples_per_buf - origformat_samples_to_zerofill + correction_samples;
    }

    if (threadstate->resync_delay_remaining_ns > 0) {
        threadstate->resync_delay_remaining_ns -= ns_per_buf;
        if (threadstate->resync_delay_remaining_ns <= 0) {
            threadstate->setup_cb(threadstate->setup_cb_userdata, RC__STATUS_SUCCESS, threadstate->token);
            threadstate->setup_cb = NULL;
            self->cb_lost_userdata = threadstate->cb_lost_userdata;
            self->cb_lost          = threadstate->cb_lost;
        }
    }

    uint8_t *buf = threadstate->origbuf;

    if (origformat.sample_type == RAAT__SAMPLE_TYPE_PCM && pcmformat.bits_per_sample > origformat.bits_per_sample) {
        // repack PCM into wider PCM. This case is most relevant when performing digital volume adjustments. We may have
        // opened the audio device at a wider bits-per-sample than the stream, so that we can preserve as much data as possible after
        // volume attenuation
        RAAT__stream_format_repack(&origformat, buf, &pcmformat, threadstate->pcmbuf, origformat_samples_per_buf);
        buf = threadstate->pcmbuf;
    }

    if (self->volume) {
        self->volume->process(self->volume->userdata, 0, buf, origformat_samples_per_buf);
    }

    // DSD must be repacked for encapsulation
    if (origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) {
        if (self->dsd_mode == DSD_MODE_DOP) {
            RAAT__pack_dop_samples(&origformat, buf, threadstate->dsdbuf, origformat_samples_per_buf, &threadstate->dsd_dop_flipper);
        }
        else if (self->dsd_mode == DSD_MODE_DCS) {
            RAAT__pack_dcs_samples(&origformat, buf, threadstate->dsdbuf, origformat_samples_per_buf);
        }
        else {
            RC__ASSERT(false);
        }
        if (pcmformat.bits_per_sample == 32) {
            RAAT__StreamFormat dopformat = pcmformat; dopformat.bits_per_sample = 24;
            RAAT__stream_format_repack(&dopformat, threadstate->dsdbuf, &pcmformat, hwbuf, origformat_samples_per_buf / 16);
        } else {
            memcpy(hwbuf, threadstate->dsdbuf, bytes_per_hwbuf);
        }
    }
    else {
        memcpy(hwbuf, buf, bytes_per_hwbuf);
    }


    if (stream != NULL) RAAT__stream_decref(stream);
    return 0;
}

static void notify_default_device_changed(WasapiOutputPlugin *self, LPCWSTR device_id) {
    uv_mutex_lock(&self->lock);

    IMMDevice *device;
    HRESULT hr;

    hr = self->device_enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[output/wasapi] [notify_default_device_changed] Failed to get default audio endpoint HRESULT=0x%x", hr);
        goto fail;
    }

    LPWSTR device_id_wide;
    hr = device->GetId(&device_id_wide);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[output/wasapi] [notify_default_device_changed] Failed to get device id HRESULT=0x%x", hr);
        goto fail;
    }

    BOOL has_devicedesc = TRUE;
    PROPVARIANT ifacenameprop;
    PropVariantInit(&ifacenameprop);
    hr = self->deviceprops->GetValue(PKEY_DeviceInterface_FriendlyName, &ifacenameprop);
    if (!SUCCEEDED(hr)) {
        PropVariantClear(&ifacenameprop);
        RAAT__ERROR("[output/wasapi] [failed to get desc from device HRESULT=0x%x", self->device_id, hr);
        goto fail;
    }

    char *new_device_name;
    wchar_t *new_device_id_wide;
    char *new_device_id;

    size_t device_name_len = wcstombs(NULL, ifacenameprop.pwszVal, 0) + 1;
    new_device_name = RC__new0(self->alloc, char, device_name_len);
    wcstombs(new_device_name, ifacenameprop.pwszVal, device_name_len);
    PropVariantClear(&ifacenameprop);

    size_t device_id_wide_size = sizeof(wchar_t)*(wcslen(device_id_wide) + 1);
    new_device_id_wide = (wchar_t*)RC__alloc0(self->alloc, device_id_wide_size);
    memcpy(new_device_id_wide, device_id_wide, device_id_wide_size);

    size_t device_id_size = wcstombs(NULL, device_id_wide, 0) + 1;
    new_device_id = RC__new0(self->alloc, char, device_id_size);
    wcstombs(new_device_id, device_id_wide, device_id_size);
    CoTaskMemFree(device_id_wide);

    char *old_device_name = self->device_name;
    wchar_t *old_device_id_wide = self->device_id_wide;
    char *old_device_id = self->device_id;

    self->device_name = new_device_name;
    self->device_id_wide = new_device_id_wide;
    self->device_id = new_device_id;

    RC__free(self->alloc, old_device_id);
    RC__free(self->alloc, old_device_name);
    RC__free(self->alloc, old_device_id_wide);

    IMMDevice *old_device = self->device;
    self->device = device;
    SafeRelease(&old_device);

fail:
    uv_mutex_unlock(&self->lock);
}

class WasapiOutput_DefaultDeviceNotifications : public IMMNotificationClient {
    ULONG                _cRef;
    WasapiOutputPlugin *_self;

public:
    WasapiOutput_DefaultDeviceNotifications(WasapiOutputPlugin *self) : _cRef(1), _self(self) { }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDevice) {
        if (role == eMultimedia && flow == eRender) {
            notify_default_device_changed(_self, pwstrDefaultDevice);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR deviceId) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR deviceId) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR deviceId, DWORD dwNewState) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR deviceId, const PROPERTYKEY key) { return S_OK; }

    ~WasapiOutput_DefaultDeviceNotifications() { }

    /* IUnknown */
    ULONG STDMETHODCALLTYPE AddRef() {
        return InterlockedIncrement(&_cRef);
    }
    ULONG STDMETHODCALLTYPE Release() {
        ULONG ulRef = InterlockedDecrement(&_cRef);
        if (0 == ulRef) delete this;
        return ulRef;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID  riid, VOID  **ppvInterface) {
        if (IID_IUnknown == riid) {
            AddRef();
            *ppvInterface = (IUnknown*)this;
        }
        else if (__uuidof(IMMNotificationClient) == riid) {
            AddRef();
            *ppvInterface = (IMMNotificationClient*)this;
        }
        else {
            *ppvInterface = NULL;
            return E_NOINTERFACE;
        }
        return S_OK;
    }
};

static void wasapi_output_thread(void *vself) {
    WasapiOutputThreadState *threadstate = (WasapiOutputThreadState*)vself;
    WasapiOutputPlugin      *self = threadstate->self;
    IAudioRenderClient      *rclient = NULL;
    IAudioClient            *client = NULL;
    IMMDeviceEnumerator     *device_enumerator = NULL;
    IMMDevice               *device = NULL;
    IAudioClock             *clock = NULL;

    RAAT__StreamFormat         pcmformat = threadstate->pcmformat;
    RAAT__StreamFormat         origformat = threadstate->origformat;
    UINT                     pcm_samples_per_buf = threadstate->pcm_samples_per_buf;
    HANDLE                     ev = CreateEvent(NULL, FALSE, FALSE, NULL);
    HRESULT hr;

    CoInitialize(NULL);

    wchar_t *device_id_wide = NULL;

    DWORD _;
    AvSetMmThreadCharacteristics("Pro Audio", &_);

    // now, open the device. There is some retry-loop type stuff here, so wrap in a loop
    bool did_inval_retry        = false;
    bool did_set_event            = false;
    bool default_device_changed = false;

        if (self->force_max_volume) {
            force_max_volume(self);
        }

inval_retry:
    SafeRelease(&client);
    SafeRelease(&rclient);
    SafeRelease(&device_enumerator);
    SafeRelease(&device);

    self->last_monotonic_sample = 0;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&device_enumerator);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[output/wasapi] [%s] Failed to create IMMDeviceEnumerator HRESULT=0x%x", self->device_name, hr);
        goto exit;
    }
    if (device_id_wide) free(device_id_wide);
    device_id_wide = _wcsdup(self->device_id_wide);
    RAAT__TRACE("[output/wasapi] [%s] opening %s", self->device_name, self->device_id);
    hr = device_enumerator->GetDevice(device_id_wide, &device);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[output/wasapi] [%s] Failed to create device HRESULT=0x%x", self->device_name, hr);
        goto exit;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&client);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[output/wasapi] [%s] Failed to activate IAudioClient HRESULT=0x%x", self->device_name, hr);
        goto exit;
    }

    REFERENCE_TIME    hnsPeriod = 0;
    AUDCLNT_SHAREMODE sharemode = self->exclusive_mode ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;

    if (self->event_driven_mode) { /* event driven mode. */
        hnsPeriod = (REFERENCE_TIME)(self->buffer_duration_secs * 10000000); 
        hr = client->Initialize(sharemode, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_RATEADJUST,
                                hnsPeriod, hnsPeriod, reinterpret_cast<WAVEFORMATEX*>(&threadstate->waveformat), NULL);
        if (AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED == hr) {
            // if the buffer size was not aligned, need to do the alignment dance
            RAAT__TRACE("[output/wasapi] [%s] IAudioClient->Initialize Failed due to alignment. Doing the dance", self->device_name);

            UINT32 nFramesInBuffer = 0;

            // get the buffer size, which will be aligned
            hr = client->GetBufferSize(&nFramesInBuffer);
            if (FAILED(hr)) {
                RAAT__ERROR("[output/wasapi] [%s] IAudioClient->GetBufferSize failed HRESULT=0x%x", self->device_name, hr);
                goto exit;
            }

            // calculate the new aligned periodicity
            hnsPeriod = // hns =
                (REFERENCE_TIME)(
                    10000.0 * // (hns / ms) *
                    1000 * // (ms / s) *
                    nFramesInBuffer / // frames /
                    pcmformat.sample_rate
                    + 0.5 // rounding
                    );

            // activate a new IAudioClient
            hr = device->Activate(
                __uuidof(IAudioClient),
                CLSCTX_ALL, NULL,
                (void**)&client
                );
            if (FAILED(hr)) {
                RAAT__ERROR("[output/wasapi] [%s] Activate failed during alignment dance HRESULT=0x%x", self->device_name, hr);
                goto exit;
            }

            // try initialize again
            RAAT__TRACE("[output/wasapi] [%s] Trying again with periodicity of %I64u hundred-nanoseconds, or %u frames.\n", self->device_name, hnsPeriod, nFramesInBuffer);
            hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_RATEADJUST,
                hnsPeriod, hnsPeriod, reinterpret_cast<WAVEFORMATEX*>(&threadstate->waveformat), NULL);
            if (FAILED(hr)) {
                RAAT__ERROR("[output/wasapi] [%s] IAudioClient->Initialize failed with aligned buffer, HRESULT=0x%x", self->device_name, hr);
                goto exit;
            }
        } else if (FAILED(hr)) {
            RAAT__ERROR("[output/wasapi] [%s] IAudioClient->Initialize failed, HRESULT=0x%x", self->device_name, hr);
            goto exit;
        }

        hr = client->SetEventHandle(ev);
        if (FAILED(hr)) {
            RAAT__ERROR("[output/wasapi] [%s] IAudioClient->SetEventHandle failed, HRESULT=0x%x", self->device_name, hr);
            goto exit;
        }

    } else { /* Push mode: much simpler */
        hr = client->Initialize(sharemode, AUDCLNT_STREAMFLAGS_RATEADJUST,
            10000000 / PUSH_BUFS_PER_SEC,    // in 100ns units.
            0,
            reinterpret_cast<WAVEFORMATEX*>(&threadstate->waveformat),
            NULL);
        if (!SUCCEEDED(hr)) {
            RAAT__ERROR("[output/wasapi] [%s] IAudioClient->Initialize failed, HRESULT=0x%x", self->device_name, hr);
            goto exit;
        }
    }

    REFERENCE_TIME latency;
    hr = client->GetStreamLatency(&latency);
    if (SUCCEEDED(hr)) {
        threadstate->latency_ns = latency * 100;
    } else {
        threadstate->latency_ns = 0;
    }
    if (!self->exclusive_mode) {
        threadstate->latency_ns += 250 * 1000000;       // 1s / PUSH_BUFS_PER_SEC
    }
    RAAT__TRACE("[output/wasapi] [%s] stream latency = %lldns", self->device_name, threadstate->latency_ns);


    hr = client->GetBufferSize(&pcm_samples_per_buf);
    if (FAILED(hr)) {
        RAAT__ERROR("[output/wasapi] [%s] IAudioClient->GetBufferSizeFailed failed, HRESULT=0x%x", self->device_name, hr);
        goto exit;
    }

    // free old DSP buffers (in case we're re-running this code due to a device change
    if (threadstate->pcmbuf) RC__free(self->alloc, threadstate->pcmbuf);
    if (threadstate->dsdbuf) RC__free(self->alloc, threadstate->dsdbuf);
    if (threadstate->origbuf) RC__free(self->alloc, threadstate->origbuf);

    // allocate DSP buffers
    int max_origformat_samples_per_buf = origformat.sample_type == RAAT__SAMPLE_TYPE_DSD ? 16 * pcm_samples_per_buf : pcm_samples_per_buf;
    int max_bytes_per_origbuf = RAAT__stream_format_compute_buffer_size(&origformat, max_origformat_samples_per_buf);
    threadstate->origbuf = (uint8_t*)RC__alloc(self->alloc, max_bytes_per_origbuf);

    int max_bytes_per_pcmbuf = RAAT__stream_format_compute_buffer_size(&pcmformat, pcm_samples_per_buf);
    threadstate->pcmbuf = (uint8_t*)RC__alloc(self->alloc, max_bytes_per_pcmbuf);

    if (origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) {
        // repacked DoP is a 3/2 ratio to the size of the DSD
        threadstate->dsdbuf = (uint8_t*)RC__alloc(self->alloc, max_bytes_per_origbuf * 3 / 2);
    }

    // calculate the new period
    hnsPeriod = // hns =
        (REFERENCE_TIME)(
            10000.0 * // (hns / ms) *
            1000 * // (ms / s) *
            pcm_samples_per_buf / // frames /
            pcmformat.sample_rate// (frames / s)
            + 0.5 // rounding
            );


    hr = client->GetService(__uuidof(IAudioRenderClient), (void**)&rclient);
    if (!SUCCEEDED(hr)) {
        RAAT__WARNING("[output/wasapi] [%s] in audio thread, GetService(IAudioRenderClient) failed hr=0x%x", self->device_name, hr);
        goto exit;
    }

    hr = client->GetService(__uuidof(IAudioClock), (void**)&clock);
    if (!SUCCEEDED(hr)) {
        RAAT__WARNING("[output/wasapi] [%s] in audio thread, GetService(IAudioClock) failed hr=0x%x", self->device_name, hr);
        goto exit;
    }

    if (self->event_driven_mode) {
        RAAT__TRACE("using event driven mode");

        BYTE *buf = NULL;
        DWORD flags = 0;
        BOOL done = false;

        buf = NULL;
        hr = rclient->GetBuffer(pcm_samples_per_buf, &buf);
        if (!SUCCEEDED(hr)) {
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                // With a few sloppy drivers the first call after Initialize fails because the device was "invalidated" due to a sample rate change.
                // This will succeed if tried immediately
                RAAT__TRACE("[output/wasapi] [%s] IAudioClient->GetBufferSize => AUDCLNT_E_DEVICE_INVALIDATED", self->device_name);
                if (!did_inval_retry) {
                    RAAT__TRACE("[output/wasapi] [%s] Retrying...", self->device_name);
                    did_inval_retry = true;
                    goto inval_retry;
                }
            }
            else {
                RAAT__WARNING("[output/wasapi] [%s] in audio thread, IAudioRenderClient->GetBuffer failed hr=0x%x", self->device_name, hr);
            }
            goto exit;
        }

        GetSilence(self, threadstate, buf, 0, pcm_samples_per_buf);

        hr = rclient->ReleaseBuffer(pcm_samples_per_buf, flags);
        if (!SUCCEEDED(hr)) {
            RAAT__ERROR("[output/wasapi] [%s] failed to IAudioRenderClient->ReleaseBuffer failed HRESULT=0x%x", self->device_name, hr);
            goto exit;
        }

        // Start playback
        hr = client->Start();
        if (!SUCCEEDED(hr)) {
            RAAT__ERROR("[output/wasapi] [%s] failed to IAudioRenderClient->ReleaseBuffer failed HRESULT=0x%x", self->device_name, hr);
            goto exit;
        }

        for (;;) {
            DWORD retval = WaitForSingleObject(ev, 2000);
            if (retval != WAIT_OBJECT_0) {
                RAAT__TRACE("[output/wasapi] [%s] timed out while waiting for event in eventdriven mode", self->device_name);
                hr = ERROR_TIMEOUT;
                goto exit; 
            }

            if (!did_set_event) {
                // OK. Good to go. Tell the world we're up
                did_set_event = true;
                threadstate->start_hr = S_OK;
                SetEvent(threadstate->start_event);
                RAAT__TRACE("[output/wasapi] [%s] set the event!", self->device_name);
            }

            buf = NULL;
            hr = rclient->GetBuffer(pcm_samples_per_buf, &buf);
            if (!SUCCEEDED(hr)) {
                RAAT__TRACE("[output/wasapi] [%s] failed to get buffer HRESULT=0x%x", self->device_name, hr);
                break;
            }

            bool done = fill_buffer(self, threadstate, clock, buf, pcm_samples_per_buf);

            if (done) {
                GetSilence(self, threadstate, buf, 0, pcm_samples_per_buf);
            }

            flags = 0;
            hr = rclient->ReleaseBuffer(pcm_samples_per_buf, flags);
            if (!SUCCEEDED(hr)) {
                RAAT__TRACE("[output/wasapi] [%s] failed to release HRESULT=0x%x", self->device_name, hr);
                break;
            }

            if (done) {
                client->Stop();
                goto exit; 
            }

            //RAAT__TRACE("render buffer %d frames (%d frames per buffer)", pcm_samples_per_buf, pcm_samples_per_buf);
        }

        Sleep((DWORD)hnsPeriod / REFTIMES_PER_MILLISEC);

    } else {
        RAAT__TRACE("using push mode");

        for (;;) {
            BYTE *buf = NULL;
            UINT32 padding;

            hr = client->GetCurrentPadding(&padding);
            if (!SUCCEEDED(hr)) {
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                    // With a few sloppy drivers the first call after Initialize fails because the device was "invalidated" due to a sample rate change.
                    // This will succeed if retried immediately
                    RAAT__TRACE("[output/wasapi] [%s] IAudioClient->GetPadding => AUDCLNT_E_DEVICE_INVALIDATED", self->device_name);
                    if (!did_inval_retry) {
                        RAAT__TRACE("[output/wasapi] [%s] Retrying...", self->device_name);
                        did_inval_retry = true;
                        goto inval_retry;
                    }
                }
                else {
                    RAAT__TRACE("[output/wasapi] [%s] failed to get padding HRESULT=0x%x", self->device_name, hr);
                }
                goto exit;
            }

            int samples_to_fill = pcm_samples_per_buf - padding;

            hr = rclient->GetBuffer(samples_to_fill, &buf);
            if (!SUCCEEDED(hr)) {
                RAAT__TRACE("[output/wasapi] [%s] failed to get buffer HRESULT=0x%x", self->device_name, hr);
                goto exit;
            }

            if (!did_set_event) {
                hr = client->Start();
                if (!SUCCEEDED(hr)) {
                    RAAT__TRACE("[output/wasapi] [%s] Failed to start stream HRESULT=0x%x", self->device_name, hr);
                    goto exit;
                }
                did_set_event = true;
                threadstate->start_hr = S_OK;
                SetEvent(threadstate->start_event);
            }

            bool done = fill_buffer(self, threadstate, clock, buf, samples_to_fill);
            if (done) {
                GetSilence(self, threadstate, buf, 0, samples_to_fill);
            }

            DWORD flags = 0;
            hr = rclient->ReleaseBuffer(samples_to_fill, flags);

            if (!SUCCEEDED(hr)) {
                RAAT__TRACE("[output/wasapi] [%s] failed to release HRESULT=0x%x", self->device_name, hr);
                goto exit;
            }

            if (done) {
                client->Stop();
                goto exit;
            }

            Sleep(100);

            uv_mutex_lock(&self->lock);
            if (wcscmp(self->device_id_wide, device_id_wide)) {
                // NOTE: this can only happen in shared mode + push mode, so we don't need this code in the event-driven case
                RAAT__TRACE("[output/wasapi] [%s] device changed during streaming! aborting and notifying roon...", self->device_name);
                default_device_changed = true;
                uv_mutex_unlock(&self->lock);
                goto exit;
            }
            uv_mutex_unlock(&self->lock);
        }
    }

exit:
    // complicated branching:
    if (!did_set_event) {
        // case 0: we are still blocked in setup() and haven't set the event yet, so communicate our error code back to setup()
        did_set_event = true;
        threadstate->start_hr = hr;
        SetEvent(threadstate->start_event);
        Sleep(200);        // sleep before cleaning up threadstate so the event wakeup can happen before the event object dies (crude, but safe).

    } else if (threadstate->setup_cb != NULL) {
        // case 1: we are within the resync delay, so we haven't reported a successful setup_cb_yet. Report failure that way
        threadstate->setup_cb(threadstate->setup_cb_userdata, RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED, threadstate->token);
        threadstate->setup_cb = NULL;
    } else {
        // case 3: failure while the stream is flowing. Report this as an output_lost situation
        json_t *reason = json_object();
        if (default_device_changed) {
            json_object_set_new(reason, "reason", json_string("default_device_changed"));
            json_object_set_new(reason, "resume", json_true());
        } else {
            json_object_set_new(reason, "reason", json_string("device_not_available"));
        }
        on_output_lost(self, reason);
        json_decref(reason);
    }

    // now clean up the resources..
    CloseHandle(threadstate->start_event);
    client->Stop();
    client->Reset();

    SafeRelease(&rclient);
    SafeRelease(&clock);
    SafeRelease(&client);
    SafeRelease(&device);
    SafeRelease(&device_enumerator);

    CloseHandle(ev);

    if (threadstate->pcmbuf) RC__free(self->alloc, threadstate->pcmbuf);
    if (threadstate->dsdbuf) RC__free(self->alloc, threadstate->dsdbuf);
    if (threadstate->origbuf) RC__free(self->alloc, threadstate->origbuf);
    RC__free(self->alloc, threadstate);
}


static void output_setup(void *vself, RAAT__StreamFormat *format, RAAT__OutputSetupCallback cb_setup, void *cb_setup_userdata, RAAT__OutputLostCallback cb_lost, void *cb_lost_userdata) {
    WasapiOutputPlugin *self = (WasapiOutputPlugin*)vself;
    RC__Status status = RC__STATUS_SUCCESS;
    int        token = RAAT__OUTPUT_TOKEN_INVALID;

    RAAT__OutputLostCallback  old_cb_lost = NULL;
    void                     *old_cb_lost_userdata = NULL;
    bool                      needs_join = false;

    char formatstr[RAAT__STREAM_FORMAT_MAX_STRLEN];
    RAAT__stream_format_to_string(format, formatstr);

    uv_mutex_lock(&self->lock);
    // before we do anything else, kick off anyone currently using the device
    self->current_token = token = ++self->next_token;

    if (self->state != IDLE) {
        RAAT__TRACE("[output/wasapi] kicking off old output");
        needs_join = true;
        self->state = IDLE;
        old_cb_lost = self->cb_lost;
        old_cb_lost_userdata = self->cb_lost_userdata;
        self->cb_lost = NULL;
        self->cb_lost_userdata = NULL;
    }
    uv_mutex_unlock(&self->lock);

    if (needs_join) {
        RAAT__TRACE("[output/wasapi] joining thread");
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
        RAAT__TRACE("[output/wasapi] output setup: lost the race");
        status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED;
        goto fail;
    }

    // clean up leftover stream 
    if (self->stream != NULL) {
        RAAT__stream_decref(self->stream);
        self->stream = NULL;
    }

    RAAT__TRACE("[output/wasapi] output setup: format is %s", formatstr);

    bool found = false;
    RAAT__StreamFormat pcmformat;
    WAVEFORMATEXTENSIBLE waveformat;
    int i;

    // If we are preferring larger samples due to software volume adjustment, then figure out the real format here
    bool prefer_larger_samples = self->volume != NULL && format->sample_type == RAAT__SAMPLE_TYPE_PCM && format->bits_per_sample < 32;
    if (prefer_larger_samples) {
        RAAT__StreamFormat format32 = *format; format32.bits_per_sample = 32;
        RAAT__StreamFormat format24 = *format; format24.bits_per_sample = 24;
        switch (format->bits_per_sample) {
        case 24:
            for (i = 0; !found && i < self->n_supported_formats; i++) {
				if (RAAT__stream_format_equals(&format32, &self->supported_formats[i])) { found = true; pcmformat = format32; waveformat = self->supported_waveformats[i]; break; }
            }
            break;
        case 16:
            for (i = 0; !found && i < self->n_supported_formats; i++) {
                if (RAAT__stream_format_equals(&format32, &self->supported_formats[i])) { found = true; pcmformat = format32; waveformat = self->supported_waveformats[i]; break; }
            }
            for (i = 0; !found && i < self->n_supported_formats; i++) {
                if (RAAT__stream_format_equals(&format24, &self->supported_formats[i])) { found = true; pcmformat = format24; waveformat = self->supported_waveformats[i]; break; }
            }
            break;
        }
    }

    for (i = 0; !found && i < self->n_supported_formats; i++) {
        if (RAAT__stream_format_equals(format, &self->supported_formats[i])) { found = true; pcmformat = *format; waveformat = self->supported_waveformats[i]; break; }
    }

    if (found) {
        // for DoP/dCS pack
        if (format->sample_type == RAAT__SAMPLE_TYPE_DSD) {
            pcmformat.sample_type = RAAT__SAMPLE_TYPE_PCM;
            pcmformat.sample_rate = format->sample_rate / 16;
            pcmformat.bits_per_sample = 24;
            pcmformat.channels = format->channels;
        } else {
            pcmformat = *format;
        }

        // this is needed if we are doing 16_32, 16_24, or 24_32
        pcmformat.bits_per_sample = waveformat.Format.wBitsPerSample;

        RAAT__TRACE("[output/wasapi] opening %d/%d/%d", pcmformat.sample_rate, pcmformat.bits_per_sample, pcmformat.channels);

        HRESULT hr;

        RAAT__drift_correction_init(&self->drift_correction, self->log, format);

        WasapiOutputThreadState *threadstate = RC__new0(self->alloc, WasapiOutputThreadState, 1);
        threadstate->waveformat = waveformat;
        threadstate->origformat = *format;
        threadstate->pcmformat = pcmformat;
        threadstate->pcm_samples_per_buf = pcmformat.sample_rate / 20;
        threadstate->self = self;
        threadstate->token = token;
        threadstate->resync_delay_remaining_ns = self->resync_delay_secs * 1000000000LL;
        threadstate->setup_cb = cb_setup;
        threadstate->setup_cb_userdata = cb_setup_userdata;
        threadstate->start_event = CreateEvent(NULL, FALSE, FALSE, NULL);
        threadstate->cb_lost = cb_lost;
        threadstate->cb_lost_userdata = cb_lost_userdata;

        if (self->volume) {
            if (format->sample_type == RAAT__SAMPLE_TYPE_DSD)
                self->volume->setup(self->volume->userdata, &pcmformat, &threadstate->volume_delay_samples);
            else if (format->sample_type == RAAT__SAMPLE_TYPE_PCM)
                self->volume->setup(self->volume->userdata, &pcmformat, &threadstate->volume_delay_samples);
        }

        // start the thread, wait for it to prime the output once, and make sure it didn't get a device invalidation
        uv_thread_create(&self->tid, wasapi_output_thread, threadstate);
        RAAT__DEBUG("[output/wasapi] waiting on event");
        WaitForSingleObject(threadstate->start_event, INFINITE);
        hr = threadstate->start_hr;
        RAAT__DEBUG("[output/wasapi] [%s] start finished, HRESULT=0x%x", self->device_name, hr);
        if (FAILED(hr)) {
            RAAT__ERROR("[output/wasapi] [%s] start failed, HRESULT=0x%x", self->device_name, hr);
            if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
                status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED;
            else if (hr == AUDCLNT_E_DEVICE_IN_USE)
                status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_IN_USE;
            else
                status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED;
            goto fail;
        }

        // all good, finally. 
        status = RC__STATUS_SUCCESS;
        self->state = STOPPED;
        self->origformat = *format;
        self->pcmformat = pcmformat;

        update_signal_path(self);
    }
    else {
        status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED;
    }

fail:
    uv_mutex_unlock(&self->lock);

    // mutexes might not be recursive, so the following happen outside of the lock.

    if (status == RC__STATUS_SUCCESS) {
        return;    // callback will be invoked from WASAPI thread
    }
    else {
        // call our setup callback
        cb_setup(cb_setup_userdata, status, token);
    }
}

static RC__Status output_teardown(void *vself, int token) {
    WasapiOutputPlugin *self = (WasapiOutputPlugin*)vself;
    RC__Status status = RC__STATUS_UNEXPECTED_ERROR;

    bool needs_join = false;

    uv_mutex_lock(&self->lock);
    if (self->current_token == token) {
        RAAT__TRACE("[output/wasapi] teardown");
        self->current_token = RAAT__OUTPUT_TOKEN_INVALID;
        self->state = IDLE;
        self->cb_lost = NULL;
        self->cb_lost_userdata = NULL;

        if (self->stream != NULL) {
            RAAT__stream_decref(self->stream);
            self->stream = NULL;
        }

        needs_join = true;

        status = RC__STATUS_SUCCESS;
    }
    else {
        status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_TOKEN;
    }
    uv_mutex_unlock(&self->lock);

    if (needs_join) {
        uv_thread_join(&self->tid);
    }

    return status;
}

static RC__Status output_stop(void *vself, int token) {
    WasapiOutputPlugin *self = (WasapiOutputPlugin*)vself;
    RC__Status status = RC__STATUS_UNEXPECTED_ERROR;

    uv_mutex_lock(&self->lock);
    if (self->current_token == token) {
        if (self->state == RUNNING) {
            self->state = STOPPED;
            RAAT__stream_decref(self->stream);
            self->stream = NULL;
            status = RC__STATUS_SUCCESS;
        }
        else {
            status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_STATE;
        }
    }
    else {
        status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_TOKEN;
    }
    uv_mutex_unlock(&self->lock);

    return status;
}

static RC__Status output_start(void *vself, int token, int64_t time, int64_t streamsample, RAAT__Stream *stream) {
    WasapiOutputPlugin *self = (WasapiOutputPlugin*)vself;
    RC__Status status = RC__STATUS_UNEXPECTED_ERROR;

    uv_mutex_lock(&self->lock);
    if (self->current_token == token) {
        if (self->state == STOPPED) {
            RAAT__stream_incref(stream);
            self->stream = stream;
            self->start_streamsample = streamsample;
            self->start_time = time;
            self->new_stream = true;
            self->state = RUNNING;
            status = RC__STATUS_SUCCESS;
        }
        else {
            status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_STATE;
        }
    }
    else {
        status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_TOKEN;
    }
    uv_mutex_unlock(&self->lock);

    return status;
}

static RC__Status output_get_output_delay(void *vself, int token, int64_t *out_delay) {
    WasapiOutputPlugin *self = (WasapiOutputPlugin*)vself;
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

static int64_t LOCKED_get_local_time(WasapiOutputPlugin *self) {
    int64_t now = RC__now_ns();
    int64_t last_wall_sample = self->last_monotonic_sample;
    return RAAT__stream_format_samples_to_ns(&self->origformat, last_wall_sample) + (now - self->last_monotonic_sample_systime);
}

static RC__Status output_get_local_time(void *vself, int token, int64_t *out_time) {
    WasapiOutputPlugin *self = (WasapiOutputPlugin*)vself;
    RC__Status status = RC__STATUS_UNEXPECTED_ERROR;
    uv_mutex_lock(&self->lock);
    if (self->current_token == token) {
        if (self->state != IDLE) {
            *out_time = LOCKED_get_local_time(self);
            status = RC__STATUS_SUCCESS;
        }
        else {
            status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_STATE;
        }
    }
    else {
        status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_TOKEN;
    }
    uv_mutex_unlock(&self->lock);
    return status;
}

static RC__Status output_set_remote_time(void *vself, int token, int64_t remote_time_offset, bool new_source) {
    WasapiOutputPlugin *self = (WasapiOutputPlugin*)vself;
    RC__Status status = RC__STATUS_UNEXPECTED_ERROR;
    uv_mutex_lock(&self->lock);
    if (self->current_token == token) {
        if (self->state != IDLE) {
            int64_t local_time = LOCKED_get_local_time(self);
            RAAT__drift_correction_set_remote_time(&self->drift_correction, local_time, remote_time_offset, new_source);
            status = RC__STATUS_SUCCESS;
        }
        else {
            status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_STATE;
        }
    }
    else {
        status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_TOKEN;
    }
    uv_mutex_unlock(&self->lock);
    return status;
}

static RC__Status
output_add_message_listener(void *vself, RAAT__OutputMessageCallback cb, void *cb_userdata) {
    WasapiOutputPlugin *self = (WasapiOutputPlugin*)vself;
    return RAAT__output_message_listeners_add(&self->message_listeners, cb, cb_userdata);
}

static RC__Status
output_remove_message_listener(void *vself, RAAT__OutputMessageCallback cb, void *cb_userdata) {
    WasapiOutputPlugin *self = (WasapiOutputPlugin*)vself;
    return RAAT__output_message_listeners_remove(&self->message_listeners, cb, cb_userdata);
}

static RC__Status
output_send_message(void *vself, json_t *message) {
    WasapiOutputPlugin *self = (WasapiOutputPlugin*)vself;
    char *s = json_dumps(message, 0);
    RAAT__TRACE("[output/wasapi] GOT MESSAGE %s", s);
    free(s);

    return RC__STATUS_SUCCESS;
}

static RC__Status
output_set_software_volume(void *vself, RAAT__OutputSoftwareVolume *volume) {
    WasapiOutputPlugin *self = (WasapiOutputPlugin*)vself;
    uv_mutex_lock(&self->lock);
    self->volume = volume;
    uv_mutex_unlock(&self->lock);
    return RC__STATUS_SUCCESS;
}

static bool check_format_the_nasty_way(WasapiOutputPlugin *self, AUDCLNT_SHAREMODE sharemode, UINT32 flags, WAVEFORMATEX *fmt) {
    IMMDeviceEnumerator *device_enumerator = NULL;
    IMMDevice *device = NULL;
    IAudioClient *client = NULL;

    REFERENCE_TIME hnsRequestedDuration = (REFERENCE_TIME)(self->buffer_duration_secs * 10000000); 


    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&device_enumerator);
    if (SUCCEEDED(hr)) {
        hr = device_enumerator->GetDevice(self->device_id_wide, &device);
        if (SUCCEEDED(hr)) {
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&client);
            if (SUCCEEDED(hr)) {
                if (self->event_driven_mode) {
                    hr = client->Initialize(sharemode, flags | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hnsRequestedDuration, hnsRequestedDuration, fmt, NULL);
                }
                else {
                    hr = client->Initialize(sharemode, flags, 10000000 / PUSH_BUFS_PER_SEC, 0, fmt, NULL);
                }
                if (SUCCEEDED(hr)) {
                    client->Reset();
                    SafeRelease(&client);
                    SafeRelease(&device);
                    SafeRelease(&device_enumerator);
                    if (self->exclusive_mode) {
                        RAAT__WARNING("[output/wasapi] [%s] format %d/%d/%d rejected by IsFormatSupported was accepted in Initialize--this indicates a driver bug", self->device_name, fmt->nSamplesPerSec, fmt->wBitsPerSample, fmt->nChannels);
                    }
                    return true;
                }
            }
        }
    }
    SafeRelease(&client);
    SafeRelease(&device);
    SafeRelease(&device_enumerator);
    return false;
}

static bool probe_one_format(WasapiOutputPlugin *self, IAudioClient *client, int samplerate, int bitspersample, int channels, WAVEFORMATEXTENSIBLE *out_wave_fmt) {
    WAVEFORMATEX  fmt_ex = { 0, };
    fmt_ex.wFormatTag = WAVE_FORMAT_PCM;
    fmt_ex.nChannels = channels;
    fmt_ex.cbSize = 0;

    WAVEFORMATEXTENSIBLE   fmt_extensible = { 0, };
    fmt_extensible.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt_extensible.Format.nChannels = channels;
    fmt_extensible.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    switch (channels) {
        case 1:  fmt_extensible.dwChannelMask = KSAUDIO_SPEAKER_MONO;             break;
        case 2:  fmt_extensible.dwChannelMask = KSAUDIO_SPEAKER_STEREO;           break;
        case 6:  fmt_extensible.dwChannelMask = KSAUDIO_SPEAKER_5POINT1_SURROUND; break;
        case 8:  fmt_extensible.dwChannelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND; break;
        default: fmt_extensible.dwChannelMask = KSAUDIO_SPEAKER_STEREO;           break;
    }
    fmt_extensible.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

	// always probe as if we are accessing in exclusive mode. This causes us to correctly ascertain what the device can do.
	AUDCLNT_SHAREMODE sharemode = AUDCLNT_SHAREMODE_EXCLUSIVE;
    UINT32 flags = self->exclusive_mode ? 0 : AUDCLNT_STREAMFLAGS_RATEADJUST;

    if (bitspersample == 16 || bitspersample == 24 || bitspersample == 32) {
        fmt_extensible.Format.nAvgBytesPerSec = fmt_ex.nAvgBytesPerSec = samplerate * 4 * channels;
        fmt_extensible.Format.nBlockAlign = fmt_ex.nBlockAlign = 4 * channels;
        fmt_extensible.Format.nSamplesPerSec = fmt_ex.nSamplesPerSec = samplerate;
        fmt_extensible.Format.wBitsPerSample = fmt_ex.wBitsPerSample = 32;
        fmt_extensible.Samples.wValidBitsPerSample = 32;
    }

    if (client->IsFormatSupported(sharemode, (WAVEFORMATEX*)&fmt_extensible, NULL) == S_OK) { *out_wave_fmt = *(WAVEFORMATEXTENSIBLE*)&fmt_extensible; return true; }
    if (channels <= 2 && client->IsFormatSupported(sharemode, (WAVEFORMATEX*)&fmt_ex, NULL) == S_OK) { *out_wave_fmt = *(WAVEFORMATEXTENSIBLE*)&fmt_ex; return true; }

    if (bitspersample == 16 || bitspersample == 24) {
        fmt_extensible.Format.nAvgBytesPerSec = fmt_ex.nAvgBytesPerSec = samplerate * 4 * channels;
        fmt_extensible.Format.nBlockAlign = fmt_ex.nBlockAlign = 4 * channels;
        fmt_extensible.Format.nSamplesPerSec = fmt_ex.nSamplesPerSec = samplerate;
        fmt_extensible.Format.wBitsPerSample = fmt_ex.wBitsPerSample = 32;
        fmt_extensible.Samples.wValidBitsPerSample = 24;

        if (client->IsFormatSupported(sharemode, (WAVEFORMATEX*)&fmt_extensible, NULL) == S_OK) { *out_wave_fmt = *(WAVEFORMATEXTENSIBLE*)&fmt_extensible; return true; }
        if (channels <= 2 && client->IsFormatSupported(sharemode, (WAVEFORMATEX*)&fmt_ex, NULL) == S_OK) { *out_wave_fmt = *(WAVEFORMATEXTENSIBLE*)&fmt_ex; return true; }

        fmt_extensible.Format.nAvgBytesPerSec = fmt_ex.nAvgBytesPerSec = samplerate * 3 * channels;
        fmt_extensible.Format.nBlockAlign = fmt_ex.nBlockAlign = 3 * channels;
        fmt_extensible.Format.nSamplesPerSec = fmt_ex.nSamplesPerSec = samplerate;
        fmt_extensible.Format.wBitsPerSample = fmt_ex.wBitsPerSample = 24;
        fmt_extensible.Samples.wValidBitsPerSample = 24;

        if (client->IsFormatSupported(sharemode, (WAVEFORMATEX*)&fmt_extensible, NULL) == S_OK) { *out_wave_fmt = *(WAVEFORMATEXTENSIBLE*)&fmt_extensible; return true; }
        if (channels <= 2 && client->IsFormatSupported(sharemode, (WAVEFORMATEX*)&fmt_ex, NULL) == S_OK) { *out_wave_fmt = *(WAVEFORMATEXTENSIBLE*)&fmt_ex; return true; }
    }

    if (bitspersample == 16) {
        fmt_extensible.Format.nAvgBytesPerSec = fmt_ex.nAvgBytesPerSec = samplerate * 2 * channels;
        fmt_extensible.Format.nBlockAlign = fmt_ex.nBlockAlign = 2 * channels;
        fmt_extensible.Format.nSamplesPerSec = fmt_ex.nSamplesPerSec = samplerate;
        fmt_extensible.Format.wBitsPerSample = fmt_ex.wBitsPerSample = 16;
        fmt_extensible.Samples.wValidBitsPerSample = 16;

        if (client->IsFormatSupported(sharemode, (WAVEFORMATEX*)&fmt_extensible, NULL) == S_OK) { *out_wave_fmt = *(WAVEFORMATEXTENSIBLE*)&fmt_extensible; return true; }
        if (channels <= 2 && client->IsFormatSupported(sharemode, (WAVEFORMATEX*)&fmt_ex, NULL) == S_OK) { *out_wave_fmt = *(WAVEFORMATEXTENSIBLE*)&fmt_ex; return true; }
    }

    if (self->nasty_format_probe && self->exclusive_mode) {
        // try the hard way (only for exclusive mode)
        if (bitspersample == 16 || bitspersample == 24 || bitspersample == 32) {
            fmt_extensible.Format.nAvgBytesPerSec = fmt_ex.nAvgBytesPerSec = samplerate * 4 * channels;
            fmt_extensible.Format.nBlockAlign = fmt_ex.nBlockAlign = 4 * channels;
            fmt_extensible.Format.nSamplesPerSec = fmt_ex.nSamplesPerSec = samplerate;
            fmt_extensible.Format.wBitsPerSample = fmt_ex.wBitsPerSample = 32;
            fmt_extensible.Samples.wValidBitsPerSample = 32;

            if (check_format_the_nasty_way(self, sharemode, flags, reinterpret_cast<WAVEFORMATEX*>(&fmt_extensible))) { *out_wave_fmt = *(WAVEFORMATEXTENSIBLE*)&fmt_extensible; return true; }
            if (channels <= 2 && check_format_the_nasty_way(self, sharemode, flags, reinterpret_cast<WAVEFORMATEX*>(&fmt_ex))) { *out_wave_fmt = *(WAVEFORMATEXTENSIBLE*)&fmt_ex; return true; }
        }

        if (bitspersample == 16 || bitspersample == 24) {
            fmt_extensible.Format.nAvgBytesPerSec = fmt_ex.nAvgBytesPerSec = samplerate * 4 * channels;
            fmt_extensible.Format.nBlockAlign = fmt_ex.nBlockAlign = 4 * channels;
            fmt_extensible.Format.nSamplesPerSec = fmt_ex.nSamplesPerSec = samplerate;
            fmt_extensible.Format.wBitsPerSample = fmt_ex.wBitsPerSample = 32;
            fmt_extensible.Samples.wValidBitsPerSample = 24;

            if (check_format_the_nasty_way(self, sharemode, flags, reinterpret_cast<WAVEFORMATEX*>(&fmt_extensible))) { *out_wave_fmt = *(WAVEFORMATEXTENSIBLE*)&fmt_extensible; return true; }
            if (channels <= 2 && check_format_the_nasty_way(self, sharemode, flags, reinterpret_cast<WAVEFORMATEX*>(&fmt_ex))) { *out_wave_fmt = *(WAVEFORMATEXTENSIBLE*)&fmt_ex; return true; }

            fmt_extensible.Format.nAvgBytesPerSec = fmt_ex.nAvgBytesPerSec = samplerate * 3 * channels;
            fmt_extensible.Format.nBlockAlign = fmt_ex.nBlockAlign = 3 * channels;
            fmt_extensible.Format.nSamplesPerSec = fmt_ex.nSamplesPerSec = samplerate;
            fmt_extensible.Format.wBitsPerSample = fmt_ex.wBitsPerSample = 24;
            fmt_extensible.Samples.wValidBitsPerSample = 24;

            if (check_format_the_nasty_way(self, sharemode, flags, reinterpret_cast<WAVEFORMATEX*>(&fmt_extensible))) { *out_wave_fmt = *(WAVEFORMATEXTENSIBLE*)&fmt_extensible; return true; }
            if (channels <= 2 && check_format_the_nasty_way(self, sharemode, flags, reinterpret_cast<WAVEFORMATEX*>(&fmt_ex))) { *out_wave_fmt = *(WAVEFORMATEXTENSIBLE*)&fmt_ex; return true; }
        }

        if (bitspersample == 16) {
            fmt_extensible.Format.nAvgBytesPerSec = fmt_ex.nAvgBytesPerSec = samplerate * 2 * channels;
            fmt_extensible.Format.nBlockAlign = fmt_ex.nBlockAlign = 2 * channels;
            fmt_extensible.Format.nSamplesPerSec = fmt_ex.nSamplesPerSec = samplerate;
            fmt_extensible.Format.wBitsPerSample = fmt_ex.wBitsPerSample = 16;
            fmt_extensible.Samples.wValidBitsPerSample = 16;

            if (check_format_the_nasty_way(self, sharemode, flags, reinterpret_cast<WAVEFORMATEX*>(&fmt_extensible))) { *out_wave_fmt = *(WAVEFORMATEXTENSIBLE*)&fmt_extensible; return true; }
            if (channels <= 2 && check_format_the_nasty_way(self, sharemode, flags, reinterpret_cast<WAVEFORMATEX*>(&fmt_ex))) { *out_wave_fmt = *(WAVEFORMATEXTENSIBLE*)&fmt_ex; return true; }
        }
    }

    return false;
}

static RC__Status
probe_formats(WasapiOutputPlugin *self) {
    int pcmbaserate[] = { 44100, 48000,       0 };
    int pcmmult[] = { 1, 2, 4, 8,         0 };
    int channels[] = { 1, 2, 3, 4, 5, 6, 7, 8, 0 };
    int pcmbitspersample[] = { 16, 24, 32,         0 };
    int dsdrate[] = { 44100 * 64, 44100 * 128, 44100 * 256, 0 };

    int channel_i, pcmbaserate_i, pcmmult_i, pcmbitspersample_i, dsdrate_i;
    int idx = 0;

    size_t                nformats    = count(channels) * (count(pcmbaserate) * count(pcmmult) * count(pcmbitspersample) + count(dsdrate));
    RAAT__StreamFormat   *formats     = RC__new0(self->alloc, RAAT__StreamFormat, (int)nformats);
    WAVEFORMATEXTENSIBLE *waveformats = RC__new0(self->alloc, WAVEFORMATEXTENSIBLE, (int)nformats);
    WAVEFORMATEXTENSIBLE waveformat;

    if (formats == NULL) return RC__STATUS_OUT_OF_MEMORY;

    IAudioClient *client = NULL;

    HRESULT hr = self->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&client);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[output/wasapi] [%s] Failed to activate IAudioClient HRESULT=0x%x", self->device_name, hr);
        goto fail;
    }

    for (channel_i = 0; channels[channel_i]; channel_i++) {
        int test_channels = channels[channel_i];

        for (pcmmult_i = 0; pcmmult[pcmmult_i]; pcmmult_i++)
            for (pcmbaserate_i = 0; pcmbaserate[pcmbaserate_i]; pcmbaserate_i++)
                for (pcmbitspersample_i = 0; pcmbitspersample[pcmbitspersample_i]; pcmbitspersample_i++) {
                    int test_samplerate = pcmbaserate[pcmbaserate_i] * pcmmult[pcmmult_i];
                    int test_bitspersample = pcmbitspersample[pcmbitspersample_i];

                    if (probe_one_format(self, client, test_samplerate, test_bitspersample, test_channels, &waveformat)) {
                        formats[idx].sample_type = RAAT__SAMPLE_TYPE_PCM;
                        formats[idx].sample_rate = test_samplerate;
                        formats[idx].bits_per_sample = test_bitspersample;
                        formats[idx].channels = test_channels;
                        waveformats[idx] = waveformat;
                        idx++;
                        RAAT__TRACE("[output/wasapi] [%s] supports PCM format %d/%d/%d", self->device_name, test_samplerate, test_bitspersample, test_channels);
                    }
                }

        if (self->exclusive_mode && self->dsd_mode != DSD_MODE_NONE) {            // no DSD in shared mode
            for (dsdrate_i = 0; dsdrate[dsdrate_i]; dsdrate_i++) {
                int test_samplerate = dsdrate[dsdrate_i] / 16;
                if (probe_one_format(self, client, test_samplerate, 24, test_channels, &waveformat)) {
                    formats[idx].sample_type = RAAT__SAMPLE_TYPE_DSD;
                    formats[idx].sample_rate = dsdrate[dsdrate_i];
                    formats[idx].bits_per_sample = 1;
                    formats[idx].channels = test_channels;
                    waveformats[idx] = waveformat;
                    idx++;
                    RAAT__TRACE("[output/wasapi] [%s] supports DSD format %d/%d/%d", self->device_name, dsdrate[dsdrate_i], 1, test_channels);
                }
            }
        }
    }

    self->n_supported_formats   = idx;
    self->supported_formats     = formats;
    self->supported_waveformats = waveformats;

    SafeRelease(&client);
    return RC__STATUS_SUCCESS;

fail:
    SafeRelease(&client);
    return RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED;
}

RC__Status
RAAT__wasapi_output_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__OutputPlugin **out_output) {
    alloc = RC__allocator_default(alloc);
    WasapiOutputPlugin *self = RC__new0(alloc, WasapiOutputPlugin, 1);
    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;

    const char *device_id = json_string_value(json_object_get(config, "device"));
    if (device == NULL) { device_id = "default"; }        // for shared mode default device output
    HRESULT hr;

    // need COM or CoCreateInstance will fail
    CoInitialize(NULL);

    // initialize the state that is associated with WasapiOutputPlugin 
    self->alloc = alloc;
    self->log = RAAT__device_get_log(device);
    self->current_token = RAAT__OUTPUT_TOKEN_INVALID;
    self->next_token = 1;
    self->state = IDLE;

    // set up vtable of plugin functions
    self->plugin.get_info = output_get_info;
    self->plugin.get_supported_formats = output_get_supported_formats;
    self->plugin.setup = output_setup;
    self->plugin.teardown = output_teardown;
    self->plugin.start = output_start;
    self->plugin.get_local_time = output_get_local_time;
    self->plugin.set_remote_time = output_set_remote_time;
    self->plugin.stop = output_stop;
    self->plugin.force_teardown = output_force_teardown;
    self->plugin.send_message = output_send_message;
    self->plugin.add_message_listener = output_add_message_listener;
    self->plugin.remove_message_listener = output_remove_message_listener;
    self->plugin.set_software_volume = output_set_software_volume;
    self->plugin.get_output_delay = output_get_output_delay;

    RAAT__output_message_listeners_init(&self->message_listeners, self->alloc);

    self->info = json_object();
    json_object_set(self->info, "config", config);

    uv_mutex_init(&self->lock);

    RC__Status status = RC__STATUS_SUCCESS;

    // now that we have loaded config and done some basic setup, init some audio stuff
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&self->device_enumerator);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[output/wasapi] failed to get device enumerator HRESULT=0x%x", hr);
        status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED;
        goto fail;
    }

    if (!strcmp(device_id, "default")) {
        hr = self->device_enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &self->device);
        if (!SUCCEEDED(hr)) {
            RAAT__ERROR("[output/wasapi] Failed to get default audio endpoint HRESULT=0x%x", hr);
            status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED;
            goto fail;
        }

        LPWSTR device_id_wide;
        hr = self->device->GetId(&device_id_wide);
        if (!SUCCEEDED(hr)) {
            RAAT__ERROR("[output/wasapi] Failed to get device id HRESULT=0x%x", hr);
            status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED;
            goto fail;
        }

        size_t device_id_wide_size = sizeof(wchar_t)*(wcslen(device_id_wide) + 1);
        self->device_id_wide = (wchar_t*)RC__alloc0(self->alloc, device_id_wide_size);
        memcpy(self->device_id_wide, device_id_wide, device_id_wide_size);

        size_t device_id_size = wcstombs(NULL, device_id_wide, 0) + 1;
        self->device_id = RC__new0(self->alloc, char, device_id_size);
        wcstombs(self->device_id, device_id_wide, device_id_size);
        CoTaskMemFree(device_id_wide);
        self->default_device = true;
    } else {
        size_t device_id_len = sizeof(wchar_t)*(_mbstrlen(device_id) + 1);
        self->device_id = RC__allocator_strdup(self->alloc, device_id);
        self->device_id_wide = (wchar_t*)RC__alloc0(self->alloc, device_id_len);
        mbstowcs(self->device_id_wide, self->device_id, _mbstrlen(self->device_id));

        hr = self->device_enumerator->GetDevice(self->device_id_wide, &self->device);
        if (!SUCCEEDED(hr)) {
            RAAT__ERROR("[output/wasapi] [%s] Failed to get device HRESULT=0x%x", self->device_id, hr);
            status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED;
            goto fail;
        }
    }
    RAAT__TRACE("[output/wasapi] [%s] initializing output", device_id);

    // parse configuration
    json_t *buffer_duration = json_object_get(config, "buffer_duration");
    if (buffer_duration) self->buffer_duration_secs = json_number_value(buffer_duration);
    if (self->buffer_duration_secs == 0) self->buffer_duration_secs = .100;
    RAAT__TRACE("[output/wasapi] [%s] preferred buffer duration=%fs", self->device_id, self->buffer_duration_secs);

    json_t *custom_signal_path = json_object_get(config, "signal_path");
    if (custom_signal_path) self->custom_signal_path = json_copy(custom_signal_path);

    json_t *resync_delay = json_object_get(config, "resync_delay");
    if (resync_delay) self->resync_delay_secs = json_number_value(resync_delay);
    if (self->resync_delay_secs == 0) self->resync_delay_secs = 0.1;
    RAAT__TRACE("[output/wasapi] [%s] resync delay=%fs", self->device_id, self->resync_delay_secs);

    json_t *max_dsd_rate = json_object_get(config, "max_dsd_rate");
    if (max_dsd_rate) self->max_dsd_rate = json_number_value(max_dsd_rate);
    if (self->max_dsd_rate == 0) self->max_dsd_rate = 256;
    RAAT__TRACE("[output/wasapi] [%s] max dsd rate=%d", self->device_id, self->max_dsd_rate);

    json_t *exclusive_mode = json_object_get(config, "exclusive_mode");
    self->exclusive_mode = json_is_true(exclusive_mode);
    RAAT__TRACE("[output/wasapi] [%s] exclusive mode=%d", self->device_id, self->exclusive_mode);

    json_t *event_driven_mode = json_object_get(config, "event_driven_mode");
    self->event_driven_mode = json_is_true(event_driven_mode);
    RAAT__TRACE("[output/wasapi] [%s] event_driven mode=%d", self->device_id, self->event_driven_mode);

    json_t *nasty_format_probe = json_object_get(config, "nasty_format_probe");
    self->nasty_format_probe = json_is_true(nasty_format_probe);
    RAAT__TRACE("[output/wasapi] [%s] nasty_format_probe=%d", self->device_id, self->nasty_format_probe);

    if (json_is_true(json_object_get(config, "force_max_volume")))
        self->force_max_volume = true;
    RAAT__TRACE("[output/wasapi] [%s] force_max_volume=%d", self->device_id, self->force_max_volume);

    const char *dsd_mode_s = json_string_value(json_object_get(config, "dsd_mode"));
    if (dsd_mode_s && !strcmp(dsd_mode_s, "dop")) self->dsd_mode = DSD_MODE_DOP;
    else if (dsd_mode_s && !strcmp(dsd_mode_s, "dcs")) self->dsd_mode = DSD_MODE_DCS;
    else                                               self->dsd_mode = DSD_MODE_NONE;

    hr = self->device->OpenPropertyStore(STGM_READ, &self->deviceprops);
    if (!SUCCEEDED(hr)) {
        RAAT__WARNING("[output/wasapi] [%s] failed to open property store from device HRESULT=0x%x", self->device_id, hr);
        status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED;
        goto fail;
    }

    BOOL has_devicedesc = TRUE;
    PROPVARIANT ifacenameprop;
    PropVariantInit(&ifacenameprop);
    hr = self->deviceprops->GetValue(PKEY_DeviceInterface_FriendlyName, &ifacenameprop);
    if (!SUCCEEDED(hr)) {
        PropVariantClear(&ifacenameprop);
        RAAT__ERROR("[output/wasapi] failed to get desc from device HRESULT=0x%x", self->device_id, hr);
        status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED;
        goto fail;
    }
    size_t device_name_len = wcstombs(NULL, ifacenameprop.pwszVal, 0) + 1;
    self->device_name = RC__new0(self->alloc, char, device_name_len);
    wcstombs(self->device_name, ifacenameprop.pwszVal, device_name_len);
    PropVariantClear(&ifacenameprop);

    if (self->default_device && self->exclusive_mode) {
        RAAT__WARNING("[output/wasapi] [%s] config requested exclusive mode, but config uses default device, so turning it off", self->device_name);
        self->exclusive_mode = false;
    }

    if (self->event_driven_mode && !self->exclusive_mode) {
        RAAT__WARNING("[output/wasapi] [%s] config requested event driven mode, but config uses shared mode, so turning it off", self->device_name);
        self->event_driven_mode = false;
    }

    /*
    if (self->event_driven_mode) {
        PROPVARIANT eventdrivenprop;
        PropVariantInit(&eventdrivenprop);
        hr = self->deviceprops->GetValue(PKEY_AudioEndpoint_Supports_EventDriven_Mode, &eventdrivenprop);
        if (!SUCCEEDED(hr) || !eventdrivenprop.uintVal) {
            RAAT__WARNING("[output/wasapi] [%s] config requested event driven mode, but device does not actually support it, so turning it off", self->device_name);
            self->event_driven_mode = false;
        } else {
            RAAT__TRACE("[output/wasapi] [%s] device actually supports event driven mode", self->device_name);
        }
        PropVariantClear(&eventdrivenprop);
    }
    */

    status = probe_formats(self);

    if (!RC__STATUS_IS_SUCCESS(status)) goto fail;

    if (self->default_device) {
        self->default_device_notifications = new WasapiOutput_DefaultDeviceNotifications(self);
        self->device_enumerator->RegisterEndpointNotificationCallback(self->default_device_notifications);
    }

    RAAT__TRACE("[output/wasapi] initialized");
    *out_output = &self->plugin;
    return RC__STATUS_SUCCESS;

fail:
    RAAT__wasapi_output_plugin_delete((RAAT__OutputPlugin*)self);
    return status;
}

void
RAAT__wasapi_output_plugin_delete(RAAT__OutputPlugin *output) {
    WasapiOutputPlugin *self = (WasapiOutputPlugin*)output;

    uv_mutex_destroy(&self->lock);

    if (self->info) json_decref(self->info);
    if (self->custom_signal_path) json_decref(self->custom_signal_path);
    RAAT__output_message_listeners_destroy(&self->message_listeners);

    if (self->default_device_notifications) {
        self->device_enumerator->UnregisterEndpointNotificationCallback(self->default_device_notifications);
        self->default_device_notifications->Release();
    }
    SafeRelease(&self->device);
    SafeRelease(&self->device_enumerator);

    RC__free(self->alloc, self->supported_formats);
    RC__free(self->alloc, self->supported_waveformats);
    RC__free(self->alloc, self->device_id);
    RC__free(self->alloc, self->device_id_wide);
    RC__free(self->alloc, self->device_name);
    RC__free(self->alloc, self);
}

