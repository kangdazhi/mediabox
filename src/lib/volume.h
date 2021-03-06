/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __ALSA_VOLUME_H__
#define __ALSA_VOLUME_H__
#include "dispatch.h"


int
avbox_volume_get(void);


int
avbox_volume_set(int volume);


int
avbox_volume_init(struct avbox_object *msgobj);


void
avbox_volume_shutdown(void);

#endif
