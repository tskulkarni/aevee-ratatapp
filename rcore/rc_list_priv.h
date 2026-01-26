//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#ifndef INCLUDED_SOOC_LIST_INLINE 
#define INCLUDED_SOOC_LIST_INLINE

/*
 * Private implementation details
 *
 * Everything in this file is subject to change at any time.
 */
struct RC__ListNode_s
{
    struct RC__ListNode_s *next;
    struct RC__ListNode_s *prev;
    void *data;
};

struct RC__List_s
{
    RC__ListNode *head;
    RC__ListNode *tail;
    size_t length;
    RC__Allocator *allocator;
};

static inline RC__ListIter RC__list_begin(RC__List *self)
{
    return self->head;
}

static inline RC__ListIter RC__list_end(RC__List *self)
{
    (void)self;
    return NULL;
}

static inline RC__ListIter RC__listiter_next(RC__ListIter iter)
{
    RC__ASSERT(iter);
    return iter->next;
}

static inline void *RC__listiter_data(RC__ListIter iter)
{
    return iter->data;
}


static inline size_t RC__list_length(RC__List *self)
{
    return self->length;
}

#endif
