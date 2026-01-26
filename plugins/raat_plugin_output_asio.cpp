// // The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_plugin_output_asio.h"
#include "raat_dsp.h"
#include "rc_list.h"

#include <uv.h>

#include <stdio.h>
#include <asiodrivers.h>
#include <asiolist.h>
#include <asio.h>
#include <iasiodrv.h>
#include <asiosys.h>
#include <asiodrivers.h>

#define RAAT__CURRENT_LOG self->log

/*
 * Output Plugin
 */

const int kMaxOutputChannels = 32;

typedef enum {
    IDLE                = 0,
    STOPPED             = 1,
    RUNNING             = 2,
} AsioOutputState;

typedef enum {
    DSD_MODE_NONE,
    DSD_MODE_NATIVE,
    DSD_MODE_DOP,
    DSD_MODE_DCS,
    DSD_MODE_NATIVE_OR_DOP,
    DSD_MODE_NATIVE_OR_DCS,
} DsdMode;

typedef struct {
    void *userdata;
    ASIOCallbacks asioCallbacks;
} CallbackThunk;

typedef struct {
    RAAT__OutputPlugin           plugin;          // must be first item in struct

    RC__Allocator               *alloc;
    RAAT__Log                   *log;

    uv_mutex_t                   lock;

    RAAT__OutputMessageListeners message_listeners;

    AsioOutputState             state;

    json_t                      *custom_signal_path;
    json_t                      *soft_volume_signal_path;

    int                          next_token;

    RAAT__StreamFormat           origformat;
    RAAT__StreamFormat           procformat;  
    int                          current_token;
    RAAT__OutputLostCallback     cb_lost;
    void                        *cb_lost_userdata;

    RAAT__Stream                *stream;

    int64_t                      last_monotonic_sample_systime;  // in ns (RC__now_ns())
    int64_t                      last_monotonic_sample;          // in samples, based on counter

    int64_t                      start_streamsample;
	int64_t						 streamsample;
    int64_t                      start_time;               
    bool                         new_stream;

	int							 samples_per_buf;
	int							 origformat_samples_per_buf;
	int64_t						 ns_per_buf;

    uint8_t                    *origbuf;
    uint8_t                    *procbuf;   
    uint8_t                    *dsdbuf; 
    int                         bytes_per_origbuf;
	int                         bytes_per_procbuf;

    RAAT__AsioOutputPluginLostCallback   device_lost_cb; 
    void                                *device_lost_userdata;

    // state for asio
    const char                  *driver_id;
	const char                  *driver_name;
    DsdMode                      dsd_mode;
    DsdMode                      effective_dsd_mode;
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

    int                          volume_delay;
    RAAT__DriftCorrection        drift_correction;

    uv_thread_t                  tid;

    RAAT__OutputSoftwareVolume  *volume;

    json_t                      *info;

    bool                         assume_dsd_native_support;

	// ASIO stuff
    bool                         needs_reinit;
	bool						 did_reset_during_block;
	bool						 block_reset;
	bool						 in_start;        
    IASIO						*asio;
	AsioDrivers				    *drivers;
	int							 drvindex;

    // ASIOGetChannels()
    long           asio_input_channels;
    long           asio_output_channels;

    // ASIOGetBufferSize()
    long           asio_min_size;
    long           asio_max_size;
    long           asio_preferred_size;
    long           asio_granularity;

    // ASIOGetSampleRate()
    ASIOSampleRate   asio_sample_rate;
    ASIOIoFormatType asio_format_type;
    bool             supports_io_format;

    // ASIOOutputReady()
    bool           asio_post_output;

    // ASIOGetLatencies ()
    long           asio_input_latency;
    long           asio_output_latency;

    // ASIOCreateBuffers ()
    long           asio_output_buffers;	// becomes number of actual created output buffers
    ASIOBufferInfo asio_buffer_infos[kMaxOutputChannels]; // buffer info's

    // ASIOGetChannelInfo()
    ASIOChannelInfo asio_channel_infos[kMaxOutputChannels]; // channel info's
    // The above two arrays share the same indexing, as the data in them are linked together

    // Information from ASIOGetSamplePosition()
    // data is converted to double floats for easier use, however 64 bit integer can be used, too
    double         asio_nano_seconds;
    double         asio_samples;
    double         asio_tc_samples;	                // time code samples

    // bufferSwitchTimeInfo()
    ASIOTime       asio_t_info;			// time info state
    unsigned long  asio_sys_ref_time;                  // system reference time, when bufferSwitch() was called

    // Signal the end of processing in this example
    bool           asio_stopped;
    bool           use_max_buffer_size;
    bool           power_of_two_buffer_size;

    int64_t        last_start;

    CallbackThunk *thunk;
} AsioOutputPlugin;

#define ASIO64toDouble(a)  ((a).lo + (a).hi * 4294967296.0)

BYTE reverse(unsigned char n) {
    static BYTE bitreverse_lookup[16] = { 0x0, 0x8, 0x4, 0xc, 0x2, 0xa, 0x6, 0xe, 0x1, 0x9, 0x5, 0xd, 0x3, 0xb, 0x7, 0xf, };
    return (bitreverse_lookup[n&15] << 4) | bitreverse_lookup[n>>4];
}

/*
 * Prototypes
 */
long asioMessage(void *userdata, long selector, long value, void* message, double* opt);
void sampleRateDidChange(void *userdata, ASIOSampleRate sRate);
void bufferSwitch(void *userdata, long index, ASIOBool processNow);
ASIOTime *bufferSwitchTimeInfo(void *userdata, ASIOTime *timeInfo, long index, ASIOBool processNow);
static void asio_reset_thread(void *vself);

/*
 * Win32/COM stuff
 *
 * ASIO doesn't pass a userdata type thing in its callbacks. Very lame. We want to host multiple
 * drivers in one process, which on the surface seems OK since they're all just COM inproc servers
 * anyways.
 *
 */
static INIT_ONCE        static_init_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION thunk_lock;
static uv_thread_t      com_thread;
static HWND				com_hwnd;

#define WM_RUNPROC (WM_USER+1)

typedef void(*asio_thread_proc)(void*);

static void run_in_asio_thread_sync(asio_thread_proc proc, void *arg) {
	SendMessage(com_hwnd, WM_RUNPROC, (WPARAM)proc, (LPARAM)arg);
}

LRESULT CALLBACK asio_wndproc(
	_In_ HWND   hwnd,
	_In_ UINT   uMsg,
	_In_ WPARAM wParam,
	_In_ LPARAM lParam) {

	if (uMsg == WM_RUNPROC) {
		asio_thread_proc fptr = (asio_thread_proc)wParam;
		void *arg = (void*)lParam;
		fptr(arg);
		return 0;
	} else {
		return DefWindowProc(hwnd, uMsg, wParam, lParam);
	}
}

/*
 * ASIO drivers are COM in-processes servers that must run in a single-threaded apartment.
 *
 * We choose to make (and pump) our own message pump here. 
 */
static void com_thread_function(void*arg) {
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

	static const char* class_name = "ASIO_MESSAGE_WIN";
	WNDCLASSEX wx = {};
	wx.cbSize = sizeof(WNDCLASSEX);
	wx.lpfnWndProc = asio_wndproc;        // function which will handle messages
	wx.hInstance = NULL;
	wx.lpszClassName = class_name;
	if (RegisterClassEx(&wx)) {
		com_hwnd = CreateWindowEx(0, class_name, "asio_message_win", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
	}

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
}

static BOOL CALLBACK StaticInitFunction(PINIT_ONCE initonce, PVOID param, PVOID *lpcontext) {
    //fprintf(stderr, "performing ASIO static initialization\n"); 
    InitializeCriticalSection(&thunk_lock);
	uv_thread_create(&com_thread, com_thread_function, NULL);
    Sleep(100); // let the thread start (lame hack)
    return TRUE;
}


/*
 * Thunks
 *
 * ASIO doesn't pass a userdata type thing in its callbacks. Very lame. We want to host multiple
 * drivers in one process, which on the surface seems OK since they're all just COM inproc servers
 * anyways.
 *
 * So we statically allocate 64 "thunks", each which comes with its own unique function pointer
 * that knows how to connect it up with user data.
 *
 * These are generated by a python script and live in roon_asio_init_thunks.h
 *
 * The generated thunks call versions of the asio callbacks with userdata, which then call back into Output methods.
 *
 * I don't think you can have 64 asio drivers active without a BSOD, so this should be enough. We can always
 * up the THUNK_COUNT in the python script + regenerate if someone finds otherwise.
 */

//
// This defines the actual thunks: 64 copies of each of the above callback functions that inject user data
//
// It looks like this:
//
// static CallbackThunk thunks[THUNK_COUNT] = { ... }
//
#include "raat_asio_init_thunks.h"

static CallbackThunk *thunk_alloc(void *userdata) {
    if (userdata == NULL) {
        return NULL;
    }
    EnterCriticalSection(&thunk_lock);
    for (int i = 0; i < THUNK_COUNT; i++) {
        if (thunks[i].userdata == NULL) {
            thunks[i].userdata = userdata;
            LeaveCriticalSection(&thunk_lock);
            return &thunks[i];
        }
    }
    LeaveCriticalSection(&thunk_lock);
    return NULL;
}

static void thunk_free(CallbackThunk *thunk) {
    if (thunk == NULL) return;
    EnterCriticalSection(&thunk_lock);
    thunk->userdata = NULL;
    LeaveCriticalSection(&thunk_lock);
}

static RC__Status output_get_info(void *vself, json_t **out_info) {
    AsioOutputPlugin *self = (AsioOutputPlugin*)vself;

    RC__ASSERT(self);
    RC__ASSERT(out_info);

    json_incref(self->info);
    *out_info = self->info;

    return RC__STATUS_SUCCESS;
}

static void LOCKED_update_signal_path(AsioOutputPlugin *self) {
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
        if (self->info) {
            json_t *asio_device = json_object_get(self->info, "asio_device");
            if (asio_device) {
                json_object_set(output, "asio_device", asio_device);
            }
        }
        json_object_set_new(output, "type", json_string("output"));
        json_object_set_new(output, "method", json_string("asio"));
        json_object_set_new(output, "quality", json_string("lossless"));
        json_array_append_new(signal_path, output);
    }

    json_object_set_new(message, "signal_path", signal_path);
    RAAT__output_message_listeners_invoke(&self->message_listeners, message);
    json_decref(message);
}

#define COUNT(a) (sizeof(a) / sizeof *(a))

void IN_ASIO_THREAD_stop_and_dispose_buffers(AsioOutputPlugin *self) {
	self->asio->stop();
	self->asio->disposeBuffers();
}


static RC__Status output_get_supported_formats(void *vself, RC__Allocator *alloc, size_t *out_nformats, RAAT__StreamFormat/*?*/ **out_formats) {
    AsioOutputPlugin *self = (AsioOutputPlugin*)vself;
    if (self->get_supported_formats_status != RC__STATUS_SUCCESS) return self->get_supported_formats_status;
    RAAT__StreamFormat *formats      = RC__new0(alloc, RAAT__StreamFormat, self->n_supported_formats);
    if (formats == NULL) return RC__STATUS_OUT_OF_MEMORY;
    memcpy(formats, self->supported_formats, self->n_supported_formats * sizeof(RAAT__StreamFormat));

    size_t i;
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

static void IN_ASIO_THREAD_probe_formats(AsioOutputPlugin *self) {
    RC__Status status = RC__STATUS_SUCCESS;

    int pcmbaserate[]      = { 44100, 48000 };
    int pcmmult[]          = { 1, 2, 4, 8, 16 };
    int channels[]         = { 1, 2, 3, 4, 5, 6, 7, 8 };
    int pcmbitspersample[] = { 16, 24, 32 };
    int dsdmult[]          = { 64, 128, 256, 512, 1024 };

    int channel_i, pcmbaserate_i, pcmmult_i, pcmbitspersample_i;
    size_t nformats = 0;

    size_t              max_nformats = COUNT(channels) * (COUNT(pcmbaserate) * COUNT(pcmmult) * COUNT(pcmbitspersample) + COUNT(dsdmult));
    RAAT__StreamFormat *formats      = RC__new0(self->alloc, RAAT__StreamFormat, max_nformats);
    RC__ASSERT(formats);

    RAAT__TRACE("[asio] [%s] probing formats", self->driver_name);

	bool supports_native        = (self->dsd_mode == DSD_MODE_NATIVE ||
								   self->dsd_mode == DSD_MODE_NATIVE_OR_DCS ||
								   self->dsd_mode == DSD_MODE_NATIVE_OR_DOP);

    if (strstr(self->driver_name, "LUXMAN ASIO")) {
        RAAT__TRACE("[asio] [%s] assuming DSD support for LUXMAN driver", self->driver_name);
        self->assume_dsd_native_support = true;
    }

	if (supports_native) {
		RAAT__TRACE("[output/asio] [%s] Probing for Native DSD output support", self->driver_name);
		ASIOIoFormat format = { 0, };
		format.FormatType = kASIODSDFormat;
		if (self->asio->future(kAsioSetIoFormat, &format) == ASE_SUCCESS || self->assume_dsd_native_support) {
            self->supports_io_format = true;
			self->asio_format_type = kASIODSDFormat;
			RAAT__TRACE("[output/asio] [%s] Successfully set I/O Format to DSD (driver supports native DSD)", self->driver_name);

			// while we have the I/O format set, probe for format support
			for (channel_i = 0; channel_i < COUNT(channels); channel_i++) {
				int channel = channels[channel_i];
				int dsdmult_i;
				for (dsdmult_i = 0; dsdmult_i < COUNT(dsdmult); dsdmult_i++) {
					int mult = dsdmult[dsdmult_i];
					int sample_rate = mult*44100;
					if (self->asio->canSampleRate(sample_rate) == ASE_OK) {
						formats[nformats].sample_type     = RAAT__SAMPLE_TYPE_DSD;
						formats[nformats].sample_rate     = sample_rate;
						formats[nformats].bits_per_sample = 1;
						formats[nformats].channels        = channel;
						nformats++;
						RAAT__TRACE("[asio] [%s] supports DSD format %d/%d/%d (Native)", self->driver_name, 44100*mult, 1, channel);
					}
				}
			}

			ASIOIoFormat format = { 0, };
			format.FormatType = kASIOPCMFormat;
			if (self->asio->future(kAsioSetIoFormat, &format) == ASE_SUCCESS || self->assume_dsd_native_support) {
				self->asio_format_type = kASIOPCMFormat;
				RAAT__TRACE("[output/asio] [%s] Setting I/O Format back to PCM", self->driver_name);
			}
			else {
				RAAT__TRACE("[output/asio] [%s] device doesn't support setting IO format", self->driver_name);
			}
		}
		else {
			RAAT__TRACE("[output/asio] [%s] device doesn't support setting IO format", self->driver_name);
			self->asio_format_type = kASIOPCMFormat;		
		}
	}
	else {
		RAAT__TRACE("[output/asio] [%s] Not probing for Native DSD output support", self->driver_name);
	}

	int max_bits_per_sample = 32;

	for (int i = 0; i < self->asio_output_channels; i++) {
		ASIOChannelInfo ch_info;
		ch_info.channel = i;
		ch_info.isInput = false;
		int result = self->asio->getChannelInfo(&ch_info);
		if (result) {
			RAAT__ERROR("[output/asio] [%s] device won't let us get channel info for ch%d", self->driver_name, i);
			status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED; goto fail;
		}
		RAAT__TRACE("[output/asio] [%s] ch%d has sample type %d, name %s", self->driver_name, i, (int)ch_info.type, ch_info.name);
		switch (ch_info.type) {
			case ASIOSTInt16MSB:	max_bits_per_sample = RC__min(16, max_bits_per_sample); break;
			case ASIOSTInt16LSB:	max_bits_per_sample = RC__min(16, max_bits_per_sample); break;
			case ASIOSTInt24MSB:	max_bits_per_sample = RC__min(24, max_bits_per_sample); break;
			case ASIOSTInt24LSB:	max_bits_per_sample = RC__min(24, max_bits_per_sample); break;
			case ASIOSTInt32LSB16:	max_bits_per_sample = RC__min(16, max_bits_per_sample); break;
			case ASIOSTInt32LSB18:	max_bits_per_sample = RC__min(16, max_bits_per_sample); break;
			case ASIOSTInt32LSB20:	max_bits_per_sample = RC__min(16, max_bits_per_sample); break;
			case ASIOSTInt32LSB24:	max_bits_per_sample = RC__min(24, max_bits_per_sample); break;
			case ASIOSTInt32MSB:	max_bits_per_sample = RC__min(32, max_bits_per_sample); break;
			case ASIOSTFloat32MSB:	max_bits_per_sample = RC__min(32, max_bits_per_sample); break;
			case ASIOSTInt32LSB:	max_bits_per_sample = RC__min(32, max_bits_per_sample); break;
			case ASIOSTFloat32LSB:	max_bits_per_sample = RC__min(32, max_bits_per_sample); break;
			case ASIOSTInt32MSB16:	max_bits_per_sample = RC__min(16, max_bits_per_sample); break;
			case ASIOSTInt32MSB18:	max_bits_per_sample = RC__min(16, max_bits_per_sample); break;
			case ASIOSTInt32MSB20:	max_bits_per_sample = RC__min(16, max_bits_per_sample); break;
			case ASIOSTInt32MSB24:	max_bits_per_sample = RC__min(24, max_bits_per_sample); break;
			case ASIOSTFloat64LSB:	max_bits_per_sample = RC__min(32, max_bits_per_sample); break;
			case ASIOSTFloat64MSB:	max_bits_per_sample = RC__min(32, max_bits_per_sample); break;
			default:
				RAAT__ERROR("[output/asio] [%s] ch%d has unsupported PCM sample type %d", self->driver_name, i, (int)ch_info.type);
				status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED; goto fail;
		}
	}

	RAAT__TRACE("[output/asio] [%s] supports max bits per sample %d", self->driver_name, max_bits_per_sample);

    for (channel_i = 0; channel_i < COUNT(channels); channel_i++) {
        int channel       = channels[channel_i];

        bool supports_encapsulation = self->dsd_mode == DSD_MODE_DOP           || 
                                      self->dsd_mode == DSD_MODE_DCS           || 
                                      self->dsd_mode == DSD_MODE_NATIVE_OR_DCS || 
                                      self->dsd_mode == DSD_MODE_NATIVE_OR_DOP;

        // PCM + Encapsulated DSD
        for (pcmmult_i = 0; pcmmult_i < COUNT(pcmmult); pcmmult_i++)
        for (pcmbaserate_i = 0; pcmbaserate_i < COUNT(pcmbaserate); pcmbaserate_i++)
        for (pcmbitspersample_i = 0; pcmbitspersample_i < COUNT(pcmbitspersample); pcmbitspersample_i++) {
            int samplerate    = pcmbaserate[pcmbaserate_i] * pcmmult[pcmmult_i];
            int bitspersample = pcmbitspersample[pcmbitspersample_i];

            if (channel <= self->asio_output_channels && bitspersample <= max_bits_per_sample && self->asio->canSampleRate(samplerate) == ASE_OK) {        
                formats[nformats].sample_type     = RAAT__SAMPLE_TYPE_PCM;
                formats[nformats].sample_rate     = samplerate;
                formats[nformats].bits_per_sample = bitspersample;
                formats[nformats].channels        = channel;
                nformats++;

                RAAT__TRACE("[asio] [%s] supports PCM format %d/%d/%d", self->driver_name, samplerate, bitspersample, channel);

                int dsdmult_i;
                for (dsdmult_i = 0; dsdmult_i < COUNT(dsdmult); dsdmult_i++) {
                    int mult = dsdmult[dsdmult_i];
                    if (supports_encapsulation && self->max_dsd_rate >= mult && bitspersample == 24 && samplerate == 44100*mult/16) {
                        formats[nformats].sample_type     = RAAT__SAMPLE_TYPE_DSD;
                        formats[nformats].sample_rate     = 44100*mult;
                        formats[nformats].bits_per_sample = 1;
                        formats[nformats].channels        = channel;
                        if (!supports_format_already(formats[nformats], formats, nformats)) {
                            nformats++;
                            RAAT__TRACE("[asio] [%s] supports DSD format %d/%d/%d (Encapsulated)", self->driver_name, 44100*mult, 1, channel);
                        }
                    }
                }
            }
        }
    }

    self->supported_formats             = formats;
    self->n_supported_formats           = nformats;
    self->get_supported_formats_status  = status;

fail:
    if (status != RC__STATUS_SUCCESS) RC__free(self->alloc,formats);
    self->get_supported_formats_status  = status;
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

static void LOCKED__teardown_volume(AsioOutputPlugin *self) {
    if (self->volume) {
        self->volume->teardown(self->volume->userdata);
    }
}

static RC__Status output_force_teardown(void *vself, json_t *reason) {
	AsioOutputPlugin *self = (AsioOutputPlugin*)vself;

	RC__Status status = RC__STATUS_SUCCESS;
	int        token = RAAT__OUTPUT_TOKEN_INVALID;

	RAAT__OutputLostCallback  old_cb_lost = NULL;
	void                     *old_cb_lost_userdata = NULL;
	bool					  did_teardown = false;

	uv_mutex_lock(&self->lock);
retry:
	self->current_token = token = ++self->next_token;

	if (self->state != IDLE) {
		RAAT__TRACE("[asio] [%s] kicking off old output in force teardown", self->driver_name);
		self->state = IDLE;
		old_cb_lost = self->cb_lost;
		old_cb_lost_userdata = self->cb_lost_userdata;
		self->cb_lost = NULL;
		self->cb_lost_userdata = NULL;
		LOCKED__teardown_volume(self);
		did_teardown = true;
	}
	uv_mutex_unlock(&self->lock);

	if (did_teardown) {
		run_in_asio_thread_sync((asio_thread_proc)IN_ASIO_THREAD_stop_and_dispose_buffers, self);
	}

    if (old_cb_lost != NULL) {
        old_cb_lost(old_cb_lost_userdata, reason);
    }

    uv_mutex_lock(&self->lock);
    if (self->current_token != token) {
        RAAT__TRACE("[asio] [%s] force teardown required a retry", self->driver_name);
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

double flip_endian(double i) {
	unsigned long long u = *(unsigned long long*)&i;
	unsigned long long o = ((u << 56) & 0xff00000000000000) |
						   ((u << 48) & 0x00ff000000000000) |
						   ((u << 40) & 0x0000ff0000000000) |
						   ((u << 32) & 0x000000ff00000000) |
						   ((u >> 32) & 0x00000000ff000000) |
						   ((u >> 40) & 0x0000000000ff0000) |
						   ((u >> 48) & 0x000000000000ff00) |
						   ((u >> 56) & 0x00000000000000ff);
	return *(double*)&o;
}

float flip_endian(float i) {
	unsigned u = *(unsigned*)&i;
	unsigned o = ((u << 24) & 0xff000000) |
				 ((u <<  8) & 0x00ff0000) |
				 ((u >>  8) & 0x0000ff00) |
				 ((u >> 24) & 0x000000ff);
	return *(float*)&o;
}

int flip_endian(int i) {
	unsigned u = *(unsigned*)&i;
	unsigned o = ((u << 24) & 0xff000000) |
				 ((u <<  8) & 0x00ff0000) |
				 ((u >>  8) & 0x0000ff00) |
				 ((u >> 24) & 0x000000ff);
	return *(int*)&o;
}

static void fill_hw_buffer(AsioOutputPlugin *self, BYTE *output_buf, ASIOSampleType type, BYTE *input_buf, int offset, int stride, int samples) {
	 switch (type) {
		case ASIOSTInt16MSB:
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 2) {
				switch (self->procformat.bits_per_sample) {
					case 16:
						output_buf[1] = input_buf[0];
						output_buf[0] = input_buf[1];
						break;
					case 24: RC__ASSERT(0); break;
				}
			}
			break;

		case ASIOSTInt16LSB:
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 2) {
				switch (self->procformat.bits_per_sample) {
					case 16:
						output_buf[0] = input_buf[0];
						output_buf[1] = input_buf[1];
						break;
					case 24: RC__ASSERT(0); break;
				}
			}
			break;

		case ASIOSTInt24MSB:		// used for 20 bits as well
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 3) {
				switch (self->procformat.bits_per_sample) {
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
			}
			break;

		case ASIOSTInt24LSB:		// used for 20 bits as well
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 3) {
				switch (self->procformat.bits_per_sample) {
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
			}
			break;

		case ASIOSTInt32MSB:
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 4) {
				switch (self->procformat.bits_per_sample) {
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
						break;
					case 32:
						output_buf[3] = input_buf[0];
						output_buf[2] = input_buf[1];
						output_buf[1] = input_buf[2];
						output_buf[0] = input_buf[3];
						break;
				}
			}
			break;

		case ASIOSTInt32LSB: {
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 4) {
				switch (self->procformat.bits_per_sample) {
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
			}
		} break;

		case ASIOSTInt32LSB16:		// 32 bit data with 16 bit alignment
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 4) {
				int sample;
				switch (self->procformat.bits_per_sample) {
					case 16:
						sample = (int)input_buf[0] << 16 | ((int)input_buf[1] << 24);
						break;
					case 24:
						sample = (int)input_buf[0] << 8 | ((int)input_buf[1] << 16) | ((int)input_buf[2] << 8);
						break;
				}
				*((int*)output_buf) = (sample >> 16);
			}
			break;

		case ASIOSTInt32LSB18:		// 32 bit data with 18 bit alignment
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 4) {
				int sample;
				switch (self->procformat.bits_per_sample) {
					case 16:
						sample = (int)input_buf[0] << 16 | ((int)input_buf[1] << 24);
						break;
					case 24:
						sample = (int)input_buf[0] << 8 | ((int)input_buf[1] << 16) | ((int)input_buf[2] << 8);
						break;
				}
				*((int*)output_buf) = (sample >> 14);
			}
			break;

		case ASIOSTInt32LSB20:		// 32 bit data with 20 bit alignment
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 4) {
				int sample;
				switch (self->procformat.bits_per_sample) {
					case 16:
						sample = (int)input_buf[0] << 16 | ((int)input_buf[1] << 24);
						break;
					case 24:
						sample = (int)input_buf[0] << 8 | ((int)input_buf[1] << 16) | ((int)input_buf[2] << 8);
						break;
				}
				*((int*)output_buf) = (sample >> 12);
			}
			break;

		case ASIOSTInt32LSB24:		// 32 bit data with 24 bit alignment
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 4) {
				int sample;
				switch (self->procformat.bits_per_sample) {
					case 16:
						sample = (int)input_buf[0] << 16 | ((int)input_buf[1] << 24);
						break;
					case 24:
						sample = (int)input_buf[0] << 8 | ((int)input_buf[1] << 16) | ((int)input_buf[2] << 8);
						break;
				}
				*((int*)output_buf) = (sample >> 8);
			}
			break;

		case ASIOSTInt32MSB16:		// 32 bit data with 16 bit alignment
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 4) {
				int sample;
				switch (self->procformat.bits_per_sample) {
					case 16:
						sample = (int)input_buf[0] << 16 | ((int)input_buf[1] << 24);
						break;
					case 24:
						sample = (int)input_buf[0] << 8 | ((int)input_buf[1] << 16) | ((int)input_buf[2] << 8);
						break;
				}
				*((int*)output_buf) = flip_endian(sample >> 16);
			}
			break;

		case ASIOSTInt32MSB18:		// 32 bit data with 18 bit alignment
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 4) {
				int sample;
				switch (self->procformat.bits_per_sample) {
					case 16:
						sample = (int)input_buf[0] << 16 | ((int)input_buf[1] << 24);
						break;
					case 24:
						sample = (int)input_buf[0] << 8 | ((int)input_buf[1] << 16) | ((int)input_buf[2] << 8);
						break;
				}
				*((int*)output_buf) = flip_endian(sample >> 14);
			}
			break;

		case ASIOSTInt32MSB20:		// 32 bit data with 20 bit alignment
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 4) {
				int sample;
				switch (self->procformat.bits_per_sample) {
					case 16:
						sample = (int)input_buf[0] << 16 | ((int)input_buf[1] << 24);
						break;
					case 24:
						sample = (int)input_buf[0] << 8 | ((int)input_buf[1] << 16) | ((int)input_buf[2] << 8);
						break;
				}
				*((int*)output_buf) = flip_endian(sample >> 12);
			}
			break;

		case ASIOSTInt32MSB24:		// 32 bit data with 24 bit alignment
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 4) {
				int sample;
				switch (self->procformat.bits_per_sample) {
					case 16:
						sample = (int)input_buf[0] << 16 | ((int)input_buf[1] << 24);
						break;
					case 24:
						sample = (int)input_buf[0] << 8 | ((int)input_buf[1] << 16) | ((int)input_buf[2] << 8);
						break;
				}
				*((int*)output_buf) = flip_endian(sample >> 8);
			}
			break;

		case ASIOSTFloat32MSB:		// IEEE 754 32 bit float, as found on Intel x86 architecture
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 4) {
				int sample;
				switch (self->procformat.bits_per_sample) {
					case 16:
						sample = ((int)input_buf[0] << 16) | ((int)input_buf[1] << 24);
						break;
					case 24:
						sample = ((int)input_buf[0] << 8) | ((int)input_buf[1] << 16) | ((int)input_buf[2] << 24);
						break;
					case 32:
						sample = ((int)input_buf[0] << 0)  | ((int)input_buf[1] << 8) | ((int)input_buf[2] << 16) | ((int)input_buf[3] << 24);
						break;
				}
				*((float*)output_buf) = flip_endian((float)sample / (float)INT_MAX);
			}
			break;

		case ASIOSTFloat32LSB:		// IEEE 754 32 bit float, as found on Intel x86 architecture
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 4) {
				int sample;
				switch (self->procformat.bits_per_sample) {
					case 16:
						sample = (int)input_buf[0] << 16 | ((int)input_buf[1] << 24);
						break;
					case 24:
						sample = (int)input_buf[0] << 8 | ((int)input_buf[1] << 16) | ((int)input_buf[2] << 24);
						break;
					case 32:
						sample = ((int)input_buf[0] << 0)  | ((int)input_buf[1] << 8) | ((int)input_buf[2] << 16) | ((int)input_buf[3] << 24);
						break;
				}
				*((float*)output_buf) = (float)sample / (float)INT_MAX;
			}
			break;

		case ASIOSTFloat64LSB: 		// IEEE 754 64 bit double float, as found on Intel x86 architecture
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 8) {
				int sample;
				switch (self->procformat.bits_per_sample) {
					case 16:
						sample = (int)input_buf[0] << 16 | ((int)input_buf[1] << 24);
						break;
					case 24:
						sample = (int)input_buf[0] << 8 | ((int)input_buf[1] << 16) | ((int)input_buf[2] << 24);
						break;
					case 32:
						sample = ((int)input_buf[0] << 0)  | ((int)input_buf[1] << 8) | ((int)input_buf[2] << 16) | ((int)input_buf[3] << 24);
						break;
				}
				*((double*)output_buf) = (double)sample / (double)INT_MAX;
			}
			break;

		case ASIOSTFloat64MSB: 		// IEEE 754 64 bit double float, as found on Intel x86 architecture
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 8) {
				int sample;
				switch (self->procformat.bits_per_sample) {
					case 16:
						sample = (int)input_buf[0] << 16 | ((int)input_buf[1] << 24);
						break;
					case 24:
						sample = (int)input_buf[0] << 8 | ((int)input_buf[1] << 16) | ((int)input_buf[2] << 24);
						break;
					case 32:
						sample = ((int)input_buf[0] << 0)  | ((int)input_buf[1] << 8) | ((int)input_buf[2] << 16) | ((int)input_buf[3] << 24);
						break;
				}
				*((double*)output_buf) = flip_endian((double)sample / (double)INT_MAX);
			}
			break;

		case ASIOSTDSDInt8LSB1:             // DSD 1 bit data, 8 samples per byte. First sample in Least significant bit
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 1) {
				*output_buf = reverse(*input_buf);
			}
			break;

		case ASIOSTDSDInt8MSB1:             // DSD 1 bit data, 8 samples per byte. First sample in Most significant bit
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 1) {
				*output_buf = *input_buf;
			}
			break;

		case ASIOSTDSDInt8NER8:             // DSD 8 bit data, 1 sample per byte. No Endianness required
			for (input_buf = input_buf + offset; samples; samples--, input_buf += stride, output_buf += 8) {
				BYTE i = *input_buf;
				output_buf[0] = (i >> 7) & 0x1;
				output_buf[1] = (i >> 6) & 0x1;
				output_buf[2] = (i >> 5) & 0x1;
				output_buf[3] = (i >> 4) & 0x1;
				output_buf[4] = (i >> 3) & 0x1;
				output_buf[5] = (i >> 2) & 0x1;
				output_buf[6] = (i >> 1) & 0x1;
				output_buf[7] = (i >> 0) & 0x1;
			}
			break;

		default: break;
	 }
}


static ASIOTime *bufferSwitchTimeInfo(void *userdata, ASIOTime *timeInfo, long index, ASIOBool processNow) {
	AsioOutputPlugin   *self		       = (AsioOutputPlugin*)userdata;
    RAAT__Stream       *stream = NULL;
    RAAT__StreamFormat  origformat;
    RAAT__StreamFormat  procformat;
	bool			    call_setup_cb = false;

    if (!self->in_start) uv_mutex_lock(&self->lock);

	procformat = self->procformat;
	origformat = self->origformat;

	int64_t now_raw_ns = RC__now_ns();

    int origformat_samples_to_zerofill = 0;

    int64_t delay_ns = RAAT__stream_format_samples_to_ns(&procformat, self->asio_output_latency);

    self->last_delay_ns                         = delay_ns;
    self->last_monotonic_sample_systime         =  now_raw_ns + delay_ns;
    self->last_monotonic_sample                 += self->origformat_samples_per_buf;

    int64_t now = RAAT__stream_format_samples_to_ns(&origformat, self->last_monotonic_sample);

    int correction_samples = 0; 

    if (self->new_stream && self->stream) {
        int64_t start_time = self->start_time;
        if (now + self->ns_per_buf > start_time) {
            RAAT__TRACE("[output/asio] [%s] starting playback: now (%lldns) + ns_per_buf(%lldns) = %lldns > %lldns streamsample=%lld", 
						self->driver_name, now, self->ns_per_buf, now + self->ns_per_buf, start_time, self->start_streamsample);
            self->new_stream    = false;
            stream              = self->stream;
            self->streamsample  = self->start_streamsample;

            // streamsample must fall on an 8-bit boundary or bad things will happen
            if (origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) 
                self->streamsample -= self->streamsample % 8;

            if (now < start_time) {
                int ns_to_fill      = start_time - now;
                origformat_samples_to_zerofill = RAAT__stream_format_ns_to_samples(&origformat, ns_to_fill);
            } if (now > start_time) {
                self->streamsample += RAAT__stream_format_ns_to_samples(&origformat, now - start_time);
            }
        } else {
            RAAT__TRACE("[output/asio] [%s] waiting for start time...", self->driver_name);
        }
    } else {
        stream              = self->stream;
        origformat_samples_to_zerofill = 0;
    }

    correction_samples = RAAT__drift_correction_compute_correction(&self->drift_correction, self->origformat_samples_per_buf);

    if (stream != NULL) RAAT__stream_incref(stream);

    if (!self->in_start) uv_mutex_unlock(&self->lock);

    if (stream == NULL || self->resync_delay_remaining_ns > 0) {
        origformat_samples_to_zerofill = self->origformat_samples_per_buf;
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
        //if (correction_samples != 0) RAAT__TRACE("[alsa] applying %d sample correction", correction_samples);
        RAAT__read_stream_with_drift_correction(stream, self->streamsample, self->origbuf + bytes_to_zerofill, self->origformat_samples_per_buf - origformat_samples_to_zerofill, correction_samples, NULL, NULL);
        self->streamsample += self->origformat_samples_per_buf - origformat_samples_to_zerofill + correction_samples;
    }

    if (self->resync_delay_remaining_ns > 0) {
        self->resync_delay_remaining_ns -= self->ns_per_buf;
        if (self->resync_delay_remaining_ns <= 0) {
			call_setup_cb = true;
        }
    }

    uint8_t *buf = self->origbuf;

    if (origformat.sample_type == RAAT__SAMPLE_TYPE_PCM && procformat.bits_per_sample > origformat.bits_per_sample) {
        // repack PCM into wider PCM. This case is most relevant when performing digital volume adjustments. We may have
        // opened the audio device at a wider bits-per-sample than the stream, so that we can preserve as much data as possible after
        // volume attenuation
        RAAT__stream_format_repack(&origformat, buf, &procformat, self->procbuf, self->origformat_samples_per_buf);
        buf = self->procbuf;
    }

    if (self->volume) {
        self->volume->process(self->volume->userdata, 0, buf, self->origformat_samples_per_buf);
    }

    // DSD must be repacked
	if (origformat.sample_type == RAAT__SAMPLE_TYPE_DSD) {
		if (self->effective_dsd_mode == DSD_MODE_DOP) {
			RAAT__pack_dop_samples(&origformat, buf, self->dsdbuf, self->origformat_samples_per_buf, &self->dsd_dop_flipper);
			buf = self->dsdbuf;
		}
		else if (self->effective_dsd_mode == DSD_MODE_DCS) {
			RAAT__pack_dcs_samples(&origformat, buf, self->dsdbuf, self->origformat_samples_per_buf);
			buf = self->dsdbuf;
		}
	}

	for (int ch = 0; ch < self->origformat.channels; ch++) {
		void *asiobuf = (void*)self->asio_buffer_infos[ch].buffers[index];
		ASIOSampleType type = self->asio_channel_infos[ch].type;

		if (self->procformat.sample_type == RAAT__SAMPLE_TYPE_PCM) {
			int bytes_per_sample = self->procformat.bits_per_sample / 8;
			int bytes_per_frame = self->procformat.bits_per_sample * self->procformat.channels / 8;
			fill_hw_buffer(self, (BYTE*)asiobuf, type, (BYTE*)buf, bytes_per_sample*ch, bytes_per_frame, self->samples_per_buf);
		} else if (self->procformat.sample_type == RAAT__SAMPLE_TYPE_DSD) {
			fill_hw_buffer(self, (BYTE*)asiobuf, type, (BYTE*)buf, ch, self->procformat.channels, self->samples_per_buf / 8);
		}
		else {
			RC__ASSERT(0);
		}
	}

    if (stream != NULL) RAAT__stream_decref(stream);

	// if the driver supports the ASIOOutputReady() optimization, do it here, all data are in place
	if (self->asio_post_output) self->asio->outputReady();

	if (call_setup_cb) {
		RAAT__OutputSetupCallback setup_cb = NULL;
		void* setup_cb_userdata		       = NULL;

		uv_mutex_lock(&self->lock);
		setup_cb = self->setup_cb;
		setup_cb_userdata = self->setup_cb_userdata;
		self->setup_cb = NULL;
		uv_mutex_unlock(&self->lock);
		if (setup_cb) {
			setup_cb(setup_cb_userdata, RC__STATUS_SUCCESS, self->current_token);
		}
	}

	return NULL;

#if 0
	// store the timeInfo for later use
	self->asio_t_info = *timeInfo;

	// get the time stamp of the buffer, not necessary if no
	// synchronization to other media is required
	if (timeInfo->timeInfo.flags & kSystemTimeValid)
		self->asio_nano_seconds = ASIO64toDouble(timeInfo->timeInfo.systemTime);
	else
		self->asio_nano_seconds = 0;

	if (timeInfo->timeInfo.flags & kSamplePositionValid)
		self->asio_samples = ASIO64toDouble(timeInfo->timeInfo.samplePosition);
	else
		self->asio_samples = 0;

	if (timeInfo->timeCode.flags & kTcValid)
		self->asio_tc_samples = ASIO64toDouble(timeInfo->timeCode.timeCodeSamples);
	else
		self->asio_tc_samples = 0;

	// get the system reference time
	self->asio_sys_ref_time = timeGetTime();
#endif
}

static void asio_reset_thread(void *vself) {
    AsioOutputPlugin *self = (AsioOutputPlugin*)vself;
	json_t *reason = json_object();
	json_object_set_new(reason, "reason", json_string("device_reset"));
	output_force_teardown((RAAT__OutputPlugin*)self, reason);
	json_decref(reason);
    Sleep(200);
    if (self->device_lost_cb != NULL) self->device_lost_cb((RAAT__OutputPlugin*)self, self->device_lost_userdata);
}

static long asioMessage(void *userdata, long selector, long value, void* message, double* opt) {
    AsioOutputPlugin *self = (AsioOutputPlugin*)userdata;

    RAAT__TRACE("[output/asio] [%s] asioMessage %d %d", self->driver_name, selector, value);
    // currently the parameters "value", "message" and "opt" are not used.
    long ret = 0;
    switch(selector)
    {
    case kAsioSelectorSupported:
	    if(value == kAsioResetRequest
		    || value == kAsioEngineVersion
		    || value == kAsioResyncRequest
		    || value == kAsioLatenciesChanged
		    // the following three were added for ASIO 2.0, you don't necessarily have to support them
		    || value == kAsioSupportsTimeInfo
		    || value == kAsioSupportsTimeCode
		    || value == kAsioSupportsInputMonitor)
		    ret = 1L;
	    break;
    case kAsioResetRequest:
	    RAAT__TRACE("[output/asio] driver reset");
	    // defer the task and perform the reset of the driver during the next "safe" situation
	    // You cannot reset the driver right now, as this code is called from the driver.
	    // Reset the driver is done by completely destruct is. I.e. ASIOStop(), ASIODisposeBuffers(), Destruction
	    // Afterwards you initialize the driver again.
	    
        self->needs_reinit = true;
	    if (self->block_reset) {
			self->did_reset_during_block = true;
	    } else {
            int64_t now = RC__now_ns();
            if (now - self->last_start > 500000000L) {
                CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)asio_reset_thread, (void*)self, 0, NULL);
            } else {
                RAAT__TRACE("[output/asio] ignoring since we started in last 0.5 seconds");
            }
	    }

	    ret = 1L;
	    break;
    case kAsioResyncRequest:
	    // This informs the application, that the driver encountered some non fatal data loss.
	    // It is used for synchronization purposes of different media.
	    // Added mainly to work around the Win16Mutex problems in Windows 95/98 with the
	    // Windows Multimedia system, which could loose data because the Mutex was hold too long
	    // by another thread.
	    // However a driver can issue it in other situations, too.
	    ret = 1L;
	    break;
    case kAsioLatenciesChanged:
	    // This will inform the host application that the drivers were latencies changed.
	    // Beware, it this does not mean that the buffer sizes have changed!
	    // You might need to update internal delay data.
	    ret = 1L;
	    break;
    case kAsioEngineVersion:
	    // return the supported ASIO version of the host application
	    // If a host applications does not implement this selector, ASIO 1.0 is assumed
	    // by the driver
	    ret = 2L;
	    break;
    case kAsioSupportsTimeInfo:
	    // informs the driver wether the asioCallbacks.bufferSwitchTimeInfo() callback
	    // is supported.
	    // For compatibility with ASIO 1.0 drivers the host application should always support
	    // the "old" bufferSwitch method, too.
	    ret = 1;
	    break;
    case kAsioSupportsTimeCode:
	    // informs the driver wether application is interested in time code info.
	    // If an application does not need to know about time code, the driver has less work
	    // to do.
	    ret = 0;
	    break;
    }
    return ret;
}

static void sampleRateDidChange(void *userdata, ASIOSampleRate sRate) { }

static void bufferSwitch(void *userdata, long index, ASIOBool processNow) {
    AsioOutputPlugin *self = (AsioOutputPlugin*)userdata;

    // as this is a "back door" into the bufferSwitchTimeInfo a timeInfo needs to be created
    // though it will only set the timeInfo.samplePosition and timeInfo.systemTime fields and the according flags
    ASIOTime  timeInfo;
    memset(&timeInfo, 0, sizeof(timeInfo));

    // get the time stamp of the buffer, not necessary if no
    // synchronization to other media is required
    if (self->asio->getSamplePosition(&timeInfo.timeInfo.samplePosition, &timeInfo.timeInfo.systemTime) == ASE_OK)
	    timeInfo.timeInfo.flags = kSystemTimeValid | kSamplePositionValid;

    bufferSwitchTimeInfo(userdata, &timeInfo, index, processNow);
}

ASIOError IN_ASIO_THREAD_reinit(AsioOutputPlugin *self, bool fast) {
	ASIOError rc;

	RAAT__TRACE("[output/asio] [%s] (reinit) closing driver", self->driver_name);
	self->drivers->asioCloseDriver(self->drvindex);

	if (!fast) {
		RAAT__TRACE("[output/asio] [%s] (reinit) waiting", self->driver_name);
		Sleep(2000);
	}

	self->asio = NULL;

	RAAT__TRACE("[output/asio] [%s] (reinit) opening driver", self->driver_name);
	rc = self->drivers->asioOpenDriver(self->drvindex, (void**)&self->asio);
	if (rc != 0) {
		RAAT__TRACE("[output/asio] [%s] (reinit) asio driver open failed: %d (0x%x)", self->driver_name, rc, rc);
		return rc;
	}

	RAAT__TRACE("[output/asio] [%s] (reinit) initializing driver", self->driver_name);
	if (!self->asio->init(NULL)) {
		RAAT__TRACE("[output/asio] [%s] (reinit) init failed", self->driver_name);
		return -2;
	}

	rc = self->asio->getChannels(&self->asio_input_channels, &self->asio_output_channels);
	if (rc == ASE_OK) {
		RAAT__TRACE("[output/asio] [%s] (reinit) getChannels (inputs: %d, outputs: %d);", self->driver_name, self->asio_input_channels, self->asio_output_channels);
		if (self->asio_output_channels < 2) {
			RAAT__TRACE("[output/asio] [%s] (reinit) invalid channel count %d", self->driver_name, self->asio_output_channels);
			return -9;
		}

		rc = self->asio->getBufferSize(&self->asio_min_size, &self->asio_max_size, &self->asio_preferred_size, &self->asio_granularity);
		if (rc == ASE_OK) {
			RAAT__TRACE("[output/asio] [%s] getBufferSize (min: %d, max: %d, preferred: %d, granularity: %d);", self->driver_name, self->asio_min_size, self->asio_max_size, self->asio_preferred_size, self->asio_granularity);
		}
		else {
			RAAT__TRACE("[output/asio] [%s] (reinit) getBufferSize failed: rc=%d (0x%x)", self->driver_name, rc, rc);
			return -8;
		}

		if (self->asio->outputReady() == ASE_OK) {
			self->asio_post_output = true;
		} else {
			self->asio_post_output = false;
		}

        self->needs_reinit = false;
		RAAT__TRACE("[output/asio] [%s] (reinit) asio output is all good to go!", self->driver_name);
		return ASE_OK;
	}
	else {
		RAAT__TRACE("[output/asio] [%s] (reinit) getChannels failed: rc=%d (0x%x)", self->driver_name, rc, rc);
		return -10;
	}
}

typedef struct {
	AsioOutputPlugin *self;
	RC__Status status;
	RAAT__StreamFormat *format;
	RAAT__StreamFormat *procformat;
} setup_state;

void IN_ASIO_THREAD_output_setup(setup_state *state) {
	AsioOutputPlugin *self;
	RC__Status status;
	ASIOError result;

	self = state->self;
	status = state->status;

        if (self->needs_reinit) {
            result = IN_ASIO_THREAD_reinit(self, true);
            if (result != ASE_OK) {
                status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail;
            }
        }

	ASIOBufferInfo *info = self->asio_buffer_infos;

	bool supports_native = (self->dsd_mode == DSD_MODE_NATIVE || self->dsd_mode == DSD_MODE_NATIVE_OR_DCS || self->dsd_mode == DSD_MODE_NATIVE_OR_DOP);

	ASIOIoFormatType required_format_type = state->procformat->sample_type == RAAT__SAMPLE_TYPE_DSD && supports_native ? kASIODSDFormat : kASIOPCMFormat;
	if (required_format_type == kASIODSDFormat)
		RAAT__TRACE("using native dsd");
	else
		RAAT__TRACE("using pcm");
	if (required_format_type != self->asio_format_type && self->supports_io_format) {
		ASIOIoFormat format = { 0, };
		format.FormatType = required_format_type;
		RAAT__TRACE("[output/asio] [%s] setting format to %s", self->driver_name, (state->procformat->sample_type == RAAT__SAMPLE_TYPE_DSD == 1 ? "DSD" : "PCM"));
		result = self->asio->future(kAsioSetIoFormat, &format);
		if (result == ASE_SUCCESS || self->assume_dsd_native_support) {
			RAAT__TRACE("[output/asio] [%s] set format to %s successfully", self->driver_name, (state->procformat->sample_type == RAAT__SAMPLE_TYPE_DSD ? "DSD" : "PCM"));
			self->asio_format_type = required_format_type;
		} else {
			self->asio_format_type = kASIOPCMFormat;
        }
	}

	if (state->procformat->sample_type == RAAT__SAMPLE_TYPE_DSD) {
		if (self->asio_format_type == kASIOPCMFormat) {
			state->procformat->sample_type     = RAAT__SAMPLE_TYPE_PCM;
			state->procformat->sample_rate     = state->format->sample_rate / 16;
			state->procformat->bits_per_sample = 24;
			state->procformat->channels        = state->format->channels;

			switch (self->dsd_mode) {
				case DSD_MODE_DOP:		     self->effective_dsd_mode = DSD_MODE_DOP; break;
				case DSD_MODE_DCS:		     self->effective_dsd_mode = DSD_MODE_DCS; break;
				case DSD_MODE_NATIVE_OR_DOP: self->effective_dsd_mode = DSD_MODE_DOP; break;
				case DSD_MODE_NATIVE_OR_DCS: self->effective_dsd_mode = DSD_MODE_DCS; break;
				default:                     status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED; goto fail;
			}
		} else {
			self->effective_dsd_mode = DSD_MODE_NATIVE;
		}
	} else {
			self->effective_dsd_mode = DSD_MODE_NONE;
	}

	bool is_retry1 = false;
	bool is_retry2 = false;
retry:
        self->last_start = RC__now_ns();
	RAAT__TRACE("[output/asio] [%s] asio setting sample rate to %d", self->driver_name, state->procformat->sample_rate);
	result = self->asio->setSampleRate((double)state->procformat->sample_rate);
	if (result != ASE_OK) {
            if (is_retry1) {
                RAAT__TRACE("[output/asio] [%s] setting sample rate to %d failed: %d (0x%x)", self->driver_name, state->procformat->sample_rate, result, result);
                if (result == ASE_NotPresent || result == ASE_HWMalfunction) {
                    if (self->device_lost_cb != NULL) self->device_lost_cb((RAAT__OutputPlugin*)self, self->device_lost_userdata);
                }
		status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED; 
                goto fail;
            } else {
                result = IN_ASIO_THREAD_reinit(self, true);             
                if (result != ASE_OK) { 
                    if (self->device_lost_cb != NULL) self->device_lost_cb((RAAT__OutputPlugin*)self, self->device_lost_userdata);
                    status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED; 
                    goto fail; 
                }
                is_retry1 = true;
                goto retry;
            }
	}

	if (self->asio->getBufferSize(&self->asio_min_size, &self->asio_max_size, &self->asio_preferred_size, &self->asio_granularity) == ASE_OK) {
		RAAT__TRACE("[output/asio] [%s] getBufferSize (min: %d, max: %d, preferred: %d, granularity: %d);", self->driver_name, self->asio_min_size, self->asio_max_size, self->asio_preferred_size, self->asio_granularity);
	} else {
		RAAT__TRACE("failed to get buffer sizes");
		status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED; goto fail;
	}

	self->samples_per_buf			  = self->use_max_buffer_size ? self->asio_max_size : self->asio_preferred_size;
        if (self->power_of_two_buffer_size) {
            int samples_per_buf_pow2 = 1;
            while (samples_per_buf_pow2 * 2 < self->samples_per_buf) { samples_per_buf_pow2 *= 2; }
            if (samples_per_buf_pow2 >= self->asio_min_size && samples_per_buf_pow2 <= self->asio_max_size) {
                RAAT__DEBUG("[output/asio] Constraining buffer size to power of 2: Best %d -> %d", self->samples_per_buf, samples_per_buf_pow2);
                self->samples_per_buf = samples_per_buf_pow2;
            }
        }

	if (self->effective_dsd_mode == DSD_MODE_DOP || self->effective_dsd_mode == DSD_MODE_DCS)
		self->origformat_samples_per_buf = self->samples_per_buf * 16;
	else
		self->origformat_samples_per_buf = self->samples_per_buf;
	self->ns_per_buf = RAAT__stream_format_samples_to_ns(state->format, self->origformat_samples_per_buf);

	// prepare outputs
	if (self->asio_output_channels > kMaxOutputChannels)
		self->asio_output_buffers = kMaxOutputChannels;
	else
		self->asio_output_buffers = self->asio_output_channels;

	int i;
	for (i = 0; i < self->asio_output_buffers; i++, info++) {
		info->isInput = ASIOFalse;
		info->channelNum = i;
		info->buffers[0] = info->buffers[1] = 0;
	}

	RAAT__TRACE("[output/asio] [%s] using buffer size %d samples (origformat buf %d samples)", self->driver_name, self->samples_per_buf, self->origformat_samples_per_buf);

	// create and activate buffers
	result = self->asio->createBuffers(self->asio_buffer_infos, self->asio_output_buffers, self->samples_per_buf, &self->thunk->asioCallbacks);
	if (result == ASE_OK) {
		for (i = 0; i < self->asio_output_buffers; i++) {
			self->asio_channel_infos[i].channel = self->asio_buffer_infos[i].channelNum;
			self->asio_channel_infos[i].isInput = self->asio_buffer_infos[i].isInput;
			result = self->asio->getChannelInfo(&self->asio_channel_infos[i]);
			if (result != ASE_OK)
				break;
			RAAT__TRACE("[output/asio] [%s] got channel %d sampletype=%d", self->driver_name, i, self->asio_channel_infos[i].type);
		}

		if (result == ASE_OK) {
			result = self->asio->getLatencies(&self->asio_input_latency, &self->asio_output_latency);
			if (result == ASE_OK) {
				RAAT__TRACE("ASIOGetLatencies (input: %d, output: %d);", self->asio_input_latency, self->asio_output_latency);
			} else {
				RAAT__TRACE("ASIOGetLatencies failed: %d", result);
			}
		}
	}

	self->bytes_per_origbuf  = RAAT__stream_format_compute_buffer_size(state->format, self->origformat_samples_per_buf);
	self->bytes_per_procbuf  = RAAT__stream_format_compute_buffer_size(state->procformat, self->samples_per_buf);

	if (self->procbuf != NULL) {
		RC__free(self->alloc, self->procbuf);
		self->procbuf = NULL;
	}
	if (self->origbuf != NULL) {
		RC__free(self->alloc, self->origbuf);
		self->origbuf = NULL;
	}
	if (self->dsdbuf != NULL) {
		RC__free(self->alloc, self->dsdbuf);
		self->dsdbuf = NULL;
	}
	self->origbuf = (uint8_t*)RC__alloc(RC__ALLOCATOR_DEFAULT, self->bytes_per_origbuf);
	self->procbuf = (uint8_t*)RC__alloc(RC__ALLOCATOR_DEFAULT, self->bytes_per_procbuf);

	if (self->effective_dsd_mode == DSD_MODE_DOP || self->effective_dsd_mode == DSD_MODE_DCS) {
		// Both DoP and Dcs packing requires 3 bytes of 24bit PCM data for every 2 bytes of DSD data
		self->dsdbuf = (uint8_t*)RC__alloc(RC__ALLOCATOR_DEFAULT, self->bytes_per_origbuf * 3 / 2);
	}

	self->block_reset = true;
	self->did_reset_during_block = false;
    RAAT__TRACE("[output/asio] [%s] calling start()", self->driver_name);

    // set up the state now. We might still fail, but this needs to be good before start() because bufferSwitchTimeInfo() can happen immediately at this point.
    self->state      = STOPPED;
    self->origformat = *state->format;
    self->procformat = *state->procformat;

    self->in_start = true;      // used to prevent a deadlock in bufferSwitchTimeInfo if it is called during start()
	result = self->asio->start();
    self->in_start = false;
	if (result != ASE_OK) {
		RAAT__TRACE("[output/asio] [%s] start failed: %d", self->driver_name, result);
		if (is_retry2) {
			status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED; goto fail;
		}
		result = IN_ASIO_THREAD_reinit(self, false);
		if (result != ASE_OK) {
			status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail;
		}
		is_retry2 = true;
		goto retry;
	}

    Sleep(50);
	self->block_reset = false;

	if (self->did_reset_during_block) {
		RAAT__TRACE("[output/asio] [%s] reset during block", self->driver_name);
		result = IN_ASIO_THREAD_reinit(self, true);
		if (result != ASE_OK) {
			status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail;
		}
		is_retry2 = true;
		goto retry;
	}


fail:
    state->status = status;
}

static void output_setup(void *vself, RAAT__StreamFormat *format, RAAT__OutputSetupCallback cb_setup, void *cb_setup_userdata, RAAT__OutputLostCallback cb_lost, void *cb_lost_userdata) {
    AsioOutputPlugin *self = (AsioOutputPlugin*)vself;
    RC__Status status = RC__STATUS_SUCCESS;
    int        token = RAAT__OUTPUT_TOKEN_INVALID;

    RAAT__OutputLostCallback  old_cb_lost          = NULL;
    void                     *old_cb_lost_userdata = NULL;

    char formatstr[RAAT__STREAM_FORMAT_MAX_STRLEN];
    RAAT__stream_format_to_string(format, formatstr);

	RAAT__OutputSetupCallback old_setup_cb = NULL;
	void* old_setup_cb_userdata		       = NULL;
	int old_token;

    uv_mutex_lock(&self->lock);
	old_token			   = self->current_token;

    // before we do anything else, kick off anyone currently using the device
    self->current_token = token = ++self->next_token;

	bool did_teardown = false;

    if (self->state != IDLE) {
        RAAT__TRACE("[asio] [%s] kicking off old output", self->driver_name);
        self->state            = IDLE;
        old_cb_lost            = self->cb_lost;
        old_cb_lost_userdata   = self->cb_lost_userdata;
        self->cb_lost          = NULL;
        self->cb_lost_userdata = NULL;
		old_setup_cb	   	   = self->setup_cb;
		old_setup_cb_userdata  = self->setup_cb_userdata;
		self->setup_cb         = NULL;

		did_teardown = true;
        LOCKED__teardown_volume(self);
    }
    uv_mutex_unlock(&self->lock);

	if (old_setup_cb) {
		old_setup_cb(old_setup_cb_userdata, RC__STATUS_CANCELED, old_token);
	} else if (old_cb_lost != NULL) {       // don't call lost cb if we are calling setup cb too. It will trash memory
        json_t *reason = json_object();
        json_object_set_new(reason, "reason", json_string("setup"));
        old_cb_lost(old_cb_lost_userdata, reason);
        json_decref(reason);
    }

	if (did_teardown) {
		run_in_asio_thread_sync((asio_thread_proc)IN_ASIO_THREAD_stop_and_dispose_buffers, self);
	}

    uv_mutex_lock(&self->lock);

    if (self->current_token != token) {
        RAAT__TRACE("asio output setup: lost the race");
        status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED;
        goto fail;
    }

    // clean up leftover stream 
    if (self->stream != NULL) {
        RAAT__stream_decref(self->stream);
        self->stream = NULL;
    }

	RAAT__TRACE("[output/asio] [%s] asio output setup: format is %s", self->driver_name, formatstr);

	RAAT__StreamFormat *formats;
	size_t              nformats;
	RAAT__StreamFormat  procformat;
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
					if (RAAT__stream_format_equals(&format32, &formats[i])) { found = true; procformat = format32; break; }
				}
				break;
			case 16:
				for (i = 0; !found && i < nformats; i++) {
					if (RAAT__stream_format_equals(&format32, &formats[i])) { found = true; procformat = format32; break; }
				}
				for (i = 0; !found && i < nformats; i++) {
					if (RAAT__stream_format_equals(&format24, &formats[i])) { found = true; procformat = format24; break; }
				}
				break;
			}
		}

		for (i = 0; !found && i < nformats; i++) {
			if (RAAT__stream_format_equals(format, &formats[i])) { found = true; procformat = *format; break; }
		}

		RC__free(RC__ALLOCATOR_DEFAULT, formats);


		RAAT__TRACE("[output/asio] [%s] using procformat %d/%d/%d", self->driver_name, procformat.sample_rate, procformat.bits_per_sample, procformat.channels);

		if (found) {
			RAAT__TRACE("[output/asio] [%s] starting asio output", self->driver_name);

			setup_state state;
			state.self       = self;
			state.procformat = &procformat;
			state.format     = format;
			state.status     = status;

            self->current_token = token = ++self->next_token;
			run_in_asio_thread_sync((asio_thread_proc)IN_ASIO_THREAD_output_setup, &state);
			status = state.status;

			if (status != RC__STATUS_SUCCESS) {
				self->state                 = IDLE;
				self->current_token = ++self->next_token;
			}

            RAAT__drift_correction_init(&self->drift_correction, self->log, format);

            self->cb_lost               = cb_lost;
            self->cb_lost_userdata      = cb_lost_userdata;


            LOCKED_update_signal_path(self);
        } else {
            status = RAAT__OUTPUT_PLUGIN_STATUS_FORMAT_NOT_SUPPORTED;
        }
    }

fail:
    uv_mutex_unlock(&self->lock);

    // mutexes might not be recursive, so the following happen outside of the lock.
    if (self->volume && status == RC__STATUS_SUCCESS) {
        if (format->sample_type == RAAT__SAMPLE_TYPE_DSD) {
            self->volume->setup(self->volume->userdata, format,     &self->volume_delay);
        } else {
            self->volume->setup(self->volume->userdata, &procformat, &self->volume_delay);
        }
    }

    if (status == RC__STATUS_SUCCESS) {
        RAAT__TRACE("[output/asio] [%s] asio output started", self->driver_name);
        self->resync_delay_remaining_ns = self->resync_delay_secs * 1000000000LL;
        self->setup_cb          = cb_setup;
        self->setup_cb_userdata = cb_setup_userdata;
    } else {
        RAAT__TRACE("[output/asio] [%s] asio output failed: %s", self->driver_name, RC__status_to_string(status));
        // call our setup callback
        cb_setup(cb_setup_userdata, status, token);
    }
}

static RC__Status output_teardown(void *vself, int token) {
    AsioOutputPlugin *self = (AsioOutputPlugin*)vself;
    RC__Status status = RC__STATUS_UNEXPECTED_ERROR;
	bool did_teardown = false;

    uv_mutex_lock(&self->lock);
    if (self->current_token == token) {
        RAAT__TRACE("[asio] teardown");
        self->current_token = RAAT__OUTPUT_TOKEN_INVALID;
        self->state = IDLE;
        self->cb_lost = NULL;
        self->cb_lost_userdata = NULL;
        LOCKED__teardown_volume(self);

		did_teardown = true;

        if (self->stream != NULL) {
            RAAT__stream_decref(self->stream);
            self->stream = NULL;
        }

        status = RC__STATUS_SUCCESS;
    } else {
        status = RAAT__OUTPUT_PLUGIN_STATUS_INVALID_TOKEN;
    }
    uv_mutex_unlock(&self->lock);

	if (did_teardown) {
		run_in_asio_thread_sync((asio_thread_proc)IN_ASIO_THREAD_stop_and_dispose_buffers, self);
	}

    return status;
}

static RC__Status output_stop(void *vself, int token) {
    AsioOutputPlugin *self = (AsioOutputPlugin*)vself;
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
    AsioOutputPlugin *self = (AsioOutputPlugin*)vself;
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
    AsioOutputPlugin *self = (AsioOutputPlugin*)vself;
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

static int64_t LOCKED_get_local_time(AsioOutputPlugin *self) {
    int64_t now = RC__now_ns();
    int64_t last_wall_sample = self->last_monotonic_sample;
    return RAAT__stream_format_samples_to_ns(&self->origformat, last_wall_sample) + (now - self->last_monotonic_sample_systime);
}

static RC__Status output_get_local_time(void *vself, int token, int64_t *out_time) {
    AsioOutputPlugin *self = (AsioOutputPlugin*)vself;
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
    AsioOutputPlugin *self = (AsioOutputPlugin*)vself;
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
    AsioOutputPlugin *self = (AsioOutputPlugin*)vself;
    return RAAT__output_message_listeners_add(&self->message_listeners, cb, cb_userdata);
}

static RC__Status 
output_remove_message_listener(void *vself, RAAT__OutputMessageCallback cb, void *cb_userdata) {
    AsioOutputPlugin *self = (AsioOutputPlugin*)vself;
    return RAAT__output_message_listeners_remove(&self->message_listeners, cb, cb_userdata);
}

static RC__Status 
output_send_message(void *vself, json_t *message) {
    AsioOutputPlugin *self = (AsioOutputPlugin*)vself;
    char *s = json_dumps(message, 0);
    RAAT__TRACE("[asio] [%s] GOT MESSAGE %s", self->driver_name, s);
    free(s);
    return RC__STATUS_SUCCESS;
}

static RC__Status 
output_set_software_volume_signal_path(void *vself, json_t *soft_volume_signal_path) {
    AsioOutputPlugin *self = (AsioOutputPlugin*)vself;
    uv_mutex_lock(&self->lock);
    if (self->soft_volume_signal_path) json_decref(self->soft_volume_signal_path);
    self->soft_volume_signal_path = json_incref(soft_volume_signal_path);
    LOCKED_update_signal_path(self);
    uv_mutex_unlock(&self->lock);
    return RC__STATUS_SUCCESS;
}   

static RC__Status 
output_set_software_volume(void *vself, RAAT__OutputSoftwareVolume *volume) {
    AsioOutputPlugin *self = (AsioOutputPlugin*)vself;
    uv_mutex_lock(&self->lock);
    self->volume          = volume;
    uv_mutex_unlock(&self->lock);
    return RC__STATUS_SUCCESS;
}

typedef struct {
	AsioOutputPlugin *self;
	RC__Status	      status;
	CLSID			  clsid;
} output_plugin_new_state;

static void IN_ASIO_THREAD_output_plugin_new(output_plugin_new_state *state) {
	AsioOutputPlugin *self = state->self;
	RC__Status status = state->status;

	/*
	 * Open the driver
	 */
	self->drivers = new AsioDrivers();

	self->drvindex = -1;
	int idx = -1;
	int rc;

	for (LONG i = 0; i < self->drivers->asioGetNumDev(); i++) {
		char name[1024];
		char maybeclsid_s[1024];
		CLSID maybeclsid;
		if (0 == self->drivers->asioGetDriverName(i, name, sizeof(name)) && 0 == self->drivers->asioGetDriverCLSID(i, &maybeclsid)) {
			LPOLESTR maybeclsid_ws = NULL;
			StringFromCLSID(maybeclsid, &maybeclsid_ws);
			wcstombs(maybeclsid_s, maybeclsid_ws, 1024);
			RAAT__TRACE("[output/asio] checking asio driver name %s clsid %s", name, maybeclsid_s);
			CoTaskMemFree(maybeclsid_ws);
		}
		if (maybeclsid == state->clsid) {
			RAAT__TRACE("[output/asio]     ==> matched");
			self->driver_name = RC__allocator_strdup(self->alloc, name);
			idx = i;
			break;
		}
	}

	if (idx == -1) {
		LPOLESTR clsid_ws = NULL;
		char clsid_s[1024];
		StringFromCLSID(state->clsid, &clsid_ws);
		wcstombs(clsid_s, clsid_ws, 1024);
		RAAT__TRACE("[output/asio] asio driver not found for clsid %s");
		CoTaskMemFree(clsid_ws);
		status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail;
	}

	RAAT__TRACE("[output/asio] opening driver");
	rc = self->drivers->asioOpenDriver(idx, (void**)&self->asio);
	if (rc != 0) {
		status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail;
	}

	RAAT__TRACE("[output/asio] initializing driver");
	if (!self->asio->init(NULL)) {
		status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED; goto fail;
	}

	//
	// Now, get static data from the driver
	//
	if(self->asio->getChannels(&self->asio_input_channels, &self->asio_output_channels) == ASE_OK) {
		RAAT__TRACE("getChannels (inputs: %d, outputs: %d);", self->asio_input_channels, self->asio_output_channels);
		if (self->asio_output_channels < 2) {            // require 2 channels
			status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED; goto fail;
		}

		if (self->asio->getBufferSize(&self->asio_min_size, &self->asio_max_size, &self->asio_preferred_size, &self->asio_granularity) == ASE_OK) {
			RAAT__TRACE("[output/asio] getBufferSize (min: %d, max: %d, preferred: %d, granularity: %d);", self->asio_min_size, self->asio_max_size, self->asio_preferred_size, self->asio_granularity);
		} else {
			status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED; goto fail;
		}

		if (self->asio->outputReady() == ASE_OK) {
			self->asio_post_output = true;
		} else {
			self->asio_post_output = false;
		}

		self->thunk         = thunk_alloc(static_cast<void*>(self));
		if (self->thunk == NULL) {
			RAAT__ERROR("[output/asio] failed to initialize thunk");
			status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED; goto fail;
		}

		RAAT__TRACE("[output/asio] asio output is all good to go!");

		self->drvindex = idx;
	} else {
		status = RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED; goto fail;
	}

    RAAT__TRACE("[output/asio] probing formats");
    IN_ASIO_THREAD_probe_formats(self);

fail:
	state->status = status;
}

RC__Status 
RAAT__asio_output_plugin_new(RC__Allocator *alloc, RAAT__Device *device, json_t *config, RAAT__OutputPlugin **out_output) {
    InitOnceExecuteOnce(&static_init_once, StaticInitFunction, NULL, NULL);

    const char *driver_id = json_string_value(json_object_get(config, "device"));
    if (driver_id == NULL) return RAAT__OUTPUT_PLUGIN_STATUS_INVALID_CONFIG;


    RC__Status status = RC__STATUS_SUCCESS;

    wchar_t driver_id_w[1024];
    mbstowcs(driver_id_w, driver_id, 1024);

    CLSID clsid;
    HRESULT hr = CLSIDFromString(driver_id_w, &clsid);
    if (hr != S_OK) {
            return RAAT__OUTPUT_PLUGIN_STATUS_INVALID_CONFIG;
    }

    alloc = RC__allocator_default(alloc);
    AsioOutputPlugin *self            = RC__new0(alloc, AsioOutputPlugin, 1);
    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;

    // initialize the state that is associated with AsioOutputPlugin 
    self->alloc                        = alloc;
    self->log                          = RAAT__device_get_log(device);
    self->current_token                = RAAT__OUTPUT_TOKEN_INVALID;
    self->next_token                   = 1;
    self->state                        = IDLE;
    self->driver_id                    = RC__allocator_strdup(alloc, driver_id);

    RAAT__TRACE("[ASIO] initializing output driver_id=%s", driver_id);

    json_t *custom_signal_path = json_object_get(config, "signal_path");
    if (custom_signal_path) self->custom_signal_path = json_copy(custom_signal_path);

    json_t *resync_delay = json_object_get(config, "resync_delay");
    if (resync_delay) self->resync_delay_secs = json_number_value(resync_delay);
    if (self->resync_delay_secs == 0) self->resync_delay_secs = 0.1;
    RAAT__TRACE("[ASIO] resync delay=%fs", self->resync_delay_secs);

    json_t *max_dsd_rate = json_object_get(config, "max_dsd_rate");
    if (max_dsd_rate) self->max_dsd_rate = json_number_value(max_dsd_rate);
    if (self->max_dsd_rate == 0) self->max_dsd_rate = 256;
    RAAT__TRACE("[ASIO] max dsd rate=%d", self->max_dsd_rate);

    if (json_is_true(json_object_get(config, "use_max_buffer_size")))
        self->use_max_buffer_size = true;
    RAAT__TRACE("[ASIO] use max buffer size=%d", self->use_max_buffer_size);

    if (json_is_true(json_object_get(config, "power_of_two_buffer_size")))
        self->power_of_two_buffer_size = true;
    RAAT__TRACE("[ASIO] use power-of-two buffer size=%d", self->power_of_two_buffer_size);

    const char *dsd_mode_s     = json_string_value(json_object_get(config, "dsd_mode"));
    if      (dsd_mode_s && !strcmp(dsd_mode_s, "dop"))           self->dsd_mode = DSD_MODE_DOP;
    else if (dsd_mode_s && !strcmp(dsd_mode_s, "dcs"))           self->dsd_mode = DSD_MODE_DCS;
    else if (dsd_mode_s && !strcmp(dsd_mode_s, "native"))        self->dsd_mode = DSD_MODE_NATIVE;
    else if (dsd_mode_s && !strcmp(dsd_mode_s, "native_or_dop")) self->dsd_mode = DSD_MODE_NATIVE_OR_DOP;
    else if (dsd_mode_s && !strcmp(dsd_mode_s, "native_or_dcs")) self->dsd_mode = DSD_MODE_NATIVE_OR_DCS;
    else                                                         self->dsd_mode = DSD_MODE_NONE;

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

	output_plugin_new_state threadstate;
	threadstate.self   = self;
	threadstate.clsid  = clsid;
	threadstate.status = RC__STATUS_SUCCESS;
	run_in_asio_thread_sync((asio_thread_proc)IN_ASIO_THREAD_output_plugin_new, &threadstate);
	status = threadstate.status;

	if (status != RC__STATUS_SUCCESS) goto fail;
    RAAT__TRACE("[output/asio] initialized");
    *out_output = &self->plugin;
    return RC__STATUS_SUCCESS;

fail:
	RAAT__asio_output_plugin_delete((RAAT__OutputPlugin*)self);
	return status;
}

static void IN_ASIO_THREAD_output_plugin_delete(AsioOutputPlugin *self) {
	if (self->drvindex != -1) self->drivers->asioCloseDriver(self->drvindex);
	thunk_free(self->thunk);
	delete self->drivers;
}

void
RAAT__asio_output_plugin_delete(RAAT__OutputPlugin *output) {
    AsioOutputPlugin *self = (AsioOutputPlugin*)output;

	// close ASIO driver
	run_in_asio_thread_sync((asio_thread_proc)IN_ASIO_THREAD_output_plugin_delete, self);

	// destroy the mutex
    uv_mutex_destroy(&self->lock);

	// free allocated strings/buffers
    RC__free(self->alloc, self->driver_id);
    RC__free(self->alloc, self->driver_name);
	RC__free(self->alloc, self->procbuf);
	RC__free(self->alloc, self->origbuf);
	RC__free(self->alloc, self->dsdbuf);

	// free other stuff
    if (self->info)                    json_decref(self->info);
    if (self->custom_signal_path)      json_decref(self->custom_signal_path);
    if (self->soft_volume_signal_path) json_decref(self->soft_volume_signal_path);
    RAAT__output_message_listeners_destroy(&self->message_listeners);

    RC__free(self->alloc, self);
}

void
RAAT__asio_output_plugin_set_lost_cb(RAAT__OutputPlugin *output, RAAT__AsioOutputPluginLostCallback cb, void *userdata) {
    AsioOutputPlugin *self = (AsioOutputPlugin*)output;
    self->device_lost_cb = cb;
    self->device_lost_userdata = userdata;
}
