/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __LINKEDLIST_H__
#define __LINKEDLIST_H__

#define LIST	struct __listhead

struct __listhead
{
	struct __listhead* prev;
	struct __listhead* next;
};

#define LIST_DECLARE(var) \
	struct __listhead var

#define LISTABLE_TYPE(type, fields) \
typedef struct __ ## type \
{ \
	LIST_DECLARE(__ ## type ## _listhead); \
	fields \
} \
type


#define LISTABLE_STRUCT(type, fields) \
struct type \
{ \
	LIST_DECLARE(__ ## type ## _listhead); \
	fields \
}


#define LIST_DECLARE_STATIC(var) \
	static struct __listhead var

#define LIST_INIT(list) \
	((struct __listhead*)(list))->next = list; \
	((struct __listhead*)(list))->prev = list

#define LIST_EMPTY(list) (((struct __listhead*)(list))->next == (list))

#define LIST_INSERT(iitem, iprev, inext) \
{ \
	struct __listhead* __prev = (struct __listhead*) iprev; \
	struct __listhead* __next = (struct __listhead*) inext; \
	__next->prev = (struct __listhead*)(iitem); \
	((struct __listhead*)(iitem))->next = (struct __listhead*)(__next); \
	((struct __listhead*)(iitem))->prev = (struct __listhead*)(__prev); \
	__prev->next = (struct __listhead*)(iitem); \
}

#define LIST_ADD(list, item) \
	LIST_INSERT(item, list, (list)->next)

#define LIST_APPEND(list, item) \
	LIST_INSERT(item, (list)->prev, list)

#define LIST_REMOVE(item) \
{ \
        struct __listhead *prev = ((struct __listhead*)(item))->prev; \
        struct __listhead *next = ((struct __listhead*)(item))->next; \
        next->prev = prev; \
        prev->next = next; \
}

#define LIST_ISNULL(list, item) (((struct __listhead*)(item)) == (list))
#define LIST_TAIL(type, list) ((type) (LIST_EMPTY((list)) ? NULL : ((struct __listhead*)(list))->prev))
#define LIST_NEXT(type, item) ((type) ((struct __listhead*)(item))->next)
#define LIST_PREV(type, item) ((type) ((struct __listhead*)(item))->prev)


#define LIST_FOREACH(type, ivar, list) \
	for (ivar = LIST_NEXT(type, list); ((struct __listhead*) ivar) != list; ivar = LIST_NEXT(type, ivar))

#define LIST_FOREACH_SAFE(type, var, list, codeblock) \
{ \
	struct __listhead* __next; \
	for (var = LIST_NEXT(type, list); ((struct __listhead*) var) != list; var = (type) __next) { \
		__next = (struct __listhead*) LIST_NEXT(type, var); \
		codeblock \
	} \
}

static inline size_t
LIST_SIZE(struct __listhead* list)
{
	size_t sz = 0;
	struct __listhead *ent;
	LIST_FOREACH(struct __listhead*, ent, list) {
		sz++;
	}
	return sz;
}

#endif

