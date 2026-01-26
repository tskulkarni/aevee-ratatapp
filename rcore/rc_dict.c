//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "rc_dict.h"
#include "rc_guid.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>

static void rehash(RC__Dict *self);

static const RC__DictConfig direct_config =
{
    RC__dict_direct_hash,
    RC__dict_direct_equal,
    NULL,
    NULL
};

void RC__dict_init(RC__Dict *self, RC__Allocator *alloc, const RC__DictConfig *config)
{
    self->allocator = RC__allocator_default(alloc);
    self->tablesize = 0;
    self->table = NULL;
    if (!config)
        self->config = &direct_config;
    else
        self->config = config;
}

static void callvaluedestroy(RC__Dict *self, void *value)
{
    if (self->config->valuedestroy)
        self->config->valuedestroy(self->allocator, value);
}

static void callkeydestroy(RC__Dict *self, void *key)
{
    if (self->config->keydestroy)
        self->config->keydestroy(self->allocator, key);
}

void        
RC__dict_clear                     (RC__Dict *self)
{
    if (self->table) {
        if (self->config->keydestroy || self->config->valuedestroy) {
            size_t x;
            for (x = 0; x < self->tablesize * 2; x++) {
                if (self->table[x].key) {
                    callkeydestroy(self, self->table[x].key);
                    callvaluedestroy(self, self->table[x].value);
                }
            }
        }
        memset(self->table, 0, self->tablesize * 2 * sizeof(RC__DictEntry));
    }
}

void RC__dict_destroy(RC__Dict *self)
{
    if (self->table)
    {
        if (self->config->keydestroy || self->config->valuedestroy)
        {
            size_t x;
            for (x = 0; x < self->tablesize * 2; x++)
            {
                if (self->table[x].key)
                {
                    callkeydestroy(self, self->table[x].key);
                    callvaluedestroy(self, self->table[x].value);
                }
            }
        }
        RC__allocator_free(self->allocator, self->table);
        self->table = NULL;
    }
}

void *RC__dict_iterate(RC__Dict *self, void *iterator, void **key, void **value)
{
    size_t x = (size_t)iterator;
    for (; x < self->tablesize * 2; x++)
    {
        if (self->table[x].key)
        {
            *key = self->table[x].key;
            *value = self->table[x].value;
            return (void*)(x+1);
        }
    }
    return NULL;
}

size_t      
RC__dict_count                     (RC__Dict *self) {
    size_t ret = 0;
    size_t x;
    for (x = 0; x < self->tablesize * 2; x++)
    {
        if (self->table[x].key)
        {
            ret++;
        }
    }
    return ret;
}

void RC__dict_foreach(RC__Dict *self, RC__DictForeachCallback func, void *user_data)
{
    size_t x;
    for (x = 0; x < self->tablesize * 2; x++)
    {
        if (self->table[x].key)
        {
            if (func(self->table[x].key, self->table[x].value, user_data))
                return;
        }
    }
}

bool RC__dict_remove(RC__Dict *self, void *key)
{
    bool ret = false;
    if (self->table)
    {
        unsigned int hash1 = self->config->keyhash(key, 1) % self->tablesize;
        if (self->config->keyequal(self->table[hash1].key, key))
        {
            callkeydestroy(self, self->table[hash1].key);
            callvaluedestroy(self, self->table[hash1].value);
            self->table[hash1].key = NULL;
            ret = true;
        }
        else
        {
            unsigned int hash2 = self->config->keyhash(key, 2) % self->tablesize + self->tablesize;
            if (self->config->keyequal(self->table[hash2].key, key))
            {
                callkeydestroy(self, self->table[hash2].key);
                callvaluedestroy(self, self->table[hash2].value);
                self->table[hash2].key = NULL;
                ret = true;
            }
        }
    }
    return ret;
}

static void initial_alloc_table(RC__Dict *self)
{
    self->tablesize = RC__DICT_DEFAULT_TABLESIZE;
    self->table = RC__allocator_alloc(self->allocator, self->tablesize * 2 * sizeof(RC__DictEntry));
    memset(self->table, 0, self->tablesize * 2 * sizeof(RC__DictEntry));
}

static void rehash(RC__Dict *self)
{
    RC__DictEntry *oldtable = self->table;
    size_t oldtablesize = self->tablesize;
    size_t i;
    self->tablesize = self->tablesize * 2;
    self->table = RC__allocator_alloc(self->allocator, self->tablesize * 2 * sizeof(RC__DictEntry));
    memset(self->table, 0, self->tablesize * 2 * sizeof(RC__DictEntry));
    for (i = 0; i < oldtablesize * 2; i++)
    {
        if (oldtable[i].key)
        {
            RC__dict_insert(self, oldtable[i].key, oldtable[i].value);
        }
    }
    RC__allocator_free(self->allocator, oldtable);
}

void RC__dict_insert(RC__Dict *self, void *key, void *value)
{
    size_t i;
    if (!self->table)
        initial_alloc_table(self);
    for (i = 0; i < self->tablesize * 2; i++)
    {
        unsigned int hash1 = self->config->keyhash(key, 1) & (self->tablesize - 1);
        unsigned int hash2;
        if (!self->table[hash1].key || self->config->keyequal(self->table[hash1].key, key))
        {
            if (self->table[hash1].key)
            {
                callkeydestroy(self, self->table[hash1].key);
                callvaluedestroy(self, self->table[hash1].value);
            }
            self->table[hash1].key = key;
            self->table[hash1].value = value;
            return;
        }
        else
        {
            void *oldkey = self->table[hash1].key;
            void *oldval = self->table[hash1].value;
            self->table[hash1].key = key;
            self->table[hash1].value = value;
            key = oldkey;
            value = oldval;
        }
        hash2 = (self->config->keyhash(key, 2) & (self->tablesize-1))+ self->tablesize;
        if (!self->table[hash2].key || self->config->keyequal(self->table[hash2].key, key))
        {
            if (self->table[hash2].key)
            {
                callkeydestroy(self, self->table[hash2].key);
                callvaluedestroy(self, self->table[hash2].value);
            }
            self->table[hash2].key = key;
            self->table[hash2].value = value;
            return;
        }
        else
        {
            void *oldkey = self->table[hash2].key;
            void *oldval = self->table[hash2].value;
            self->table[hash2].key = key;
            self->table[hash2].value = value;
            key = oldkey;
            value = oldval;
        }
    }
    rehash(self);
    RC__dict_insert(self, key, value);
}

bool RC__dict_lookup_full(RC__Dict *self, void *key, void **value)
{
    void *retvalue = NULL;
    bool ret = false;
    if (self->table)
    {
        unsigned int hash1 = self->config->keyhash(key, 1) & (self->tablesize - 1);
        if (self->config->keyequal(self->table[hash1].key, key))
        {
            retvalue = self->table[hash1].value;
            ret = true;
        }
        else
        {
            int hash2 = (self->config->keyhash(key, 2) & (self->tablesize-1))+ self->tablesize;
            if (self->config->keyequal(self->table[hash2].key, key))
            {
                retvalue = self->table[hash2].value;
                ret = true;
            }
        }
    }
    if (value)
        *value = retvalue;
    return ret;
}

void *RC__dict_lookup(RC__Dict *self, void *key)
{
    void *value = NULL;
    RC__dict_lookup_full(self, key, &value);
    return value;
}

unsigned int RC__dict_strcase_hash(void *key, int algorithm)
{
    char *str = key;
    /* credit: http://www.cse.yorku.ca/~oz/hash.html */
    switch (algorithm)
    {
        case 1:     /* djb2 */
            {
                unsigned long hash = 5381;
                int c;
                while ((c = *str++))
                    hash = ((hash << 5) + hash) + tolower(c); /* hash * 33 + c */
                return hash;
            }
        case 2:     /* sdbm */
            {
                unsigned long hash = 0;
                int c;
                while ((c = *str++))
                    hash = tolower(c) + (hash << 6) + (hash << 16) - hash;
                return hash;
            }
        default:
            RC__ASSERT(0);
    }
    return 0;
}

unsigned int RC__dict_mem_hash(void *mem, size_t len, int algorithm)
{
    /* credit: http://www.cse.yorku.ca/~oz/hash.html */
    switch (algorithm)
    {
        case 1:     /* djb2 */
            {
                unsigned long hash = 5381;
                size_t i;
                for (i = 0; i < len; i++) {
                    int c = ((unsigned char*)mem)[i];
                    hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
                }
                return hash;
            }
        case 2:     /* sdbm */
            {
                unsigned long hash = 0;
                size_t i;
                for (i = 0; i < len; i++) {
                    int c = ((unsigned char*)mem)[i];
                    hash = c + (hash << 6) + (hash << 16) - hash;
                }
                return hash;
            }
        default:
            RC__ASSERT(0);
    }
    return 0;
}

bool 
RC__dict_direct_equal              (void *key1, void *key2)
{
    return key1 == key2;
}

unsigned int RC__dict_direct_hash(void *key, int algorithm)
{
    return RC__dict_mem_hash(&key, sizeof(key), algorithm);
}

unsigned int RC__dict_str_hash(void *key, int algorithm)
{
    char *str = key;
    /* credit: http://www.cse.yorku.ca/~oz/hash.html */
    switch (algorithm)
    {
        case 1:     /* djb2 */
            {
                unsigned long hash = 5381;
                int c;
                while ((c = *str++))
                    hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
                return hash;
            }
        case 2:     /* sdbm */
            {
                unsigned long hash = 0;
                int c;
                while ((c = *str++))
                    hash = c + (hash << 6) + (hash << 16) - hash;
                return hash;
            }
        default:
            RC__ASSERT(0);
    }
    return 0;
}

bool RC__dict_strcase_equal(void *key1, void *key2)
{
    return key1 && key2 && !strcasecmp((char*)key1, (char*)key2);
}

bool RC__dict_str_equal(void *key1, void *key2)
{
    return key1 && key2 && !strcmp((char*)key1, (char*)key2);
}

void RC__dict_str_destroy(RC__Allocator *alloc, void *key)
{
    RC__allocator_free(alloc, key);
}

bool
RC__dict_guid_equal(void *key1, void *key2)
{
    return key1 && key2 && RC__guid_equals((RC__Guid*)key1, (RC__Guid*)key2);
}

unsigned int 
RC__dict_guid_hash(void *key, int algorithm)
{
    RC__Guid *self = (RC__Guid*)key;
    char buf[16];
    RC__guid_tobytes(self, buf);
    return RC__dict_mem_hash(buf, 16, algorithm);
}
