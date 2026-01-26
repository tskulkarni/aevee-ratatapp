//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_dsp.h"
#include "rc_log.h"

#include <string.h>
#include <math.h>

#define RAAT__CURRENT_LOG self->log

static int32_t get_pcm_sample(RAAT__StreamFormat *format, uint8_t *frame_ptr, int ch) {
    uint8_t *sample_ptr = frame_ptr + ch * (format->bits_per_sample / 8);
    switch (format->bits_per_sample) {
        case 16: return (int32_t)(((uint32_t)sample_ptr[0] << 16) | ((uint32_t)sample_ptr[1] << 24));
        case 24: return (int32_t)(((uint32_t)sample_ptr[0] << 8)  | ((uint32_t)sample_ptr[1] << 16) | ((uint32_t)sample_ptr[2] << 24));
        case 32: return (int32_t)(((uint32_t)sample_ptr[0] << 0)  | ((uint32_t)sample_ptr[1] << 8)  | ((uint32_t)sample_ptr[2] << 16) | ((uint32_t)sample_ptr[3] << 24));
        default: return 0;
    }
}

static void set_pcm_sample(RAAT__StreamFormat *format, uint8_t *frame_ptr, int ch, int32_t v) {
    uint8_t *sample_ptr = frame_ptr + ch * (format->bits_per_sample / 8);
    switch (format->bits_per_sample) {
        case 16: 
            sample_ptr[0] = (uint8_t)((v >> 16) & 0xff);
            sample_ptr[1] = (uint8_t)((v >> 24) & 0xff);
            break;
        case 24: 
            sample_ptr[0] = (uint8_t)((v >>  8) & 0xff);
            sample_ptr[1] = (uint8_t)((v >> 16) & 0xff);
            sample_ptr[2] = (uint8_t)((v >> 24) & 0xff);
            break;
        case 32: 
            sample_ptr[0] = (uint8_t)((v >>  0) & 0xff);
            sample_ptr[1] = (uint8_t)((v >>  8) & 0xff);
            sample_ptr[2] = (uint8_t)((v >> 16) & 0xff);
            sample_ptr[3] = (uint8_t)((v >> 24) & 0xff);
            break;
        default: break;
    }
}

//
// computes the average of two PCM samples, dithered based on format, with saturation
//
// This is used when stuffing samples--we interpolate between the adjacent samples to minimize
// audible artifacts
//
static int32_t pcm_interpolate(RAAT__StreamFormat *format, int a, int32_t c) {
    int64_t val64      =  ((int64_t)a + (int64_t)c) / 2;

    // dither
    int64_t dithered64 = val64 + 
                         (RAAT__dither() >> (format->bits_per_sample-1)) +      // dither shifted to the last bit
                         (0x4 << (32 - format->bits_per_sample));               // compensate for DC offset that arises during truncation

    // saturate if there was an overflow
    int64_t saturated64;
    if      (dithered64 > INT32_MAX) saturated64 = INT32_MAX;
    else if (dithered64 < INT32_MIN) saturated64 = INT32_MIN;
    else                             saturated64 = dithered64;

    // truncate back to a 32bit int of the appropriate bit-depth for the current format
    int32_t mask = (int32_t)0x80000000 >> (format->bits_per_sample-1);

    return (int32_t)saturated64 & mask;
}

double 
RAAT__db_attenuation_to_linear_gain(double db) {
    if (db >= 0) return 1.0;
    return pow(10.0, db / 20.0);
}


RC_API RC__Status
RAAT__read_stream_with_drift_correction(RAAT__Stream *stream, int64_t streamtime, uint8_t *out_buf, int nsamples, int correction_samples, RAAT__AudioPacketMetadata **out_metadata, size_t *out_nmetadata) {
    if (correction_samples == 0) {
        return RAAT__stream_read(stream, streamtime, out_buf, nsamples, out_metadata, out_nmetadata);
    }

    RAAT__StreamFormat *format = RAAT__stream_format(stream);

    RAAT__AudioPacketMetadata *metadata;
    size_t nmetadata;

    int nsamples_to_read = nsamples + correction_samples;
    uint8_t *in_buf = alloca(RAAT__stream_format_compute_buffer_size(format, nsamples_to_read));
    RC__Status status = RAAT__stream_read(stream, streamtime, in_buf, nsamples_to_read, &metadata, &nmetadata);
    if (!RC__STATUS_IS_SUCCESS(status)) return status;

    if (format->sample_type == RAAT__SAMPLE_TYPE_DSD) {
        int correction_count = abs(correction_samples) / 2;

        int *offsets = (int*)alloca(sizeof(int) * (correction_count + 1));
        offsets[correction_count] = -1;

        int i,j;
        for (i = 0; i < correction_count; i++) {
            int min_val = (nsamples / correction_count) * i;
            int max_val = (nsamples / correction_count) * (i + 1) - 16;
            int range = max_val - min_val;
            int sample_offset = min_val + rand() % range;
            offsets[i] = sample_offset;

            for (j = 0; j < (int)nmetadata; j++) {
                if (metadata[j].sample > sample_offset)
                    metadata[j].sample += correction_samples > 0 ? 1 : -1;
            }
        }

        // in_i/out_i are measured in samples, not bytes. Be careful
        int out_i = 0;
        int in_i  = 0;
        while (out_i < nsamples) {
            int ch;
            bool stuff = false;
            bool drop  = false;

            if (*offsets != -1 && out_i > *offsets) {
                if (correction_samples < 0) {       // we read less than we are writing, so move the input pointer back (stuff)
                    // when stuffing, insert 2 samples at a time, always 10.
                    in_i    -= 2;
                    offsets ++;
                    stuff = true;
                } else {                            // we read more than we are writing, so move the input pointer forward (drop)
                    drop = true;
                    offsets += 4;
                    // note, we will increment in_i at the end. We're going to actually read all of the bits and then choose which to keep instead of 
                    // simply skipping them. See the large comment below for more information.
                }
            }

            // notes: out_i is always divisible by 8, but in_i  can be any value. thus we may need to sample 2 input bytes to build the output byte
            int in_b         = in_i / 8;
            int in_remainder = in_i % 8;
            int out_b        = out_i / 8;

            for (ch = 0; ch < format->channels; ch++) {
                uint8_t val;
                if (in_remainder == 0) {
                    val = in_buf[format->channels * in_b + ch];
                } else {
                    uint8_t a = in_buf[format->channels * (in_b + 0) + ch];
                    uint8_t b = in_buf[format->channels * (in_b + 1) + ch];
                    switch (in_remainder) {
                        case 1:  val = ((a << 1) & 0xfe) | ((b >> 7) & 0x01); break;
                        case 2:  val = ((a << 2) & 0xfc) | ((b >> 6) & 0x03); break;
                        case 3:  val = ((a << 3) & 0xf8) | ((b >> 5) & 0x07); break;
                        case 4:  val = ((a << 4) & 0xf0) | ((b >> 4) & 0x0f); break;
                        case 5:  val = ((a << 5) & 0xe0) | ((b >> 3) & 0x1f); break;
                        case 6:  val = ((a << 6) & 0xc0) | ((b >> 2) & 0x3f); break;
                        case 7:  val = ((a << 7) & 0x80) | ((b >> 1) & 0x7f); break;
                        default: val = 0; break;
                    }
                }

                if (drop) {
                    //
                    // This is crude, but it works OK. We're looking for a better idea. We experimented with a lot of options 
                    // and ended up with this method for dropping samples from DSD without producing major artifacts.
                    //
                    // The goal in this code is to read 16 bits worth of input data, and produce 8 bits of output data. Our experiments 
                    // show that making a single larger (8-bit) correction is less obtrusive than making multiple smaller corrections.
                    //
                    // We walk the 16 bits and look for segments that have two out of four bits set. These are "safer" to delete.
                    //
                    // If we can't find two of those, then arbitrary bits are skipped. This is very rare in real-world DSD data.
                    //
                    // In our test environment, we force 25 8-bit corrections/sec. This is an unrealistically high amount of correction,
                    // but is good for proving out the transparency of a scheme like this.
                    //
                    // On very demanding content, particularly well-recorded classical content, slight artifacts are audible
                    // in a good listening environment. On less demanding content, it's very difficult to hear the corrections.
                    //
                    // In a system with accurate clocks, corrections are much rarer than they were in our test environment. 
                    //
                    int in_b2         = (in_i+8) / 8;
                    int in_remainder2 = (in_i+8) % 8;

                    uint8_t val1 = val;
                    uint8_t val2;

                    if (in_remainder2 == 0) {
                        val2 = in_buf[format->channels * in_b2 + ch];
                    } else {
                        uint8_t a = in_buf[format->channels * (in_b2 + 0) + ch];
                        uint8_t b = in_buf[format->channels * (in_b2 + 1) + ch];
                        switch (in_remainder2) {
                            case 1:  val2 = ((a << 1) & 0xfe) | ((b >> 7) & 0x01); break;
                            case 2:  val2 = ((a << 2) & 0xfc) | ((b >> 6) & 0x03); break;
                            case 3:  val2 = ((a << 3) & 0xf8) | ((b >> 5) & 0x07); break;
                            case 4:  val2 = ((a << 4) & 0xf0) | ((b >> 4) & 0x0f); break;
                            case 5:  val2 = ((a << 5) & 0xe0) | ((b >> 3) & 0x1f); break;
                            case 6:  val2 = ((a << 6) & 0xc0) | ((b >> 2) & 0x3f); break;
                            case 7:  val2 = ((a << 7) & 0x80) | ((b >> 1) & 0x7f); break;
                            default: val2 = 0; break;
                        }
                    }

                    // comb contains 16 bits. We need to keep 8 and discard 8.
                    uint32_t comb = ((uint32_t)val1 << 8) | val2;

                    val = 0;
                    int removed = 0;
                    for (i = 0; i < 16; i++) {
                        uint32_t masked = comb & 0xf000;
                        if (removed < 2 && (masked == 0xa000 || masked == 0x5000 || masked == 0x9000 || masked == 0x6000 || masked == 0xc000 || masked == 0x3000)) {
                            removed++;
                            i += 4;
                            comb <<= 4;
                        }
                        val = (val << 1) | ((comb >> 15) & 0x1);
                        comb <<= 1;
                    }

                    out_buf[format->channels * out_b + ch] = val;

                } else if (stuff) {
                    //
                    // The first two samples in the value as read from the input stream are repeats. Instead
                    // of including the repeats (which makes a nasty artifact), insert a 1 bit then a 0 bit. This is 
                    // far less audible than repeating the previous two bits, or inserting a pattern with two zeroes or two ones
                    //
                    out_buf[format->channels * out_b + ch] = (val & 0x3f) | 0x80;
                } else {
                    out_buf[format->channels * out_b + ch] = val;
                }
            }

            if (drop) {
                in_i += 8;
            }

            in_i  += 8;
            out_i += 8;
        }

    } else {
        int correction_sample_count = correction_samples < 0 ? -correction_samples : correction_samples;

        int *offsets = (int*)alloca(sizeof(int) * (correction_sample_count + 1));
        offsets[correction_sample_count] = -1;

        // compute pseudo-random locations for corrections. First break up the buffer into equal segments, then position
        // corrections randomly within each segment.
        int i,j;
        for (i = 0; i < correction_sample_count; i++) {
            int min_val = (nsamples / correction_sample_count) * i + 1;
            int max_val = (nsamples / correction_sample_count) * (i + 1) - 2;
            int range = max_val - min_val;
            int sample_offset = min_val + rand() % range;
            offsets[i] = sample_offset;

            for (j = 0; j < (int)nmetadata; j++) {
                if (metadata[j].sample > sample_offset)
                    metadata[j].sample += correction_samples > 0 ? 1 : -1;
            }
        }

        int bytes_per_frame = RAAT__stream_format_compute_buffer_size(format, 1);

        int out_i = 0;
        int in_i  = 0;
        while (out_i < nsamples) {
            int j;
            bool stuff = false;

            if (*offsets == out_i) {
                if (correction_samples < 0) {       // we read less than we are writing, so move the input pointer back (stuff)
                    in_i--;
                    stuff = true;
                } else {                            // we read more than we are writing, so move the output pointer back (drop)
                    out_i--;
                }
                offsets++;
            }

            if (stuff) {
                int ch;
                for (ch = 0; ch < format->channels; ch++) {
                    int32_t a = get_pcm_sample(format, &in_buf[bytes_per_frame * in_i],     ch);
                    int32_t c = get_pcm_sample(format, &in_buf[bytes_per_frame * (in_i+1)], ch);
                    int32_t b = pcm_interpolate(format, a, c);
                    //RC__TRACE("stuff ch%d a=%d b=%d c=%d\n", ch, a, b, c);
                    set_pcm_sample(format, &out_buf[bytes_per_frame * out_i], ch, b);
                }
            } else {
                // copy a frame 
                for (j = 0; j < bytes_per_frame; j++) {
                    out_buf[bytes_per_frame * out_i + j] = in_buf[bytes_per_frame * in_i + j];
                }
            }

            in_i++;
            out_i++;
        }
    }

    if (out_metadata)  *out_metadata  = metadata;
    if (out_nmetadata) *out_nmetadata = nmetadata;

    return RC__STATUS_SUCCESS;
}

void
RAAT__drift_correction_set_remote_time(RAAT__DriftCorrection *self, int64_t local_time, int64_t remote_time_offset, bool new_source) {
    int64_t remote_time = local_time + remote_time_offset;
    if (new_source) {
        self->drift_local_base_time    = self->drift_local_last_time  = local_time;
        self->drift_remote_base_time   = self->drift_remote_last_time = remote_time;
        self->drift_correction_samples = 0;
        self->is_active                = true;
        self->error_total              = 0;
        self->error_count              = 0;
        self->error_pos                = 0;
        self->error_sample_count_accum = 0;
        if (self->log) RAAT__TRACE("[drift] new remote clock %lld at local clock %lld", remote_time, local_time);
    } else {
        self->drift_local_last_time  = local_time;
        self->drift_remote_last_time = remote_time;

        int64_t local_time_since_base  = self->drift_local_last_time - self->drift_local_base_time;
        int64_t remote_time_since_base = self->drift_remote_last_time - self->drift_remote_base_time;
        int64_t error                  = remote_time_since_base - local_time_since_base;
        int64_t drift_correction       = (self->drift_correction_samples * 1000000000LL / self->format.sample_rate);
        int64_t net_error              = error + drift_correction;
        int64_t net_error_samples      = net_error * self->format.sample_rate / 1000000000LL;

        if (self->log)
            RAAT__TRACE("[drift] update remote clock %lldus at local clock %lldus error=%lldus correction=%lldus neterror=%lldus (%lld samples @ %dhz)", 
                        remote_time/1000, local_time/1000, error/1000, drift_correction/1000, net_error/1000, net_error_samples, self->format.sample_rate);
    }
}

void
RAAT__drift_correction_init(RAAT__DriftCorrection *self, RAAT__Log *log, RAAT__StreamFormat *format) {
    memset(self, 0, sizeof(RAAT__DriftCorrection));
    self->log    = log;
    self->format = *format;
}

int
RAAT__drift_correction_compute_correction(RAAT__DriftCorrection *self, int samples_per_buf) {
    if (!self->is_active) return 0;

    int correction_threshold_samples = self->format.sample_rate * 2 / 1000;  // 2ms is threshold for correction

    // compute needed correction
    int64_t local_time_since_base  = self->drift_local_last_time - self->drift_local_base_time;
    int64_t remote_time_since_base = self->drift_remote_last_time - self->drift_remote_base_time;
    int64_t error                  = remote_time_since_base - local_time_since_base;

    // compute moving average of error
    self->error_total += error;
    self->error_samples[self->error_pos++] = error; 
   
    if (self->error_sample_count_accum < self->format.sample_rate * 20 && self->error_count < RAAT__MAX_ERROR_SAMPLES) {
        // keep increasing the length of the moving average until we have 20s of audio
        self->error_sample_count_accum += samples_per_buf;
        self->error_count++;
        return 0;
    }  else {
        self->error_pos %= self->error_count;
        self->error_total -= self->error_samples[self->error_pos];
    }

    int64_t error_moving_avg = self->error_total / self->error_count;

    // convert to samples
    int64_t error_samples          = (int64_t)((double)error_moving_avg * (double)self->format.sample_rate / 1000000000.0);
    int64_t net_error_samples      = error_samples + self->drift_correction_samples;

    // compute max correction based on drift rate
    double drift_rate           = (double)remote_time_since_base / (double)local_time_since_base;
    double bufsize_seconds      = (double)((double)samples_per_buf / self->format.sample_rate);
    int max_correction_samples  = RC__max(1, (int)((RC__max(drift_rate, 1/drift_rate) - 1.0) * bufsize_seconds * 1.5 * self->format.sample_rate));

    //RAAT__TRACE("[drift] net_error_samples %lld drift rate %f max_correction_samples %d", net_error_samples, drift_rate, max_correction_samples);

    int abs_correction_amount;
    if (self->format.bits_per_sample == 1) {
        abs_correction_amount = (max_correction_samples + 7) / 8 * 8; // round up to nearest mult of 8
    } else {
        abs_correction_amount = max_correction_samples;
    }

    int drift_correction = 0;

    if (net_error_samples < -correction_threshold_samples) drift_correction =  abs_correction_amount;
    if (net_error_samples >  correction_threshold_samples) drift_correction = -abs_correction_amount;

    self->drift_correction_samples += drift_correction;

    return drift_correction;
}

void 
RAAT__pack_dop_samples(RAAT__StreamFormat *format, uint8_t *input_buf, uint8_t *output_buf, int n_dsd_samples, bool *flipper_ptr) {
    bool flipper = *flipper_ptr;
    int n_pcm_samples = n_dsd_samples / 16;
    int i;
    if (format->channels == 1) {
        for (i = 0; i < n_pcm_samples; i++) {
            output_buf[i * 3 + 0] = input_buf[i * 2 + 0];
            output_buf[i * 3 + 1] = input_buf[i * 2 + 1];
            output_buf[i * 3 + 2] = flipper ? 0x05 : 0xfa;
            flipper = !flipper;
        }
    } else if (format->channels == 2) {
        for (i = 0; i < n_pcm_samples; i++) {
            output_buf[i * 6 + 0] = input_buf[i * 4 + 2];                                   
            output_buf[i * 6 + 1] = input_buf[i * 4 + 0];
            output_buf[i * 6 + 2] = flipper ? 0x05 : 0xfa;

            output_buf[i * 6 + 3] = input_buf[i * 4 + 3];
            output_buf[i * 6 + 4] = input_buf[i * 4 + 1];
            output_buf[i * 6 + 5] = flipper ? 0x05 : 0xfa;
            flipper = !flipper;
        }
    } else {
        int ch;
        int in_size  = 2 * format->channels;
        int out_size = 3 * format->channels;
        for (i = 0; i < n_pcm_samples; i++) {
            for (ch = 0; ch < format->channels; ch++) {
                output_buf[i * out_size + ch * 3 + 0] = input_buf[i * in_size + format->channels + ch];
                output_buf[i * out_size + ch * 3 + 1] = input_buf[i * in_size + ch];
                output_buf[i * out_size + ch * 3 + 2] = flipper ? 0x05 : 0xfa;
            }
            flipper = !flipper;
        }
    }
    *flipper_ptr = flipper;
}

void 
RAAT__pack_dcs_samples(RAAT__StreamFormat *format, uint8_t *input_buf, uint8_t *output_buf, int n_dsd_samples) {
    int n_pcm_samples = n_dsd_samples / 16;
    int i;
    if (format->channels == 1) { 
        for (i = 0; i < n_pcm_samples; i++) {
            output_buf[i * 3 + 0] = input_buf[i * 2 + 0];
            output_buf[i * 3 + 1] = input_buf[i * 2 + 1];
            output_buf[i * 3 + 2] = 0xaa;
        }
    } else if (format->channels == 2) {
        for (i = 0; i < n_pcm_samples; i++) {
            output_buf[i * 6 + 0] = input_buf[i * 4 + 2];
            output_buf[i * 6 + 1] = input_buf[i * 4 + 0];
            output_buf[i * 6 + 2] = 0xaa;

            output_buf[i * 6 + 3] = input_buf[i * 4 + 3];
            output_buf[i * 6 + 4] = input_buf[i * 4 + 1];
            output_buf[i * 6 + 5] = 0xaa;
        }
    } else {
        int ch;
        int in_size  = 2 * format->channels;
        int out_size = 3 * format->channels;
        for (i = 0; i < n_pcm_samples; i++) {
            for (ch = 0; ch < format->channels; ch++) {
                output_buf[i * out_size + ch * 3 + 0] = input_buf[i * in_size + format->channels + ch];
                output_buf[i * out_size + ch * 3 + 1] = input_buf[i * in_size + ch];
                output_buf[i * out_size + ch * 3 + 2] = 0xaa;
            }
        }
    }
}

static int __rpdf = 0x3fffffff;

int 
RAAT__dither(void)
{
    // Generate Triangular probability distribution function (TPDF) dither.
    // tpdf has TPDF over the range $80000000 (most negative) to $7fffffff (most positive)
    int feedback, tpdf;

    feedback = __rpdf >> 3;
    __rpdf = ((__rpdf & 7) << 28) ^ (feedback << 3) ^ (feedback);
    tpdf = __rpdf;

    feedback = __rpdf >> 3;
    __rpdf = ((__rpdf & 7) << 28) ^ (feedback << 3) ^ (feedback);
    tpdf -= __rpdf;

    return tpdf;
}

static int32_t 
linear_gain_to_fixedpoint(double linear_gain) {
    double fixedgain_dbl = linear_gain * (double)INT32_MAX;
    if (fixedgain_dbl > (double)INT32_MAX) fixedgain_dbl = (double)INT32_MAX;
    if (fixedgain_dbl < (double)INT32_MIN) fixedgain_dbl = (double)INT32_MIN;
    return (int32_t)fixedgain_dbl;
}

void 
RAAT__pcm_gain_16(uint8_t *data, double linear_gain, int nvalues) {
    //fprintf(stderr, "pcm gain 16\n");
    int i;

    if (linear_gain == 1.0) return;
    int32_t fixedgain_i32 = linear_gain_to_fixedpoint(linear_gain);

    for (i = 0; i < nvalues; i++) {
        int    val_i32    = (int)(((uint32_t)data[2*i+0] << 16) | ((uint32_t)data[2*i+1] << 24));

        int64_t scaled_i64                   = (int64_t)val_i32 * (int64_t)fixedgain_i32;
        int64_t scaled_i64_dithered24        = scaled_i64 + ((int64_t)RAAT__dither() << 16);
        int64_t scaled_i64_dithered24_shr    = scaled_i64_dithered24 >> 31;
        int64_t scaled_i64_dithered24_shr_dc = scaled_i64_dithered24_shr + 0x8000;        // compensate for DC offset from truncation

        int64_t saturated_i64;
        if      (scaled_i64_dithered24_shr_dc > INT32_MAX) saturated_i64 = INT32_MAX;
        else if (scaled_i64_dithered24_shr_dc < INT32_MIN) saturated_i64 = INT32_MIN;
        else                                               saturated_i64 = scaled_i64_dithered24_shr_dc;

        int32_t scaled_i32 = (int32_t)saturated_i64;

        data[2*i+0] = (uint8_t)(scaled_i32 >> 16);
        data[2*i+1] = (uint8_t)(scaled_i32 >> 24);
    }
}

void 
RAAT__pcm_gain_24(uint8_t *data, double linear_gain, int nvalues) {
    //fprintf(stderr, "pcm gain 24\n");
    int i;

    if (linear_gain == 1.0) return;
    int32_t fixedgain_i32 = linear_gain_to_fixedpoint(linear_gain);

    for (i = 0; i < nvalues; i++) {
        int    val_i32    = (int)(((uint32_t)data[3*i+0] << 8) | ((uint32_t)data[3*i+1] << 16) | ((uint32_t)data[3*i+2] << 24));

        int64_t scaled_i64                   = (int64_t)val_i32 * (int64_t)fixedgain_i32;
        int64_t scaled_i64_dithered24        = scaled_i64 + ((int64_t)RAAT__dither() << 8);
        int64_t scaled_i64_dithered24_shr    = scaled_i64_dithered24 >> 31;
        int64_t scaled_i64_dithered24_shr_dc = scaled_i64_dithered24_shr + 0x80;        // compensate for DC offset from truncation

        int64_t saturated_i64;
        if      (scaled_i64_dithered24_shr_dc > INT32_MAX) saturated_i64 = INT32_MAX;
        else if (scaled_i64_dithered24_shr_dc < INT32_MIN) saturated_i64 = INT32_MIN;
        else                                               saturated_i64 = scaled_i64_dithered24_shr_dc;

        int32_t scaled_i32 = (int32_t)saturated_i64;

        data[3*i+0] = (uint8_t)(scaled_i32 >>  8);
        data[3*i+1] = (uint8_t)(scaled_i32 >> 16);
        data[3*i+2] = (uint8_t)(scaled_i32 >> 24);
    }
}

void 
RAAT__pcm_gain_32(uint8_t *data, double linear_gain, int nvalues) {
    int i;

    if (linear_gain == 1.0) return;
    int32_t fixedgain_i32 = linear_gain_to_fixedpoint(linear_gain);

    for (i = 0; i < nvalues; i++) {
        int    val_i32    = (int)(((uint32_t)data[4*i+0] << 0) | ((uint32_t)data[4*i+1] << 8) | ((uint32_t)data[4*i+2] << 16) | ((uint32_t)data[4*i+3] << 24));

        int64_t scaled_i64                   = (int64_t)val_i32 * (int64_t)fixedgain_i32;
        int64_t scaled_i64_dithered          = scaled_i64 + (int64_t)RAAT__dither();
        int64_t scaled_i64_dithered_dc       = scaled_i64_dithered + ((int64_t)0x80 << 23);        // compensate for DC offset from truncation
        int64_t scaled_i64_dithered_dc_shr   = scaled_i64_dithered_dc >> 31;

        int64_t saturated_i64;
        if      (scaled_i64_dithered_dc_shr > INT32_MAX) saturated_i64 = INT32_MAX;
        else if (scaled_i64_dithered_dc_shr < INT32_MIN) saturated_i64 = INT32_MIN;
        else                                               saturated_i64 = scaled_i64_dithered_dc_shr;

        int32_t scaled_i32 = (int32_t)saturated_i64;

        data[4*i+0] = (uint8_t)(scaled_i32 >>  0);
        data[4*i+1] = (uint8_t)(scaled_i32 >>  8);
        data[4*i+2] = (uint8_t)(scaled_i32 >> 16);
        data[4*i+3] = (uint8_t)(scaled_i32 >> 24);
    }
}

