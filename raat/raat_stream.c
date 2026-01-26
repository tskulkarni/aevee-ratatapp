//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_stream.h"

#include <string.h>

#define RAAT__CURRENT_LOG (self->log)

typedef struct Region_s {
    int64_t               streamsample;
    void                 *buf;
    int                   nsamples;

    double                gain;
    double                peak;

    struct Region_s      *next;
    struct Region_s      *prev;
} Region;

#define POOL_SIZE (100)

typedef struct {
    int    size;      
    void  *buf;
} databuf_t;

struct RAAT__Stream_s {
    // This lock might be accessed from sensitive places, so be careful with it. In particular, do not
    // use allocators within RAAT__Stream while this lock is held.
    uv_mutex_t            lock;
    uv_mutex_t            pool_lock;
    uv_cond_t             cond;
    RC__Allocator        *alloc;

    RAAT__Log            *log;
    RAAT__StreamFormat    format;

    int                   refcnt;

    // to control memory usage
    int                   sample_capacity;
    int                   sample_fill;

    // doubly-linked list of "alive" regions of sampledata
    Region               *head;
    Region               *tail;

    // singly linked list of "dead" regions that require cleanup
    Region               *dead_head;

    RAAT__StreamStats     stats;

    Region                   *region_pool[POOL_SIZE];
    int region_pool_count;

    databuf_t                 databuf_pool[POOL_SIZE];
    int databuf_pool_count;

    int                       metadata_capacity;
    RAAT__AudioPacketMetadata *metadata;
};

Region *alloc_region(RAAT__Stream *self) {
    uv_mutex_lock(&self->pool_lock);
    if (self->region_pool_count > 0) {
        Region * ret = self->region_pool[--self->region_pool_count];
        memset(ret, 0, sizeof(Region));
        uv_mutex_unlock(&self->pool_lock);
        return ret;
    }
    uv_mutex_unlock(&self->pool_lock);
    return RC__new0(self->alloc, Region, 1);
}

void free_region(RAAT__Stream *self, Region *r) {
    uv_mutex_lock(&self->pool_lock);
    if (self->region_pool_count == POOL_SIZE) { 
        RC__free(self->alloc, r); 
    } else {
        self->region_pool[self->region_pool_count++] = r;
    }
    uv_mutex_unlock(&self->pool_lock);
}

void free_databuf(RAAT__Stream *self, void *buf, int size) {
    uv_mutex_lock(&self->pool_lock);
    if (self->databuf_pool_count == POOL_SIZE) { 
        RC__free(self->alloc, buf); 
    } else {
        self->databuf_pool[self->databuf_pool_count  ].size = size;
        self->databuf_pool[self->databuf_pool_count++].buf  = buf;
    }
    uv_mutex_unlock(&self->pool_lock);
}

void *alloc_databuf(RAAT__Stream *self, int size) {
    uv_mutex_lock(&self->pool_lock);
    int i;
    for (i = self->databuf_pool_count - 1; i >= 0; i--) {
        if (self->databuf_pool[i].size >= size) {
            void *ret = self->databuf_pool[i].buf;
            if (i < self->databuf_pool_count - 1) {
                memmove(&self->databuf_pool[i], &self->databuf_pool[i+1], (self->databuf_pool_count - i - 1)*sizeof(databuf_t));
            } else {
            }
            self->databuf_pool_count--;
            uv_mutex_unlock(&self->pool_lock);
            return ret;
        }
    }
    uv_mutex_unlock(&self->pool_lock);
    return RC__alloc(self->alloc, size);
}

void free_pools(RAAT__Stream *self) {
    int i;
    for (i = 0; i < self->region_pool_count; i++) {
        RC__free(self->alloc, self->region_pool[i]);
    }
    self->region_pool_count = 0;

    for (i = 0; i < self->databuf_pool_count; i++) {
        RC__free(self->alloc, self->databuf_pool[i].buf);
    }
    self->databuf_pool_count = 0;
}

RC_API RC__Status
RAAT__stream_new                (RC__Allocator *alloc, RAAT__Log *log, RAAT__StreamFormat *format, int sample_capacity, RAAT__Stream **out_self) {
    alloc = RC__allocator_default(alloc);
    RAAT__Stream *self = RC__new0(alloc, RAAT__Stream, 1);
    if (self == NULL) return RC__STATUS_OUT_OF_MEMORY;

    self->alloc             = alloc;
    self->log               = log;
    self->format            = *format;
    self->refcnt            = 1;
    self->sample_capacity   = sample_capacity;
    self->sample_fill       = 0;
    self->head = self->tail = NULL;
    self->dead_head         = NULL;

    memset(&self->stats, 0, sizeof(self->stats));

    uv_mutex_init(&self->lock);
    uv_mutex_init(&self->pool_lock);
    uv_cond_init(&self->cond);

    *out_self = self;
    return RC__STATUS_SUCCESS;
}

static void destroy_region(RAAT__Stream *self, Region *region) {
    if (region) { 
        if (region->buf) {
            int nbytes = RAAT__stream_format_compute_buffer_size(&self->format, region->nsamples);
            free_databuf(self, region->buf, nbytes);
        }
        free_region(self, region);
    }
}

static void destroy_regions(RAAT__Stream *self, Region *r) {
    while (r) {
        Region *r_next = r->next;
        destroy_region(self, r);
        r = r_next;
    }
}

RC_API void 
RAAT__stream_clear              (RAAT__Stream   *self) {
    RC__ASSERT(self != NULL);
    Region *head, *dead_head;
    RAAT__AudioPacketMetadata *metadata;

    uv_mutex_lock(&self->lock);
    head      = self->head;
    dead_head = self->dead_head;
    metadata = self->metadata;
    self->head = NULL;
    self->tail = NULL;
    self->dead_head = NULL;
    self->metadata = NULL;
    uv_mutex_unlock(&self->lock);

    if (metadata) RC__free(self->alloc, metadata);

    destroy_regions(self, head);
    destroy_regions(self, dead_head);

    uv_mutex_lock(&self->pool_lock);
    free_pools(self);
    uv_mutex_unlock(&self->pool_lock);
}

RC_API RC__Status
RAAT__stream_write_chmap        (RAAT__Stream *self, int64_t streamsample, double gain, double peak, void *buf, int nsamples, unsigned chmap) {
    if (chmap == 0xffffffff) {
        return RAAT__stream_write(self, streamsample, gain, peak, buf, nsamples);
    }

    int buf_size_bytes = RAAT__stream_format_compute_buffer_size(&self->format, nsamples);
    uint8_t *buf2 = (uint8_t*)alloca(buf_size_bytes);
    int nframes;
    int chstride;
    if (self->format.sample_type == RAAT__SAMPLE_TYPE_DSD) {
        memset(buf2, 0x69, buf_size_bytes);
        nframes  = nsamples / 8;
        chstride = 1;
    } else {
        memset(buf2, 0x00, buf_size_bytes);
        nframes  = nsamples;
        chstride = self->format.bits_per_sample / 8;
    }
    uint8_t *inp  = buf;
    uint8_t *outp = buf2;
    int i;
    for (i = 0; i < nframes; i++) {
        int ch;
        for (ch = 0; ch < self->format.channels; ch++) {
            if (chmap & (1<<ch)) {
                int byt;
                for (byt = 0; byt < chstride; byt++) {
                    *outp++ = *inp++;
                }
            } else {
                outp += chstride;
            }
        }
    }
    return RAAT__stream_write(self, streamsample, gain, peak, buf2, nsamples);
}

RC_API RC__Status
RAAT__stream_write              (RAAT__Stream *self, int64_t streamsample, double gain, double peak, void *buf, int nsamples) {
    Region *it;

    uv_mutex_lock(&self->lock);

    // 1. check for duplicate packets. We don't want to waste effort or memory allocations
    //    if we already have this one.
    for (it = self->tail; it; it = it->prev) {
        if (it->streamsample == streamsample) {
            uv_mutex_unlock(&self->lock);
            return RC__STATUS_SUCCESS;
        }
        if (it->streamsample < streamsample) break;
    }


    // 2. If the buffer is full, discard old packets to make room. Note that 
    // no memory operatiojns happen within the lock.
    Region *dead_head = NULL;

    while (self->sample_fill + nsamples > self->sample_capacity) {
        Region *dead_region = self->head;
        RC__ASSERT(dead_region);

#if !defined(PLATFORM_IOS)
        RAAT__WARNING("buffer overrun. discarding %d samples at %lld fill=%d", dead_region->nsamples, dead_region->streamsample, self->sample_fill);
#endif
        self->sample_fill -= dead_region->nsamples;

        self->stats.overrun_count++;
        self->stats.samples_overrun += dead_region->nsamples;

        // remove dead region from the alive lists
        self->head = dead_region->next;
        self->head->prev = NULL;

        // add dead region to the dead regions list
        dead_region->next = self->dead_head;
        self->dead_head = dead_region;
    }

    // copy/clear dead_head, so we can free it outside of the lock
    dead_head = self->dead_head;
    self->dead_head = NULL;

    uv_mutex_unlock(&self->lock);

    // 3. Now that lock is released, free any dead regions to make space
    destroy_regions(self, dead_head);

    // 4. Allocate the new region, and fail with OOM if needed
    Region *region = alloc_region(self);
    if (region == NULL) return RC__STATUS_OUT_OF_MEMORY;

    int buf_size_bytes = RAAT__stream_format_compute_buffer_size(&self->format, nsamples);
    region->streamsample = streamsample;
    region->gain         = gain;
    region->peak         = peak;
    region->nsamples     = nsamples;
    region->buf          = alloc_databuf(self, buf_size_bytes);

    if (region->buf == NULL) {
        destroy_region(self, region);
        return RC__STATUS_OUT_OF_MEMORY;
    }
    memcpy(region->buf, buf, buf_size_bytes);

    // 5. Acquire the lock, then insert the new region into the doubly linked list
    uv_mutex_lock(&self->lock);
    bool inserted = false;

    self->stats.samples_in += nsamples;
    self->stats.write_count++;

    //RAAT__TRACE("in stream_write, writing %d samples at %lld", nsamples, streamsample);
    for (it = self->tail; it; it = it->prev) {
        if (it->streamsample < streamsample) {
            region->prev       = it;
            region->next       = it->next;
            it->next           = region;

            if (region->next != NULL) region->next->prev = region;      // insert in middle of list
            else                      self->tail         = region;      // insert at end of non-empty list

            inserted = true;
            break;
        }
    }

    if (!inserted) {
        if (!self->tail) {      
            self->head = self->tail = region;   // insert into empty list
        } else {               
            region->next = self->head;          // insert at front of non-empty list
            region->next->prev = region;
            self->head = region;
        }
    }

    self->sample_fill += nsamples;

    uv_cond_signal(&self->cond);

    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

static void ensure_metadata_capacity(RAAT__Stream *self, int i) {
    if (self->metadata_capacity >= i) return;

    if (self->metadata_capacity == 0) {
        self->metadata = RC__new0(self->alloc, RAAT__AudioPacketMetadata, 16);
        RC__ASSERT(self->metadata);
        self->metadata_capacity = 16;
    } else {
        self->metadata = RC__resize0(self->alloc, self->metadata, 
                                     sizeof(RAAT__AudioPacketMetadata)*self->metadata_capacity, 
                                     sizeof(RAAT__AudioPacketMetadata)*self->metadata_capacity*2);
        RC__ASSERT(self->metadata);
        self->metadata_capacity *= 2;
    }
}


RC_API RC__Status
RAAT__stream_consume_packet     (RAAT__Stream *self, RAAT__AudioPacket *out_packet) {
    uv_mutex_lock(&self->lock);

    Region *r = self->head;
    if (r == NULL) {
        uv_cond_wait(&self->cond, &self->lock);
        r = self->head;
        if (r == NULL) {
            uv_mutex_unlock(&self->lock);
            return RC__STATUS_CANCELED;
        }
    }

    out_packet->buf = r->buf; r->buf = NULL;    // transfer ownership of the buf to out_packet, who will destroy it later.
    out_packet->streamsample = r->streamsample;
    out_packet->nsamples     = r->nsamples;
    out_packet->gain         = r->gain;
    out_packet->peak         = r->peak;

    r = self->head;
    self->head = r->next;
    self->sample_fill -= r->nsamples;
    if (self->head) self->head->prev = NULL;
    else self->tail = NULL;
    r->next = self->dead_head;
    self->dead_head = r;

    uv_mutex_unlock(&self->lock);
    return RC__STATUS_SUCCESS;
}

RC_API void
RAAT__stream_destroy_packet      (RAAT__Stream *self, RAAT__AudioPacket *packet) {
    if (!packet) return;
    if (packet->buf) {
        int nbytes = RAAT__stream_format_compute_buffer_size(&self->format, packet->nsamples);
        free_databuf(self, packet->buf, nbytes);
    }
}

RC_API RC__Status
RAAT__stream_cancel_consume_packet (RAAT__Stream *self) {
    uv_mutex_lock(&self->lock);
    uv_cond_broadcast(&self->cond);
    uv_mutex_unlock(&self->lock);
    return RC__STATUS_SUCCESS;
}

RC_API RC__Status
RAAT__stream_read               (RAAT__Stream *self, int64_t streamsample, void *out_buf, int nsamples, RAAT__AudioPacketMetadata **out_metadata, size_t *out_nmetadata) {
    uv_mutex_lock(&self->lock);

    Region *r;

    // Discard packets that would have been played in the past
    while (self->head && streamsample > (self->head->streamsample + self->head->nsamples)) {
        r = self->head;
        self->head = r->next;
        self->sample_fill -= r->nsamples;
        if (self->head) self->head->prev = NULL;
        else (self->tail) = NULL;
        r->next = self->dead_head;
        self->dead_head = r;
    }

    int dropoutsamples = 0;

    int64_t read_cur = streamsample;
    int64_t read_end = streamsample+nsamples;

    int metadata_i = 0;

    //RAAT__TRACE("read at streamsample %lld read_cur %lld fill=%d", streamsample, read_cur, self->sample_fill);
    for (r = self->head; r; r = r->next) {
        if (r->streamsample > read_end) break;         // if we hit a packet that's past the end of our read, we're done
        int64_t r_end = r->streamsample + r->nsamples;

        int64_t copy_start_time = RC__max(read_cur, r->streamsample);
        int64_t copy_end_time   = RC__min(read_end, r_end);

        int copy_read_sample_off  = (int)(copy_start_time - r->streamsample);
        int copy_write_sample_off = (int)(copy_start_time - streamsample);
        int copy_samples          = (int)(copy_end_time   - copy_start_time);

        int copy_read_byte_off  = RAAT__stream_format_compute_buffer_size(&self->format, copy_read_sample_off);
        int copy_write_byte_off = RAAT__stream_format_compute_buffer_size(&self->format, copy_write_sample_off);
        int copy_bytes          = RAAT__stream_format_compute_buffer_size(&self->format, copy_samples);
    
        //RAAT__TRACE("    copy read_sample_off=%d write_sample_off=%d samples=%d at start_time=%d", copy_read_sample_off, copy_write_sample_off, copy_samples, read_cur);
        //RAAT__TRACE("         read_byte_off=%d write_byte_off=%d bytes=%d", copy_read_byte_off, copy_write_byte_off, copy_bytes);

        if (copy_start_time != read_cur) {              // dropout prior to the region we're copying from
            int zerofill_byte_off = RAAT__stream_format_compute_buffer_size(&self->format, read_cur - streamsample);
            int zerofill_samples = copy_start_time - read_cur;
            RAAT__stream_format_zero_fill(&self->format, (uint8_t*)out_buf + zerofill_byte_off, zerofill_samples);
#if !defined(PLATFORM_IOS)
            RAAT__WARNING("dropout of %d samples at %lld [1]", zerofill_samples, copy_start_time);
#endif
            dropoutsamples += zerofill_samples;
            self->stats.dropout_count++;
        }

        // update chunk metadata
        ensure_metadata_capacity(self, metadata_i+1);
        self->metadata[metadata_i].sample       = copy_write_sample_off;
        self->metadata[metadata_i].gain         = r->gain;
        self->metadata[metadata_i].peak         = r->peak;

        /*
        RAAT__TRACE("chunk %d metadata sample %d gain %f peak %f",
                     metadata_i,
                     self->metadata[metadata_i].sample       ,
                     self->metadata[metadata_i].gain         ,
                     self->metadata[metadata_i].peak         );
                     */

        metadata_i++;

        memcpy((uint8_t*)out_buf + copy_write_byte_off, (uint8_t*)r->buf + copy_read_byte_off, copy_bytes);

        read_cur = copy_end_time;

        if (read_cur == read_end) break;
    }

    // produce a dropout from here to the end of the buffer
    if (read_cur < read_end) {
        int zerofill_byte_off = RAAT__stream_format_compute_buffer_size(&self->format, nsamples - (read_end - read_cur));
        int zerofill_samples  = read_end - read_cur;
        RAAT__stream_format_zero_fill(&self->format, (uint8_t*)out_buf + zerofill_byte_off, zerofill_samples);
#if !defined(PLATFORM_IOS)
        RAAT__WARNING("dropout of %d samples at %lld [2]", zerofill_samples, read_cur);
#endif
        dropoutsamples += zerofill_samples;
        self->stats.dropout_count++;
    }

    self->stats.samples_out     += nsamples;
    self->stats.read_count++;
    self->stats.samples_dropped += dropoutsamples;

    // Discard packets that we no longer need
    while (self->head && streamsample + nsamples > (self->head->streamsample + self->head->nsamples)) {
        r = self->head;
        self->sample_fill -= r->nsamples;
        //RAAT__TRACE("discarded %d samples at %lld fill=%d", r->nsamples, r->streamsample, self->sample_fill);
        self->head = r->next;
        if (self->head) self->head->prev = NULL;
        else self->tail = NULL;
        r->next = self->dead_head;
        self->dead_head = r;
    }

    uv_mutex_unlock(&self->lock);

    if (out_metadata)  *out_metadata = self->metadata;
    if (out_nmetadata) *out_nmetadata = metadata_i;

    return RC__STATUS_SUCCESS;
}

RC_API void 
RAAT__stream_incref             (RAAT__Stream   *self) {
    RC__ASSERT(self != NULL);

    uv_mutex_lock(&self->lock);
    self->refcnt++;
    uv_mutex_unlock(&self->lock);
}

static void destroy(RAAT__Stream *self) {
    RC__ASSERT(self != NULL);
    uv_cond_destroy(&self->cond);
    uv_mutex_destroy(&self->lock);
    destroy_regions(self, self->head);
    destroy_regions(self, self->dead_head);
    RC__free(self->alloc, self->metadata);
    free_pools(self);
    uv_mutex_destroy(&self->pool_lock);
    RC__free(self->alloc, self);
}

RC_API void 
RAAT__stream_decref             (RAAT__Stream   *self) {
    RC__ASSERT(self != NULL);

    uv_mutex_lock(&self->lock);
    if (--self->refcnt == 0) {
        uv_mutex_unlock(&self->lock);
        destroy(self);
        return;
    }
    uv_mutex_unlock(&self->lock);
}

RC_API bool 
RAAT__stream_format_equals(const RAAT__StreamFormat *f1, const RAAT__StreamFormat *f2) {
    return f1->sample_type     == f2->sample_type &&
           f1->sample_rate     == f2->sample_rate && 
           f1->bits_per_sample == f2->bits_per_sample && 
           f1->channels        == f2->channels;
}

RC_API void 
RAAT__stream_format_zero_fill(RAAT__StreamFormat *format, void *buf, int nsamples) {
    int nbytes = RAAT__stream_format_compute_buffer_size(format, nsamples);
    if (format->sample_type == RAAT__SAMPLE_TYPE_DSD) {
        memset(buf, 0x69, nbytes);
    } else if (format->sample_type == RAAT__SAMPLE_TYPE_PCM) {
        memset(buf, 0x00, nbytes);
    } else {
        RC__ASSERT(false);
    }
}

RC_API int 
RAAT__stream_format_compute_buffer_size(const RAAT__StreamFormat *f, int nsamples) {
    if (f->sample_type == RAAT__SAMPLE_TYPE_DSD) {
        return (nsamples / 8) * f->channels;
    } else {
        return nsamples * f->channels * f->bits_per_sample / 8;
    }
}

RC_API RAAT__StreamFormat *
RAAT__stream_format             (RAAT__Stream *self) {
    RC__ASSERT(self != NULL);
    return &self->format;
}

RC_API void 
RAAT__stream_get_stats          (RAAT__Stream   *self, RAAT__StreamStats *out_stats) {
    uv_mutex_lock(&self->lock);
    self->stats.capacity_samples = self->sample_capacity;
    self->stats.fill_samples     = self->sample_fill;
    *out_stats = self->stats;
    uv_mutex_unlock(&self->lock);
}

RC_API 
void RAAT__stream_format_to_string(const RAAT__StreamFormat *format, char *buffer) {
    const char *sample_type;
    switch (format->sample_type) {
        case RAAT__SAMPLE_TYPE_PCM: sample_type = "pcm";     break;
        case RAAT__SAMPLE_TYPE_DSD: sample_type = "dsd";     break;
        default:                    sample_type = "invalid"; break;
    }
    snprintf(buffer, RAAT__STREAM_FORMAT_MAX_STRLEN, "%s %d/%d/%d", sample_type, format->sample_rate, format->bits_per_sample, format->channels);
}

RC_API int   
RAAT__stream_get_sample_capacity(RAAT__Stream *self) {
    RC__ASSERT(self);
    return self->sample_capacity;
}

RC_API 
int64_t RAAT__stream_format_samples_to_ns(const RAAT__StreamFormat *f, int64_t nsamples) {
    // NOTE: the intermediate value nsamples * 1000000000L easily overflows an int64_t, so we
    //       perform this calculation using a double precision float, and order the operations to avoid
    //       a very large intermediate result that would lose precision when crammed into a 53-bit mantissa.
    //
    //       This gives us nanosecond resolution clock values for up to ~145 years of continuous playback at DSD256. After 
    //       that, we start to lose bits of precision, which means we manage the clock at 2ns for the second ~145 years, and 
    //       so on. This is more than sufficient, since our real concerns are in the microsecond/millisecond domains.
    return (int64_t)((double)nsamples / (double)f->sample_rate * 1000000000.0);
}

RC_API 
int64_t RAAT__stream_format_ns_to_samples(const RAAT__StreamFormat *f, int64_t ns) {
    int64_t ret = ((int64_t)ns * f->sample_rate / 1000000000LL);
    if (f->sample_type == RAAT__SAMPLE_TYPE_DSD)           // make sure we're on an 8-sample boundary after this computation for DSD
        ret -= ret % 8;
    return ret;
}

RC_API 
void RAAT__stream_format_repack(const RAAT__StreamFormat *from, uint8_t *from_buf, const RAAT__StreamFormat *to, uint8_t *to_buf, int nsamples) {
    RC__ASSERT(from != NULL);
    RC__ASSERT(to   != NULL);
    RC__ASSERT(from->channels    == to->channels);
    RC__ASSERT(from->sample_type == to->sample_type);

    int from_stride  = from->bits_per_sample / 8;
    int to_stride    =   to->bits_per_sample / 8;
    int samplevalues = from->channels * nsamples;

    if (to_stride < from_stride) RC__ASSERT(0);

    if (from_stride == to_stride) {
        memcpy(to_buf, from_buf, from_stride * samplevalues);
        return;
    }

    int i;
    for (i = 0; i < samplevalues; i++) {
        switch (to_stride - from_stride) {
            case 2: *to_buf++ = 0;  /* fall through */
            case 1: *to_buf++ = 0;  /* fall through */
            case 0: break;
            default: RC__ASSERT(0); break;
        }
        switch (from->bits_per_sample) {
            case 32: *to_buf++ = *from_buf++; /* fall through */
            case 24: *to_buf++ = *from_buf++; /* fall through */
            case 16: *to_buf++ = *from_buf++; *to_buf++ = *from_buf++; break;
            default: RC__ASSERT(0); break;
        }
    }
}

 
