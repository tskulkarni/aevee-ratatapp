// // The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RC_DICT_H
#define INCLUDED_RC_DICT_H

/*
 * Dictionary data structure based on cuckoo hash. This allows
 * amortized constant time inserts and guaranteed constant
 * time lookups.
 *
 * if you pass a null config object, a default config object is
 * used that assumes that hashes on the pointer value of the key
 * and compares them directly.
 *
 * The current implementation takes up 16 bytes per hashtable
 * and 8 bytes per table entry on 32 bit architectures. Maintaining
 * this for memory compactness is important. Additionally, the table
 * isn't allocated until the first insert. This should limit the cost
 * of an empty hashtable to the 16 bytes required for the RC__Dict struct.
 */

#include "rc_base.h"
#include "rc_allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RC__dict_internal RC__Dict;

#ifndef RC__DICT_DEFAULT_TABLESIZE
#define RC__DICT_DEFAULT_TABLESIZE   8
#endif

typedef unsigned int 
(*RC__KeyHashCallback)            (void *key, 
                                     int algorithm);

typedef bool 
(*RC__KeyEqualsCallback)          (void *key1, 
                                     void *key2);

typedef void 
(*RC__KeyValueDestroyCallback)    (RC__Allocator *dict_alloc, 
                                     void *key);

/* for iteration. return TRUE to stop iterating */
typedef bool 
(*RC__DictForeachCallback)          (void *key, 
                                     void *value, 
                                     void *user_data);

typedef struct {
    RC__KeyHashCallback              keyhash;
    RC__KeyEqualsCallback            keyequal;
    RC__KeyValueDestroyCallback    keydestroy;
    RC__KeyValueDestroyCallback    valuedestroy;
} RC__DictConfig;

void        
RC__dict_init                      (RC__Dict *dict,
                                     RC__Allocator *alloc,
                                     const RC__DictConfig *config);
void        
RC__dict_clear                     (RC__Dict *dict);

void        
RC__dict_destroy                   (RC__Dict *dict);

bool    
RC__dict_remove                    (RC__Dict *dict,
                                     void *key);

void        
RC__dict_insert                    (RC__Dict *dict,
                                     void *key,
                                     void *value);

void *      
RC__dict_lookup                    (RC__Dict *dict,
                                     void *key);

size_t      
RC__dict_count                     (RC__Dict *dict);

bool    
RC__dict_lookup_full               (RC__Dict *dict,
                                     void *key,
                                     void **value);

void        
RC__dict_foreach                   (RC__Dict *dict,
                                     RC__DictForeachCallback func,
                                     void *user_data);

void *
RC__dict_iterate                   (RC__Dict *dict,
                                     void *iterator,
                                     void **key,
                                     void **value);

/* --- convenience functions --- */
unsigned int 
RC__dict_str_hash                  (void *key, 
                                     int algorithm);

bool 
RC__dict_str_equal                 (void *key1, 
                                     void *key2);

unsigned int 
RC__dict_direct_hash               (void *key, 
                                     int algorithm);

bool 
RC__dict_direct_equal              (void *key1, 
                                     void *key2);

unsigned int 
RC__dict_strcase_hash              (void *key, 
                                     int algorithm);

bool 
RC__dict_strcase_equal             (void *key1, 
                                     void *key2);

void 
RC__dict_str_destroy               (RC__Allocator *alloc, 
                                     void *key);

/* 
 * helper for writing hash functions. Produces a hash
 * value for a block of memory.
 */
unsigned int 
RC__dict_mem_hash                  (void *mem, 
                                     size_t len,
                                     int algorithm); 

bool
RC__dict_guid_equal                (void *key1, 
                                     void *key2);
unsigned int 
RC__dict_guid_hash                 (void *key, 
                                     int algorithm);

/** \internal */
typedef struct {
    void *key;
    void *value;
} RC__DictEntry;

/** \internal */
struct RC__dict_internal {
    RC__Allocator *allocator;
    const RC__DictConfig *config;
    RC__DictEntry * table;
    size_t tablesize;      /* tablesize is 1/2 of the length
                            * of table. t1 lives in the first
                            * half and t2 in the second half */
};

#ifdef __cplusplus
}
#endif

#endif
