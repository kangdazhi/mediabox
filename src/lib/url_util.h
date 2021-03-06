/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __MB_URL_UTIL_H__
#define __MB_URL_UTIL_H__


void
urldecode(char *dst, const char *src);


/**
 * mb_url_fetch_to_mem() -- Fetch to content of a url to a
 * malloc() allocated buffer
 */
int
mb_url_fetch2mem(char *url, void **dest, size_t *size);


#endif
