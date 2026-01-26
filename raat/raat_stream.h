//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RAAT_STREAM_H
#define INCLUDED_RAAT_STREAM_H

#include "rc_base.h"
#include "rc_allocator.h"
#include "rc_status.h"

#include "raat_log.h"

#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup raat_stream Stream
 *
 *  \brief
 *  The stream plugin represents an audio stream.
 *
 *  @{
 */

/** Opaque structure definition for RAAT__Stream */
typedef struct RAAT__Stream_s RAAT__Stream; 

/** Represents the sample type contained in audio sample data */
typedef enum {
    RAAT__SAMPLE_TYPE_PCM,
    /**< PCM samples */
    RAAT__SAMPLE_TYPE_DSD,
    /**< DSD samples */
} RAAT__SampleType;

typedef enum {
    RAAT__SAMPLE_SUBTYPE_NONE,
    /**< No Subtype*/
    RAAT__SAMPLE_SUBTYPE_MQA,
    /**< MQA stream*/
    RAAT__SAMPLE_SUBTYPE_MQA_CORE,
    /**< MQA Core stream*/
} RAAT__SampleSubtype;

/** Represents the format of an audio stream */
typedef struct {
    /** The sample type */
    RAAT__SampleType    sample_type;
    /** The sample rate (in hZ) */
    int                 sample_rate;
    /** Number of bits per sample */
    int                 bits_per_sample;
    /** Number of channels */
    int                 channels;
    /** subtype */
    RAAT__SampleSubtype sample_subtype;
    /** "Original Sample Rate" for MQA content */
    int                 mqa_original_sample_rate;
} RAAT__StreamFormat;

/**
 * Represents metadata that came along with an audio packet. #RAAT__stream_read optionally 
 * returns this metadata for endpoints that can apply volume normalization adjustments in hardware.
 */
typedef struct {
    /** Offset within the buffer where this metadata becomes applicable */
    int     sample;                    
    /** Volume normalization gain in dB. 0.0 = no adjustment*/
    double  gain;                       
    /** Peak value for volume normalization. Only valid if less than or equal to zero. Only relevant if gain is non-zero */
    double  peak;                     
} RAAT__AudioPacketMetadata;

typedef struct {
    int64_t                   streamsample;
    void                     *buf;
    int                       nsamples;
    double                    gain;
    double                    peak;
} RAAT__AudioPacket;

/**
 * Instantiate a new stream.
 *
 * Streams are reference counted. If successful, this call must be matched with a call to #RAAT__stream_decref.
 *
 * \param alloc the allocator to use for this stream
 * \param format the format of the audio stream
 * \param log   a #RAAT__Log instance that the stream can use to output log messages
 * \param sample_capacity The number of samples that the stream can buffer before overflowing
 * \param out_self output parameter that will receive the newly constructed stream
 */
RC_API RC__Status
RAAT__stream_new                (RC__Allocator *alloc, RAAT__Log *log, RAAT__StreamFormat *format, int sample_capacity, RAAT__Stream **out_self);

/**
 * Write audio packet data into the stream.
 *
 * \param self the audio stream
 * \param streamtime the sample offset into the stream where this packet begins
 * \param gain the volume normalization gain for this stream (0.0 means no adjustment)
 * \param peak the volume normalization peak for this stream. Only valid if gain is non-zero
 * \param buf a buffer containing the sample data
 * \param nsamples the number of samples to copy
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__stream_write              (RAAT__Stream *self, int64_t streamtime, double gain, double peak, void *buf, int nsamples);               

/**
 * Write audio packet data into the stream.
 *
 * The chmap will be used to expand data and zero-fill accordingly, to avoid passing zeroes on the wire.
 *
 * \retval The return status. (See \ref rc_status for more information)
 */
RC_API RC__Status
RAAT__stream_write_chmap        (RAAT__Stream *self, int64_t streamsample, double gain, double peak, void *buf, int nsamples, unsigned chmap);

/**
 * Get the format for an audio stream
 *
 * \param self the audio stream
 *
 * \retval a pointer to a #RAAT__StreamFormat structure. This pointer is valid as long as the stream is alive
 */
RC_API RAAT__StreamFormat *
RAAT__stream_format             (RAAT__Stream *self);

/**
 * Read sample data from the audio stream.
 *
 * \param self the audio stream
 * \param streamtime the offset from the beginning of the stream, in samples, where the read operation should occur
 * \param out_buf output buffer that will receive the sample data.
 * \param nsamples number of samples to read
 * \param out_metadata optional output parameter that will receive metadata associated with the packets that satisfied this read operation. This pointer is valid until #RAAT__stream_read is called again or the stream is destroyed.
 * \param out_nmetadata number of #RAAT__AudioPacketMetadata instances returned in *out_metadata
 *
 */
RC_API RC__Status
RAAT__stream_read               (RAAT__Stream *self, int64_t streamtime, void *out_buf, int nsamples, /*opt*/RAAT__AudioPacketMetadata **out_metadata, size_t *out_nmetadata);

/**
 * Increment the reference count on this stream. 
 *
 * \param self the audio stream
 */
RC_API void 
RAAT__stream_incref             (RAAT__Stream   *self);

/**
 * Decrement the reference count on this stream, destroying it if the reference count reaches 0.
 *
 * \param self the audio stream
 */
RC_API void 
RAAT__stream_decref             (RAAT__Stream   *self);

/**
 * Represents statistics associated with an audio stream
 */
typedef struct {
    int     write_count;        // number of write operations into the buffer
    int     read_count;         // number of read operations from the buffer

    int64_t samples_in;         // number of samples that have been written to the stream
    int64_t samples_out;        // number of samples that have been read from the stream

    int     dropout_count;      // the number of times that samples were zero-filled because data was not available
    int64_t samples_dropped;    // the total number of samples that were dropped

    int     overrun_count;      // the number of times that samples were discarded because of a buffer overrun
    int64_t samples_overrun;    // the total number of samples that were discarded because of a buffer overrun

    int64_t capacity_samples;   // the capacity of the buffer in samples
    int64_t fill_samples;       // how many samples are currently stored in the buffer
} RAAT__StreamStats;

/**
 * Retrieves the statistics for an audio stream
 *
 * \param self the audio stream
 * \param out_stats output parameter that will receive the stats
 */
RC_API void 
RAAT__stream_get_stats          (RAAT__Stream   *self, RAAT__StreamStats *out_stats);

RC_API int   
RAAT__stream_get_sample_capacity(RAAT__Stream   *self);

/**
 * Clears out all audio sample data from a stream.
 *
 * \param self the audio stream
 */
RC_API void 
RAAT__stream_clear              (RAAT__Stream   *self);


/**
 * Consume a packet from the stream, blocking if necessary.
 *
 * Packets behave like datagrams--they can be duplicated or out of order. It is expected that downstream code will manage re-assembly. 
 *
 * This read can be canceled by RAAT__stream_cancel_blocking_read, in which case the method will return RC__STATUS_CANCELED. 
 * Otherwise the method will return RC__STATUS_SUCCESS.
 *
 * It is not safe to destroy the last reference to a stream during a consume call. The reader is responsible for keeping it alive.
 *
 * \param self the audio stream
 */
RC_API RC__Status
RAAT__stream_consume_packet     (RAAT__Stream *self, RAAT__AudioPacket *out_packet);

/**
 * Free resources associated with an audio packet that was populated by RAAT__stream_consume         
 *
 * \param self the audio stream
 */
RC_API void
RAAT__stream_destroy_packet      (RAAT__Stream *self, RAAT__AudioPacket *packet);

/**
 * Cancel all blocking reads that are in progress on this stream
 *
 * \param self the audio stream
 */
RC_API RC__Status
RAAT__stream_cancel_consume_packet (RAAT__Stream *self);


/**
 * Fills a sample buffer with silence. PCM buffers receive '0' bytes. DSD buffers receive '0x69' bytes. 
 *
 * \param format the format of the sample data
 * \param buf the buffer to zero-fill
 * \param nsamples the number of silent samples to generate
 */
RC_API
void RAAT__stream_format_zero_fill(RAAT__StreamFormat *format, void *buf, int nsamples);

/**
 * Convert samples to nanoseconds.
 *
 * \param f the stream format of the samples
 * \param nsamples the number of samples
 *
 * \retval the number of nanoseconds represented by nsamples samples of format f
 */
RC_API 
int64_t RAAT__stream_format_samples_to_ns(const RAAT__StreamFormat *f, int64_t nsamples);

/**
 * Convert nanoseconds to samples
 *
 * \param f the stream format of the samples
 * \param ns the number of nanoseconds
 *
 * \retval the number of samples represented by ns ns. For DSD, the return value will be rounded so that it is divisible by 8.
 */
RC_API 
int64_t RAAT__stream_format_ns_to_samples(const RAAT__StreamFormat *f, int64_t ns);

/** 
 * Compares two stream formats 
 *
 * \retval true if stream formats are equal, false otherwise 
 */
RC_API 
bool RAAT__stream_format_equals(const RAAT__StreamFormat *f1, const RAAT__StreamFormat *f2);

/**
 * Computes the number of bytes required to hold nsamples samples of the given stream format 
 *
 * \retval number of bytes required to hold nsamples samples of the given stream format 
 */
RC_API 
int RAAT__stream_format_compute_buffer_size(const RAAT__StreamFormat *f, int nsamples);

/** Number of bytes required to represent a stream format as a string */ 
#define RAAT__STREAM_FORMAT_MAX_STRLEN (128)

/** 
 * Convert a stream format to a null-terminated display string, mostly for debugging.
 *
 * Buffer must point to a memory region at least #RAAT__STREAM_FORMAT_MAX_STRLEN bytes in size.
 *
 * \param f the stream format
 * \param buffer the output buffer that will receive the string
 */
RC_API 
void RAAT__stream_format_to_string(const RAAT__StreamFormat *f, char *buffer/*[RAAT__STREAM_FORMAT_MAX_STRLEN]*/);

RC_API 
void RAAT__stream_format_repack(const RAAT__StreamFormat *from, uint8_t *from_buf, const RAAT__StreamFormat *to, uint8_t *to_buf, int nsamples);

 /** @} */

#ifdef __cplusplus
}
#endif


#endif

