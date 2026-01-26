//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_plugin_volume_wasapi.h"
#include "rc_list.h"

#include <uv.h>

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
 * Utilities
 */
template <class T> void SafeRelease(T **ppT) {
    if (*ppT) {
        (*ppT)->Release();
        *ppT = NULL;
    }
}

class WasapiVolume_DefaultDeviceNotifications;
static void ExclusiveVolumeThread(void *vself);
static void SharedVolumeThread(void *vself);

/*
 * Volume Plugin
 */
typedef struct {
    RAAT__VolumePlugin           plugin;          // must be first item in struct
    RC__Allocator               *alloc;
    RAAT__Log                   *log;
    RAAT__VolumeStateListeners   state_listeners;
    uv_mutex_t                   lock;
    uv_thread_t                  tid;
    bool                         thread_alive;
    bool                         volume_dirty;
    bool                         mute_dirty;
    HANDLE                       setvolume_event;
    HRESULT                      init_status;
    HANDLE                       init_event;
    char                        *device_id;
    char                        *device_name;
    wchar_t                     *device_id_wide;
    bool                         exclusive_mode;
    bool                         default_device;
    json_t                      *info;

    // Current state. Note that these use the win32 types, and we cast/convert in LOCKED_get_state
    BOOL                         mute;
    float                        volume;

    float                         min_db, max_db;

    IMMDeviceEnumerator            *device_enumerator;
    WasapiVolume_DefaultDeviceNotifications    *default_device_notifications;
} WasapiVolumePlugin;

static RC__Status volume_get_info(void *vself, json_t **out_info) {
    WasapiVolumePlugin *self = (WasapiVolumePlugin*)vself;

    RC__ASSERT(self);
    RC__ASSERT(out_info);

    json_incref(self->info);
    *out_info = self->info;

    return RC__STATUS_SUCCESS;
}

static RC__Status volume_add_state_listener(void *vself, RAAT__VolumeStateCallback cb, void *userdata) {
    WasapiVolumePlugin *self = (WasapiVolumePlugin*)vself;
    return RAAT__volume_state_listeners_add(&self->state_listeners, cb, userdata);
}

static RC__Status volume_remove_state_listener(void *vself, RAAT__VolumeStateCallback cb, void *userdata) {
    WasapiVolumePlugin *self = (WasapiVolumePlugin*)vself;
    return RAAT__volume_state_listeners_remove(&self->state_listeners, cb, userdata);
}

static void LOCKED_get_state(WasapiVolumePlugin *self, RAAT__VolumeState *out_state) {
    out_state->volume_type  = RAAT__VOLUME_TYPE_NUMBER;
    out_state->min_volume   = 0;
    out_state->max_volume   = 100;
    out_state->volume_value = (int)(self->volume*100 + 0.5);
    out_state->mute_value   = self->mute ? true : false;
    out_state->db_max_volume = self->max_db;
    out_state->db_min_volume = self->min_db;
    out_state->volume_step   = 1.0;
}

static RC__Status volume_get_state(void *vself, RAAT__VolumeState *out_state) {
    WasapiVolumePlugin *self = (WasapiVolumePlugin*)vself;

    RC__ASSERT(self != NULL);
    RC__ASSERT(out_state != NULL);

    uv_mutex_lock(&self->lock);
    LOCKED_get_state(self, out_state);
    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

static void notify_volume_mute_changed(WasapiVolumePlugin *self, float volume, BOOL mute) {
    RAAT__VolumeState state;
    bool changed = false;

    uv_mutex_lock(&self->lock);
    if (self->volume != volume || self->mute != mute) {
        self->volume = volume;
        self->mute = mute;
        RAAT__TRACE("[volume/wasapi] [%s] volume => %d, mute => %d", self->device_name, (int)(volume * 100 + 0.5), mute);
        changed = true;
        LOCKED_get_state(self, &state);
    }
    uv_mutex_unlock(&self->lock);

    if (changed) RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);
}

static RC__Status volume_set_volume(void *vself, double volume_value) {
    WasapiVolumePlugin *self = (WasapiVolumePlugin*)vself;
    RAAT__VolumeState state;
    bool changed = false;

    uv_mutex_lock(&self->lock);
    if (self->volume != volume_value) {
        self->volume_dirty = true;
        self->volume       = (float)volume_value/100.0;
        RAAT__TRACE("[volume/wasapi] [%s] volume => %d", self->device_name, volume_value);
        changed = true;
        LOCKED_get_state(self, &state);
    }
    uv_mutex_unlock(&self->lock);

    SetEvent(self->setvolume_event);

    if (changed) RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);

    return RC__STATUS_SUCCESS;
}

static RC__Status volume_set_mute(void *vself, bool mute_value) {
    WasapiVolumePlugin *self = (WasapiVolumePlugin*)vself;
    RAAT__VolumeState state;
    bool changed = false;

    uv_mutex_lock(&self->lock);
    if (self->mute != (mute_value ? TRUE : FALSE)) {
        self->mute_dirty = true;
        self->mute       = mute_value ? TRUE : FALSE;
        SetEvent(self->setvolume_event);
        RAAT__TRACE("[volume/wasapi] [%s] mute => %d", self->device_name, mute_value);
        changed = true;
        LOCKED_get_state(self, &state);
    }
    uv_mutex_unlock(&self->lock);
    
    if (changed) RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);

    return RC__STATUS_SUCCESS;
}

static void notify_default_device_changed(WasapiVolumePlugin *self, LPCWSTR device_id) {
    IMMDevice *device = NULL;
    IPropertyStore *deviceprops = NULL;
    HRESULT hr;

    RAAT__TRACE("[volume/wasapi] [default] default device changed. Re-initializing");

    hr = self->device_enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] [notify_default_device_changed] Failed to get default audio endpoint HRESULT=0x%x", hr);
        goto cleanup;
    }

    LPWSTR device_id_wide;
    hr = device->GetId(&device_id_wide);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] [notify_default_device_changed] Failed to get device id HRESULT=0x%x", hr);
        goto cleanup;
    }

    hr = device->OpenPropertyStore(STGM_READ, &deviceprops);
    if (!SUCCEEDED(hr)) {
        RAAT__WARNING("[volume/wasapi] [default] Failed to open property store from device HRESULT=0x%x", hr);
        goto cleanup;
    }

    BOOL has_devicedesc = TRUE;
    PROPVARIANT ifacenameprop;
    PropVariantInit(&ifacenameprop);
    hr = deviceprops->GetValue(PKEY_DeviceInterface_FriendlyName, &ifacenameprop);
    if (!SUCCEEDED(hr)) {
        PropVariantClear(&ifacenameprop);
        RAAT__ERROR("[volume/wasapi] [Failed to get desc from device HRESULT=0x%x", self->device_id, hr);
        goto cleanup;
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

    SetEvent(self->setvolume_event);

cleanup:
    SafeRelease(&device);
    SafeRelease(&deviceprops);
}

class WasapiVolume_VolumeThreadNotifications : public IAudioEndpointVolumeCallback, public IAudioSessionEvents, virtual IUnknown {
    ULONG                _cRef;
    WasapiVolumePlugin *_self;

public:
    WasapiVolume_VolumeThreadNotifications(WasapiVolumePlugin *self) : _cRef(1), _self(self) { }

    /* IAudioEndpointVolumeCallback */
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA pNotify) {
        notify_volume_mute_changed(_self, pNotify->fMasterVolume, pNotify->bMuted);
        return S_OK;
    }

    /* IAudioSessionEvents */
    HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR NewDisplayName, LPCGUID EventContext) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR NewIconPath, LPCGUID EventContext) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD ChannelCount, float NewChannelVolumeArray[], DWORD ChangedChannel, LPCGUID EventContext) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID NewGroupingParam, LPCGUID EventContext) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState NewState) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason DisconnectReason) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float NewVolume, BOOL NewMute, LPCGUID EventContext) {
        notify_volume_mute_changed(_self, NewVolume, NewMute);
        return S_OK;
    }

    ~WasapiVolume_VolumeThreadNotifications() { }

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
        else if (__uuidof(IAudioSessionEvents) == riid) {
            AddRef();
            *ppvInterface = (IAudioSessionEvents*)this;
        } else if (__uuidof(IAudioEndpointVolumeCallback) == riid) {
            AddRef();
            *ppvInterface = (IAudioEndpointVolumeCallback*)this;
        } else {
            *ppvInterface = NULL;
            return E_NOINTERFACE;
        }
        return S_OK;
    }
};

static void ExclusiveVolumeThread(void *vself) {
    WasapiVolumePlugin                        *self                      = (WasapiVolumePlugin*)vself;
    IMMDevice                                *device                   = NULL;
    IAudioEndpointVolume                    *volume                   = NULL;
    IMMDeviceEnumerator                        *device_enumerator        = NULL;
    WasapiVolume_VolumeThreadNotifications    *notifications              = NULL;
    BOOL                                     didinit                  = FALSE;

    HRESULT hr;

    CoInitialize(NULL);

    notifications = new WasapiVolume_VolumeThreadNotifications(self);

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&device_enumerator);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] [%s] Failed to get device enumerator HRESULT=0x%x", self->device_name, hr);
        goto failexit;
    }
    hr = device_enumerator->GetDevice(self->device_id_wide, &device);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] [%s] Failed to get default audio endpoint HRESULT=0x%x", self->device_name, hr);
        goto failexit;
    }
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&volume);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] [%s] Failed to activate IAudioEndpointVolume HRESULT=0x%x", self->device_name, hr);
        goto failexit;
    }

    DWORD hw_support_mask = 0;
    hr = volume->QueryHardwareSupport(&hw_support_mask);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] [%s] Failed to query hardware support HRESULT=0x%x", self->device_name, hr);
        goto failexit;
    }

    float inc_db;
    hr = volume->GetVolumeRange(&self->min_db, &self->max_db, &inc_db);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] [%s] Failed to get volume range HRESULT=0x%x", self->device_name, hr);
    } else {
        RAAT__TRACE("[volume/wasapi] [%s] Got volume range %fdB-%fdB increment=%fdB", self->device_name, self->min_db, self->max_db, inc_db);
    }

    if ((hw_support_mask & ENDPOINT_HARDWARE_SUPPORT_VOLUME) == 0) {
        RAAT__TRACE("[volume/wasapi] [%s] Hardware doesn't support volume control, so failing init", self->device_name);
        goto failexit;
    }

    hr = volume->RegisterControlChangeNotify(notifications);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] [%s] Failed to register audio session notification HRESULT=0x%x", self->device_name, hr);
        goto failexit;
    }
    hr = volume->GetMasterVolumeLevelScalar(&self->volume);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] [%s] Failed to get master volume HRESULT=0x%x", self->device_name, hr);
        goto failexit;
    }
    hr = volume->GetMute(&self->mute);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] [%s] Failed to get mute HRESULT=0x%x", self->device_name, hr);
        goto failexit;
    }

    self->setvolume_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!self->setvolume_event) {
        RAAT__ERROR("[volume/wasapi] [%s] Failed to create event handle for setvolume", self->device_name);
        goto failexit;
    }

    self->init_status = 0;
    didinit = TRUE;
    SetEvent(self->init_event);

    // whenever the setvolume event is set, wake up, set vol+mute, then go
    // back to sleep
    while (self->thread_alive) {
        RAAT__TRACE("[volume/wasapi] [%s] Set volume to %f [EXCLUSIVE]", self->device_name, (double)self->volume);
        if (self->volume_dirty) {
            hr = volume->SetMasterVolumeLevelScalar(self->volume, NULL);
            if (!SUCCEEDED(hr)) {
                RAAT__ERROR("[volume/wasapi] [%s] warning: setting volume failed", self->device_name);
            }
            self->volume_dirty = false;
        }
        if (self->mute_dirty) {
            hr = volume->SetMute(self->mute, NULL);
            if (!SUCCEEDED(hr)) {
                RAAT__ERROR("[volume/wasapi] [%s] warning: setting mute failed", self->device_name);
            }
            self->mute_dirty = false;
        }
        WaitForSingleObject(self->setvolume_event, INFINITE);
    }

failexit:
    volume->UnregisterControlChangeNotify(notifications);
    SafeRelease(&device);
    SafeRelease(&device_enumerator);
    SafeRelease(&volume);
    notifications->Release();
    if (!didinit) {
        self->init_status = hr == 0 ? -1 : hr;
        SetEvent(self->init_event);
    }
}

static void SharedVolumeThread(void *vself) {
    WasapiVolumePlugin *self = (WasapiVolumePlugin*)vself;

    IMMDeviceEnumerator                         *device_enumerator  = NULL;
    IMMDevice                                 *device             = NULL;
    IAudioSessionManager                     *sessionmgr         = NULL;
    ISimpleAudioVolume                         *volume             = NULL;
    IAudioSessionControl                     *control             = NULL;
    IAudioClient                             *client             = NULL;
    WasapiVolume_VolumeThreadNotifications     *notifications         = NULL;
    BOOL                                      didinit             = FALSE;

device_changed:
	if (control) control->RegisterAudioSessionNotification(notifications);
    SafeRelease(&client);
    SafeRelease(&device);
    SafeRelease(&device_enumerator);
    SafeRelease(&control);
    SafeRelease(&sessionmgr);
    SafeRelease(&notifications);

    uv_mutex_lock(&self->lock);
    wchar_t *device_id_wide = NULL;
    if (device_id_wide) free(device_id_wide);
    device_id_wide = _wcsdup(self->device_id_wide);
    uv_mutex_unlock(&self->lock);

    HRESULT hr;

    CoInitialize(NULL);

    notifications = new WasapiVolume_VolumeThreadNotifications(self);

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&device_enumerator);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] [%s] Failed to get device enumerator HRESULT=0x%x", self->device_name, hr);
        goto failexit;
    }
    hr = device_enumerator->GetDevice(self->device_id_wide, &device);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] [%s] Failed to get default audio endpoint HRESULT=0x%x", self->device_name, hr);
        goto failexit;
    }
    hr = device->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, NULL, (void**)&sessionmgr);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] [%s] Failed to activate IAudioSessionManager HRESULT=0x%x", self->device_name, hr);
        goto failexit;
    }
    hr = sessionmgr->GetSimpleAudioVolume(NULL, FALSE, &volume);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] [%s] Failed to get volume control HRESULT=0x%x", self->device_name, hr);
        goto failexit;
    }
    hr = sessionmgr->GetAudioSessionControl(NULL, 0, &control);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] [%s] Failed to get audio session control HRESULT=0x%x", self->device_name, hr);
        goto failexit;
    }
    hr = control->RegisterAudioSessionNotification(notifications);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] [%s] Failed to register audio session notification HRESULT=0x%x", self->device_name, hr);
        goto failexit;
    }
    hr = volume->GetMasterVolume(&self->volume);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] [%s] Failed to get master volume HRESULT=0x%x", self->device_name, hr);
        goto failexit;
    }
    hr = volume->GetMute(&self->mute);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] [%s] Failed to get mute HRESULT=0x%x", self->device_name, hr);
        goto failexit;
    }

    /*
     * briefly activate a 44100/16 stream so we show up in volume control + ensure basic sanity
     */
    {
        WAVEFORMATEXTENSIBLE   fmt      = {0,};
        fmt.Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
        fmt.Format.nChannels            = 2;
        fmt.Format.cbSize               = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        fmt.dwChannelMask               = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        fmt.SubFormat                   = KSDATAFORMAT_SUBTYPE_PCM;
        fmt.Format.nAvgBytesPerSec      = 44100 * 4;
        fmt.Format.nBlockAlign          = 4;
        fmt.Format.nSamplesPerSec       = 44100;
        fmt.Format.wBitsPerSample       = 16;
        fmt.Samples.wValidBitsPerSample = 16;

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&client);
        if (!SUCCEEDED(hr)) {
            RAAT__ERROR("[volume/wasapi] [%s] Failed to activate IAudioClient HRESULT=0x%x", self->device_name, hr);
            goto failexit;
        }
        Sleep(300);
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 
                                AUDCLNT_STREAMFLAGS_RATEADJUST, 
                                10000000 / 10,    // in 100ns units (doesn't matter for this use case)
                                0,
                                reinterpret_cast<WAVEFORMATEX*>(&fmt), 
                                NULL);
        if (!SUCCEEDED(hr)) {
            RAAT__ERROR("[volume/wasapi] [%s] Failed to initialize IAudioClient (vol) HRESULT=0x%x", self->device_name, hr);
            goto failexit;
        }

        hr = client->Start();
        if (!SUCCEEDED(hr)) {
            RAAT__ERROR("[volume/wasapi] [%s] Failed to start stream HRESULT=0x%x", self->device_name, hr);
            goto failexit;
        }
        hr = client->Stop();
        if (!SUCCEEDED(hr)) {
            RAAT__ERROR("[volume/wasapi] [%s] Failed to start stream HRESULT=0x%x", self->device_name, hr);
            goto failexit;
        }
        client->Reset();
    }

    self->setvolume_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!self->setvolume_event) {
        RAAT__ERROR("[volume/wasapi] [%s] Failed to create event handle for setvolume", self->device_name);
        goto failexit;
    }

    // notify the value, in case this is a re-init
    RAAT__VolumeState state;
    uv_mutex_lock(&self->lock);
    LOCKED_get_state(self, &state);
    uv_mutex_unlock(&self->lock);
    RAAT__volume_state_listeners_invoke(&self->state_listeners, &state);

    self->init_status = 0;
    didinit = TRUE;
    SetEvent(self->init_event);

    // whenever the setvolume event is set, wake up, set vol+mute, then go
    // back to sleep
    while (self->thread_alive) {
        uv_mutex_lock(&self->lock);
        if (wcscmp(self->device_id_wide, device_id_wide)) {
            RAAT__TRACE("[output/wasapi] [default] device changed during streaming! re-opening shared volume thread");
            uv_mutex_unlock(&self->lock);
            goto device_changed;
        }
        uv_mutex_unlock(&self->lock);

        RAAT__TRACE("[volume/wasapi] [%s] Set volume to %f mute to %d [SHARED]", self->device_name, (double)self->volume, self->mute);
        if (self->volume_dirty) {
            volume->SetMasterVolume(self->volume, NULL);
            self->volume_dirty = false;
        }
        if (self->mute_dirty) {
            volume->SetMute(self->mute, NULL);
            self->mute_dirty = false;
        }
        WaitForSingleObject(self->setvolume_event, INFINITE);
    }

failexit:
	if (control) control->RegisterAudioSessionNotification(notifications);
    SafeRelease(&client);
    SafeRelease(&device);
    SafeRelease(&device_enumerator);
    SafeRelease(&control);
    SafeRelease(&sessionmgr);
    SafeRelease(&notifications);
    if (!didinit) {
        self->init_status = hr == 0 ? -1 : hr;
        SetEvent(self->init_event);
    }
}

class WasapiVolume_DefaultDeviceNotifications : public IMMNotificationClient {
    ULONG                _cRef;
    WasapiVolumePlugin *_self;

public:
    WasapiVolume_DefaultDeviceNotifications(WasapiVolumePlugin *self) : _cRef(1), _self(self) { }

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

    ~WasapiVolume_DefaultDeviceNotifications() { }

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
        } else {
            *ppvInterface = NULL;
            return E_NOINTERFACE;
        }
        return S_OK;
    }
};

RC__Status 
RAAT__wasapi_volume_plugin_new(RC__Allocator *alloc, RAAT__Device *raatdevice, json_t *config, RAAT__VolumePlugin **out_volume) { 
    alloc = RC__allocator_default(alloc);
    WasapiVolumePlugin *self            = RC__new0(alloc, WasapiVolumePlugin, 1);
    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;
    self->alloc                        = alloc;
    self->log                          = RAAT__device_get_log(raatdevice);
    self->plugin.get_info              = volume_get_info;
    self->plugin.add_state_listener    = volume_add_state_listener;
    self->plugin.remove_state_listener = volume_remove_state_listener;
    self->plugin.get_state             = volume_get_state;
    self->plugin.set_volume            = volume_set_volume;
    self->plugin.set_mute              = volume_set_mute;

    self->mute     = false;
    self->volume   = 100;
    uv_mutex_init(&self->lock);
    RAAT__volume_state_listeners_init(&self->state_listeners, self->alloc);

    RC__Status status = RC__STATUS_SUCCESS;
    IMMDevice *device = NULL;
    IPropertyStore *deviceprops = NULL;

    const char *device_id = json_string_value(json_object_get(config, "device"));
    if (device_id == NULL) { device_id = "default"; }        // for shared mode default device output
    HRESULT hr;

    // now that we have loaded config and done some basic setup, init some audio stuff
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&self->device_enumerator);
    if (!SUCCEEDED(hr)) {
        RAAT__ERROR("[volume/wasapi] Failed to get device enumerator HRESULT=0x%x", hr);
        status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED;
        goto fail;
    }

    if (!strcmp(device_id, "default")) {
        hr = self->device_enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
        if (!SUCCEEDED(hr)) {
            RAAT__ERROR("[volume/wasapi] Failed to get default audio endpoint HRESULT=0x%x", hr);
            status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED;
            goto fail;
        }

        LPWSTR device_id_wide;
        hr = device->GetId(&device_id_wide);
        if (!SUCCEEDED(hr)) {
            RAAT__ERROR("[volume/wasapi] Failed to get device id HRESULT=0x%x", hr);
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

        hr = self->device_enumerator->GetDevice(self->device_id_wide, &device);
        if (!SUCCEEDED(hr)) {
            RAAT__ERROR("[volume/wasapi] [%s] Failed to get device HRESULT=0x%x", self->device_id, hr);
            status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED;
            goto fail;
        }
    }
    RAAT__TRACE("[volume/wasapi] [%s] initializing output", device_id);

    if (self->default_device) {
        RAAT__WARNING("[volume/wasapi] [%s] config requested exclusive mode, but config uses default device, so turning it off", self->device_name);
        self->exclusive_mode = false;
    }

    // parse configuration
    json_t *exclusive_mode = json_object_get(config, "exclusive_mode");
    self->exclusive_mode = json_is_true(exclusive_mode);
    RAAT__TRACE("[volume/wasapi] [%s] exclusive mode=%d", self->device_id, self->exclusive_mode);

    hr = device->OpenPropertyStore(STGM_READ, &deviceprops);
    if (!SUCCEEDED(hr)) {
        RAAT__WARNING("[volume/wasapi] [%s] Failed to open property store from device HRESULT=0x%x", self->device_id, hr);
        status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED;
        goto fail;
    }

    BOOL has_devicedesc = TRUE;
    PROPVARIANT ifacenameprop;
    PropVariantInit(&ifacenameprop);
    hr = deviceprops->GetValue(PKEY_DeviceInterface_FriendlyName, &ifacenameprop);
    if (!SUCCEEDED(hr)) {
        PropVariantClear(&ifacenameprop);
        RAAT__ERROR("[volume/wasapi] Failed to get desc from device HRESULT=0x%x", self->device_id, hr);
        status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED;
        goto fail;
    }
    size_t device_name_len = wcstombs(NULL, ifacenameprop.pwszVal, 0) + 1;
    self->device_name = RC__new0(self->alloc, char, device_name_len);
    wcstombs(self->device_name, ifacenameprop.pwszVal, device_name_len);
    PropVariantClear(&ifacenameprop);

    if (self->default_device) {
        self->default_device_notifications = new WasapiVolume_DefaultDeviceNotifications(self);
        self->device_enumerator->RegisterEndpointNotificationCallback(self->default_device_notifications);
    }

    self->init_event   = CreateEvent(NULL, FALSE, FALSE, NULL);
    self->init_status  = S_OK;
    self->thread_alive = true;
    if (self->exclusive_mode) {
        uv_thread_create(&self->tid, ExclusiveVolumeThread, self);
    } else {
        uv_thread_create(&self->tid, SharedVolumeThread, self);
    }
    WaitForSingleObject(self->init_event, INFINITE);
    CloseHandle(self->init_event);
    if (!SUCCEEDED(self->init_status)) {
        status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED;
        goto fail;
    }

    self->info = json_object();
    json_object_set(self->info, "config", config);

    RAAT__TRACE("[volume/wasapi] initialized");
    *out_volume = &self->plugin;

fail:
    SafeRelease(&deviceprops);
    SafeRelease(&device);
    if (status != RC__STATUS_SUCCESS) {
        RAAT__wasapi_volume_plugin_delete((RAAT__VolumePlugin*)self);
    }
    return status;
}

void
RAAT__wasapi_volume_plugin_delete(RAAT__VolumePlugin *volume) {
    WasapiVolumePlugin *self = (WasapiVolumePlugin*)volume;
    if (self->thread_alive) {
        self->thread_alive = false;
        SetEvent(self->setvolume_event);
        uv_thread_join(&self->tid);
    }
    if (self->default_device_notifications) {
        self->device_enumerator->UnregisterEndpointNotificationCallback(self->default_device_notifications);
        self->default_device_notifications->Release();
    }
    SafeRelease(&self->device_enumerator);
    uv_mutex_destroy(&self->lock);
    if (self->info) json_decref(self->info);
    RAAT__volume_state_listeners_destroy(&self->state_listeners);
    RC__free(self->alloc, self->device_id);
    RC__free(self->alloc, self->device_id_wide);
    RC__free(self->alloc, self->device_name);
    RC__free(self->alloc, self);
}

