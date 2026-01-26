//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#include "rc_list.h"

void RC__list_init(RC__List *self, RC__Allocator *alloc)
{
    self->head = self->tail = NULL;
    self->allocator = alloc;
    self->length = 0;
}

void RC__list_destroy(RC__List *self)
{
    RC__list_foreach_destroy(self, NULL, NULL);
}

bool RC__list_pop_full(RC__List *self, void **out_data) {
    RC__ASSERT(self != NULL);
    RC__ASSERT(out_data != NULL);

    *out_data = NULL;
    if (self->tail == NULL) return false;
    *out_data = self->tail->data;
    RC__list_remove(self, self->tail);
    return true;
}

bool RC__list_shift_full(RC__List *self, void **out_data) {
    *out_data = NULL;
    if (self->head == NULL) return false;
    *out_data = self->head->data;
    RC__list_remove(self, self->head);
    return true;
}

void *RC__list_pop(RC__List *self) {
    void *ret = NULL;
    RC__list_pop_full(self, &ret);
    return ret;
}

void *RC__list_shift(RC__List *self) {
    void *ret = NULL;
    RC__list_shift_full(self, &ret);
    return ret;
}

void RC__list_push(RC__List *self, void *data)
{
    RC__ListNode *node = RC__allocator_alloc(self->allocator, sizeof(RC__ListNode));
    node->data = data;
    node->next = NULL;
    node->prev = self->tail;
    if (self->tail)
        self->tail->next = node;
    else
        self->head = node;
    self->tail = node;
    self->length++;
}

void RC__list_insert_before(RC__List *self, RC__ListIter it, void *data) {
    RC__ListNode *node = RC__allocator_alloc(self->allocator, sizeof(RC__ListNode));
    node->data = data;
    node->next = it;
    node->prev = it == NULL ? self->tail : it->prev;

    if (node->next) node->next->prev = node;
    else            self->tail = node;

    if (node->prev) node->prev->next = node;
     else           self->head = node;
}

void RC__list_insert_after(RC__List *self, RC__ListIter it, void *data) {
    RC__ListNode *node = RC__allocator_alloc(self->allocator, sizeof(RC__ListNode));
    node->data = data;
    node->prev = it;
    node->next = it == NULL ? self->head : it->next;

    if (node->next) node->next->prev = node;
    else            self->tail = node;

    if (node->prev) node->prev->next = node;
     else           self->head = node;
}

void RC__list_prepend(RC__List *self, void *data)
{
    RC__ListNode *node = RC__allocator_alloc(self->allocator, sizeof(RC__ListNode));
    node->data = data;
    node->prev = NULL;
    node->next = self->head;
    if (self->head)
        self->head->prev = node;
    else
        self->tail = node;
    self->head = node;
    self->length++;
}

void RC__list_foreach_remove(RC__List *self, RC__ListDestroyNodeCallback cb, void *user_data)
{
    RC__ListIter i = RC__list_begin(self);
    while (i != RC__list_end(self))
    {
        RC__ListIter tmp = RC__listiter_next(i);
        if ((void*)cb)
            cb(RC__listiter_data(i), user_data);
        RC__allocator_free(self->allocator, i);
        i = tmp;
    }
    self->head = self->tail = NULL;
}

void RC__list_foreach_destroy(RC__List *self, RC__ListDestroyNodeCallback cb, void *user_data)
{
    RC__ListIter i = RC__list_begin(self);
    while (i != RC__list_end(self))
    {
        RC__ListIter tmp = RC__listiter_next(i);
        if ((void*)cb)
            cb(RC__listiter_data(i), user_data);
        RC__allocator_free(self->allocator, i);
        i = tmp;
    }
    self->allocator = NULL;
    self->head = self->tail = NULL;
}

void RC__list_reverse(RC__List *self)
{
    RC__ListIter i = RC__list_begin(self);
    RC__ListNode *prev = NULL;
    RC__ListNode *oldhead = self->head;
    if (!self->head) return;
    while (i != RC__list_end(self))
    {
        RC__ListIter tmp = RC__listiter_next(i);
        i->next = prev;
        i->prev = tmp;
        prev = i;
        i = tmp;
    }
    self->head = self->tail;
    self->tail = oldhead;
}

void RC__list_foreach(RC__List *self, RC__ListForeachCallback cb, void *user_data)
{
    RC__ListIter i = RC__list_begin(self);
    while (i != RC__list_end(self))
    {
        RC__ListIter tmp = RC__listiter_next(i);
        if (cb(RC__listiter_data(i), user_data))
            return;
        i = tmp;
    }
}

void *RC__list_get_nth_data(RC__List *self, int n)
{
    int idx = 0;
    RC__ListIter i = RC__list_begin(self);
    while (i != RC__list_end(self))
    {
        if (idx++ == n) return RC__listiter_data(i);
        i = RC__listiter_next(i);
    }
    return NULL;
}

void RC__list_remove(RC__List *self, RC__ListIter iter)
{
    if (self->head == iter)
        self->head = iter->next;
    else
        iter->prev->next = iter->next;
    if (self->tail == iter)
        self->tail = iter->prev;
    else
        iter->next->prev = iter->prev;
    self->length--;

    RC__allocator_free(self->allocator, iter);
}


/* --- Functions Implemented against the public API --- */

RC__ListIter RC__list_find(RC__List *self, void *data)
{
    RC__ListIter i = RC__list_begin(self);
    while (i != RC__list_end(self))
    {
        if (RC__listiter_data(i) == data)
            return i;
        i = RC__listiter_next(i);
    }
    return RC__list_end(self);
}

bool RC__list_contains(RC__List *self, RC__ListPredicateCallback func, void *user_data)
{
    RC__ListIter i = RC__list_begin(self);
    while (i != RC__list_end(self))
    {
        if (func(RC__listiter_data(i), user_data))
            return true;
        i = RC__listiter_next(i);
    }
    return false;
}

void RC__list_map(RC__List *self, RC__ListMapCallback func, void *user_data)
{
    RC__ListIter i = RC__list_begin(self);
    while (i != RC__list_end(self))
    {
        i->data = func(RC__listiter_data(i), user_data);
        i = RC__listiter_next(i);
    }
}

void RC__list_filter(RC__List *self, RC__ListPredicateCallback func, void *user_data)
{
    RC__ListIter i = RC__list_begin(self);
    while (i != RC__list_end(self))
    {
        RC__ListIter tmp = RC__listiter_next(i);
        if (!func(RC__listiter_data(i), user_data))
            RC__list_remove(self, i);
        i = tmp;
    }
}

void RC__list_copy(RC__List *self, RC__List *out_list)
{
    RC__ListIter i = RC__list_begin(self);
    while (i != RC__list_end(self))
    {
        RC__ListIter tmp = RC__listiter_next(i);
        RC__list_push(out_list, RC__listiter_data(i));
        i = tmp;
    }
}

void RC__list_map_copy(RC__List *self, RC__List *out_list, RC__ListMapCallback func, void *user_data)
{
    RC__ListIter i = RC__list_begin(self);
    while (i != RC__list_end(self))
    {
        RC__list_push(out_list, func(RC__listiter_data(i), user_data));
        i = RC__listiter_next(i);
    }
}

void RC__list_filter_copy(RC__List *self, RC__List *out_list, RC__ListPredicateCallback func, void *user_data)
{
    RC__ListIter i = RC__list_begin(self);
    while (i != RC__list_end(self))
    {
        RC__ListIter tmp = RC__listiter_next(i);
        if (func(RC__listiter_data(i), user_data))
            RC__list_push(out_list, RC__listiter_data(i));
        i = tmp;
    }
}

bool RC__list_remove_by_data(RC__List *self, void *data)
{
    RC__ListIter i = RC__list_begin(self);
    while (i != RC__list_end(self))
    {
        RC__ListIter tmp = RC__listiter_next(i);
        if (RC__listiter_data(i) == data)
        {
            RC__list_remove(self, i);
            return true;
        }
        i = tmp;
    }
    return false;
}
