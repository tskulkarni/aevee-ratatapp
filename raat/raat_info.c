//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "raat_info.h"

#include "rc_base.h"
#include "rc_dict.h"
#include "rc_status.h"
#include "raat_base.h"

#include <stdlib.h>
#include <string.h>

struct RAAT__Info_s {
    RC__Allocator       *alloc;
    RAAT__Log           *log;
    uv_mutex_t           lock;
    RC__Dict             dict;
};

#define RAAT__CURRENT_LOG self->log

static RC__DictConfig info_dict_config = {
    RC__dict_str_hash,
    RC__dict_str_equal,
    RC__dict_str_destroy,
    RC__dict_str_destroy
};

RC_API RC__Status
RAAT__info_new                 (RC__Allocator *alloc, RAAT__Log *log, RAAT__Info **out_self) 
{
    RAAT__static_init();

    RC__Status status;
    RAAT__Info *self     = NULL;

    RC__ASSERT(out_self != NULL);
    *out_self = NULL;

    char protoversionbuf[20];

    sprintf(protoversionbuf, "%d", RAAT__PROTOCOL_VERSION);

    self = RC__new0(alloc, RAAT__Info, 1);
    if (self == NULL) {
        return RC__STATUS_OUT_OF_MEMORY;
    }

    uv_mutex_init(&self->lock);
    self->alloc = RC__allocator_default(alloc);
    RC__dict_init(&self->dict, alloc, &info_dict_config);
    self->log = log;

    RAAT__TRACE("[info] initializing info dictionary");

    status = RAAT__info_set(self, RAAT__INFO_KEY_RAAT_VERSION, RAAT__VERSION);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        goto fail;
    }

    status = RAAT__info_set(self, RAAT__INFO_KEY_PROTOCOL_VERSION, protoversionbuf);
    if (!RC__STATUS_IS_SUCCESS(status)) {
        goto fail;
    }

    *out_self = self;

    return RC__STATUS_SUCCESS;
fail:
    if (self) {
        uv_mutex_destroy(&self->lock);
        RC__dict_destroy(&self->dict);
    }
    return status;
}

RC_API void 
RAAT__info_delete             (RAAT__Info   *self) 
{
    if (!self) return;
    uv_mutex_destroy(&self->lock);
    RC__dict_destroy(&self->dict);
    RC__free(self->alloc, self);
}

static bool is_known_key(const char *k) {
    return !strcmp(RAAT__INFO_KEY_VENDOR,           k) ||
           !strcmp(RAAT__INFO_KEY_OUTPUT_NAME,      k) ||
           !strcmp(RAAT__INFO_KEY_UNIQUE_ID,        k) ||
           !strcmp(RAAT__INFO_KEY_SERIAL,           k) ||
           !strcmp(RAAT__INFO_KEY_CONFIG_URL,       k) ||
           !strcmp(RAAT__INFO_KEY_MODEL,            k) ||
           !strcmp(RAAT__INFO_KEY_VENDOR_MODEL,     k) ||
           !strcmp(RAAT__INFO_KEY_AUTO_NAME,        k) ||
           !strcmp(RAAT__INFO_KEY_PROTOCOL_VERSION, k) ||
           !strcmp(RAAT__INFO_KEY_RAAT_VERSION,     k) ||
           !strcmp(RAAT__INFO_KEY_VERSION,          k);
}

RC_API RC__Status
RAAT__info_set                 (RAAT__Info *self, const char *key, const char *val) {
    size_t key_len;
    size_t val_len;

    RC__ASSERT(self != NULL);
    RC__ASSERT(key != NULL);
    RC__ASSERT(val != NULL);
    

    key_len = strlen(key);
    val_len = strlen(val);

    if (key_len + 1 > RAAT__INFO_MAX_KEY_LEN) {
        return RAAT__INFO_SET_STATUS_INVALID_KEY;
    }
    if (val_len + 1 > RAAT__INFO_MAX_VALUE_LEN) {
        return RAAT__INFO_SET_STATUS_INVALID_VALUE;
    }

    if (!is_known_key(key)) {
        if (key_len < 2 || key[0] != '_')       /* require a _ prefix on custom keys */
            return RAAT__INFO_SET_STATUS_INVALID_KEY;
    }

    key = RC__allocator_strdup(self->alloc, key);
    if (key == NULL) return RC__STATUS_OUT_OF_MEMORY;

    val = RC__allocator_strdup(self->alloc, val);
    if (val == NULL) {
        RC__free(self->alloc, key);
        return RC__STATUS_OUT_OF_MEMORY;
    }

    uv_mutex_lock(&self->lock);
    // OUTOFMEM: memory allocations in RC__dict_insert fail silently
    RC__dict_insert(&self->dict, (void*)key, (void*)val);
    RAAT__TRACE("[info] inserting %s -> %s", key, val);
    uv_mutex_unlock(&self->lock);

    return RC__STATUS_SUCCESS;
}

RC_API bool        
RAAT__info_try_get_value       (RAAT__Info *self, const char *key, char *out_value/*[RAAT__INFO_MAX_VALUE_LEN]*/) {
    char *value;
    bool ret;

    RC__ASSERT(self      != NULL);
    RC__ASSERT(key       != NULL);
    RC__ASSERT(out_value != NULL);

    uv_mutex_lock(&self->lock);
    ret = RC__dict_lookup_full(&self->dict, (char*)key, (void**)&value);
    if (ret) strncpy(out_value, value, RAAT__INFO_MAX_VALUE_LEN);
    uv_mutex_unlock(&self->lock);

    return ret;
}

static bool
verify_nonempty(const char* s) {
    return s && *s;
}

RC_API bool
RAAT__info_validate            (RAAT__Info *self) {
    bool ok = true;

    RC__ASSERT(self != NULL);

    uv_mutex_lock(&self->lock);
    if (!RC__dict_lookup(&self->dict, RAAT__INFO_KEY_VERSION))                    ok = false;
    if (!RC__dict_lookup(&self->dict, RAAT__INFO_KEY_MODEL))                      ok = false;
    if (!RC__dict_lookup(&self->dict, RAAT__INFO_KEY_VENDOR))                     ok = false;
    if (!verify_nonempty(RC__dict_lookup(&self->dict, RAAT__INFO_KEY_UNIQUE_ID))) ok = false;
    if (!RC__dict_lookup(&self->dict, RAAT__INFO_KEY_PROTOCOL_VERSION))           ok = false;
    if (!RC__dict_lookup(&self->dict, RAAT__INFO_KEY_RAAT_VERSION))               ok = false;
    uv_mutex_unlock(&self->lock);

    return ok;
}

RC_API void
RAAT__info_foreach             (RAAT__Info *self, RAAT__InfoForeachCallback cb, void *userdata) {
    RC__ASSERT(self != NULL);
    RC__ASSERT(cb != NULL);

    RC__dict_foreach(&self->dict, (RC__DictForeachCallback)cb, userdata);
}

const char * RAAT__info_status_to_string(RC__Status status) {
    RC__ASSERT(status >= RAAT__INFO_STATUS_BASE && status <= RAAT__INFO_STATUS_MAX);

    switch (status) {
        case RAAT__INFO_SET_STATUS_INVALID_VALUE: return "RAAT__INFO_SET_STATUS_INVALID_VALUE";
        case RAAT__INFO_SET_STATUS_INVALID_KEY: return "RAAT__INFO_SET_STATUS_INVALID_KEY";
        default: RC__ASSERT(0); return NULL;
    }
    return NULL;
}
