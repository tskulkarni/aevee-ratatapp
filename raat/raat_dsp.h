//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_PLUGIN_DSP_H
#define INCLUDED_RAAT_PLUGIN_DSP_H

#include "raat_stream.h"
#include "raat_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup raat_dsp Dsp
 *
 *  \brief
 *  This module implements DSP helper functions for RAAT
 *
 *  @{
 */

/**
 * Read from a \ref RAAT__Stream while applying the drift correction specified in <tt>correction_samples</tt>.
 * 
 * \param stream the \ref RAAT__Stream to read from
 * \param streamtime the time in the stream where the read should take place
 * \param out_buf an output buffer large enough to receive nsamples samples from the read operation
 * \param nsamples the number of samples required to fill the output buffer. Note that if <tt>correction_samples</tt> is non-zero then this will be different from the number of samples that are actually read from the stream.
 * \param correction_samples the difference between nsamples and the number of samples that should actually be read from the stream. This is a very small integer computed by RAAT__drift_correction_compute_correction. For DSD streams this must be a multiple of 8. 
 * \param out_metadata (optional) pointer that will receive metadata for the audio packets used to fill this buffer. This is used to convey volume normalization information. 
 * \param out_nmetadata (optional) pointer that will receive the number of metadata entries stored in out_metadata. 
 *
 * \retval The return status from the underlying \ref RAAT__stream_read operation. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__read_stream_with_drift_correction(RAAT__Stream *stream, int64_t streamtime, uint8_t *out_buf, int nsamples, int correction_samples, /*optional*/RAAT__AudioPacketMetadata **out_metadata, /*optional*/size_t *out_nmetadata);

#define RAAT__MAX_ERROR_SAMPLES 1000

/**
 * Data structure for storing state related to clock drift correction calculations
 *
 * Use of \ref RAAT__DriftCorrection is optional, and is not necessary in situations where hardware clocks can be "bent" to conform
 * to a remote clock signal.
 *
 * The contents of this structure are opaque.
 */
typedef struct {
    bool                         is_active;   
    RAAT__Log                   *log;
    RAAT__StreamFormat           format;
    int64_t                      drift_remote_base_time;                // in ns
    int64_t                      drift_local_base_time;                 // in ns
    int64_t                      drift_remote_last_time;                // in ns
    int64_t                      drift_local_last_time;                 // in ns
    int64_t                      drift_correction_samples;              // in orig format samples

    int64_t                      error_sample_count_accum;

    int64_t                      error_samples[RAAT__MAX_ERROR_SAMPLES];
    int64_t                      error_total;
    int                          error_count;
    int                          error_pos;
} RAAT__DriftCorrection;

/**
 * Initialize a \ref RAAT__DriftCorrection structure.
 *
 * \param self the structure to initIalize
 * \param log a \ref RAAT__Log structure for logging drift correction statistics
 * \param format the format of the stream that requires drift corrections.
 */
void
RAAT__drift_correction_init(RAAT__DriftCorrection *self, RAAT__Log *log, RAAT__StreamFormat *format);

/**
 * Compute an adjustment for an output buffer.
 *
 * If an output plugin is using a remote clock, it may call this function just prior to reading from a #RAAT__Stream
 * instance in order to determine a correction that should be applied to the #RAAT__stream_read operation.
 *
 * This is most commonly used in conjunction with #RAAT__read_stream_with_drift_correction, which accepts the return value
 * from this function in its <tt>correction_samples</tt> parameter.
 *
 * \param self the drift correction structure to use
 * \param samples_per_buf the number of samples in the output buffer
 *
 * \retval an adjustment, typically +/- a small number of samples that should be applied to the read operation 
 *         from a \ref RAAT__Stream that will full the current output buffer.
 */
int
RAAT__drift_correction_compute_correction(RAAT__DriftCorrection *self, int samples_per_buf);

/**
 * Inform the \ref RAAT__DriftCorrection calculation of the current remote clock offset.
 *
 * If an output module is using RAAT__DriftCorrection, this must be called each time the output plugin's 
 * set_remote_time function is called, with the same arguments.
 *
 */
void
RAAT__drift_correction_set_remote_time(RAAT__DriftCorrection *self, int64_t local_time, int64_t remote_time_offset, bool new_source);

/**
 *
 * Helper function for packing DoP-encapsulated DSD sample data.
 *
 * \param format The DSD format being packed
 * \param input_buf storage for the input samples. This should be at least <tt>n_dsd_samples * format->channels / 8 bytes</tt> of data
 * \param output_buf storage for the output PCM samples. This space should be at least <tt>n_dsd_samples / 8 * format->channels * 3 / 2</tt> bytes
 * \param n_dsd_samples the number of DSD samples to convert. This must be a multiple of 8.
 * \param flipper a pointer to a boolean that's maintained across multiple calls to \ref RAAT__pack_dop_samples that pertain to the same stream
 */
void 
RAAT__pack_dop_samples(RAAT__StreamFormat *format, uint8_t *input_buf, uint8_t *output_buf, int n_dsd_samples, bool *flipper);

/**
 *
 * Helper function for packing dCS-encapsulated DSD sample data.
 *
 * \param format The DSD format being packed
 * \param input_buf storage for the input samples. This should be at least <tt>n_dsd_samples * format->channels / 8 bytes</tt> of data
 * \param output_buf storage for the output PCM samples. This space should be at least <tt>n_dsd_samples / 8 * format->channels * 3 / 2</tt> bytes
 * \param n_dsd_samples the number of DSD samples to convert. This must be a multiple of 8.
 */
void 
RAAT__pack_dcs_samples(RAAT__StreamFormat *format, uint8_t *input_buf, uint8_t *output_buf, int n_dsd_samples);

/**
 *
 * Pseudo-random number generator that generates a TPDF dither.
 */
int 
RAAT__dither(void);

double 
RAAT__db_attenuation_to_linear_gain(double db);

void 
RAAT__pcm_gain_16(uint8_t *data, double linear_gain, int nvalues);

void 
RAAT__pcm_gain_24(uint8_t *data, double linear_gain, int nvalues);

void 
RAAT__pcm_gain_32(uint8_t *data, double linear_gain, int nvalues);

/** @} */

#ifdef __cplusplus
}
#endif

#endif
