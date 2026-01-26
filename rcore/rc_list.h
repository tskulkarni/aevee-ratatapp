//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_RC_LIST_H
#define INCLUDED_RC_LIST_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Allocator-aware Doubly linked list implementation
 *
 * Provides O(1) append, prepend, remove, length and
 * some common functional operators. Also supports
 * inline iteration with no function call overhead when inlining
 * is enabled.
 */

#include "rc_base.h"
#include "rc_allocator.h"

/* === public api === */

/* --- typedefs for private structures --- */
typedef struct RC__ListNode_s RC__ListNode;
typedef struct RC__ListNode_s *RC__ListIter;
typedef struct RC__List_s RC__List;

/* --- typedefs for callback functions --- */

/*
 * Callback for RC__list_foreach
 *
 * return RC__TRUE to stop iteration, RC__FALSE to continue
 * iteration
 */
typedef bool (*RC__ListForeachCallback)(void *data, void *user_data);

/*
 * Callback for RC__list_map and RC__list_map_copy
 */
typedef void *(*RC__ListMapCallback)(void *data, void *user_data);

/*
 * Callback for RC__list_filter and RC__list_filter_copy
 *
 * return RC__TRUE to keep an element in the output list,
 * RC__FALSE otherwise.
 */
typedef bool (*RC__ListPredicateCallback)(void *data, void *user_data);

/*
 * Callback for RC__list_foreach_destroy
 */
typedef void RC__ListDestroyNodeCallback(void *data, void *user_data);

/* --- functions --- */

/*
 * Initialize a RC__List.
 *
 * The alloc param is used for node allocation
 */
void RC__list_init(RC__List *self, RC__Allocator *alloc);

/*
 * Destroy a list
 */
void RC__list_destroy(RC__List *self);

/*
 * Return the length of the list
 *
 * Guaranteed to be O(1)
 */
static inline size_t RC__list_length(RC__List *self);

/*
 */
void RC__list_foreach_remove(RC__List *self, RC__ListDestroyNodeCallback cb, void *user_data);

/*
 * Destroy a list, calling cb(node data, user_data) for each node
 * to give a chance to clean up the data.
 */
void RC__list_foreach_destroy(RC__List *self, RC__ListDestroyNodeCallback cb, void *user_data);

/*
 * Append a data to the end of a list
 *
 * Guaranteed to be O(1)
 */
void RC__list_push(RC__List *self, void *data);

/*
 * Append data before the provided element
 *
 * Guaranteed to be O(1)
 */
void RC__list_insert_before(RC__List *self, RC__ListIter it, void *data);

/*
 * Append data after the provided element
 *
 * Guaranteed to be O(1)
 */
void RC__list_insert_after(RC__List *self, RC__ListIter it, void *data);

/*
 * Pop data off of the end of the list and returns it.
 *
 * Guaranteed to be O(1)
 */
void *RC__list_pop(RC__List *self);

/*
 * Pop data off of the end of the list and returns it.
 *
 * Guaranteed to be O(1)
 */
bool RC__list_pop_full(RC__List *self, void **out_data);

/*
 * Shift data off of the end of the list and returns it.
 *
 * Guaranteed to be O(1)
 */
void *RC__list_shift(RC__List *self);

/*
 * Shift data off of the end of the list and returns it.
 *
 * Guaranteed to be O(1)
 */
bool RC__list_shift_full(RC__List *self, void **out_data);

/*
 * Prepend a data to the start of a list
 *
 * Guaranteed to be O(1)
 */
void RC__list_prepend(RC__List *self, void *data);

/*
 * Remove the first node for which data == node data
 */
bool RC__list_remove_by_data(RC__List *self, void *data);

/*
 * Remove a node from the list by its iterator.
 *
 * Guaranteed to be O(1)
 */
void RC__list_remove(RC__List *self, RC__ListIter iter);

/* Reverse a list in place
 *
 * Guaranteed to be O(n) where n == list length
 */
void RC__list_reverse(RC__List *self);

/*
 * get the data from the nth element of the list, or NULL if out of bounds
 */
void *RC__list_get_nth_data(RC__List *self, int n);


RC__ListIter RC__list_find(RC__List *self, void *data);

/*
 * Call cb(data, user_data) for each data in the list
 */
void RC__list_foreach(RC__List *self, RC__ListForeachCallback cb, void *user_data);

/*
 * Return true if func(data, user_data) returns true for any data
 * in the list
 *
 * This is not guaranteed to traverse the whole list
 */
bool RC__list_contains(RC__List *self, RC__ListPredicateCallback func, void *user_data);

/*
 * Replace each data in the list with the value returned by
 * func(data, user_data)
 */
void RC__list_map(RC__List *self, RC__ListMapCallback func, void *user_data);

/*
 * Remove all data from the list for which func(data, user_data) == RC__FALSE
 */
void RC__list_filter(RC__List *self, RC__ListPredicateCallback func, void *user_data);

/*
 * Copy all elements in list into out_list
 *
 * Invariant: out_list is initialized and empty. Nodes will
 * be allocated with the allocator from out_list
 */
void RC__list_copy(RC__List *self, RC__List *out_list);

/*
 * For each data in list, append func(data, user_data) to out_list
 *
 * Invariant: out_list is initialized and empty. Nodes will
 * be allocated with the allocator from out_list
 */
void RC__list_map_copy(RC__List *self, RC__List *out_list, RC__ListMapCallback func, void *user_data);

/*
 * For each data in list append it to out_list if func(data, user_data) == RC__TRUE
 *
 * Invariant: out_list is initialized and empty. Nodes will
 * be allocated with the allocator from out_list
 */
void RC__list_filter_copy(RC__List *self, RC__List *out_list, RC__ListPredicateCallback func, void *user_data);

/* list iteration example:
 *
 * RC__ListIter *i = RC__list_begin(list);
 *
 * for (; i != RC__list_end(list); i = RC__listiter_next(i))
 * {
 *    void *data = RC__listiter_data(i);
 *    ...
 * }
 * */

/*
 * Return an iterator representing the beginning of the list
 */
static inline RC__ListIter RC__list_begin(RC__List *self);

/*
 * Return an iterator representing the end of the list
 */
static inline RC__ListIter RC__list_end(RC__List *self);

/*
 * Given an iterator that is not equal to RC__list_end,
 * return an iterator representing the next item in the list
 */
static inline RC__ListIter RC__listiter_next(RC__ListIter iter);

/*
 * Retrieve the data associated with a list iterator
 */
static inline void *RC__listiter_data(RC__ListIter iter);

#include "rc_list_priv.h"

#ifdef __cplusplus
}
#endif

#endif
