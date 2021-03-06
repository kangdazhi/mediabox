/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifndef __MB_PLAYER__
#define __MB_PLAYER__

#include "video.h"
#include "input.h"
#include "../linkedlist.h"
#include "../dispatch.h"


struct avbox_player;


LISTABLE_STRUCT(avbox_playlist_item,
	const char *filepath;
);


/**
 * Media player status enum.
 */
enum avbox_player_status
{
	MB_PLAYER_STATUS_READY,
	MB_PLAYER_STATUS_BUFFERING,
	MB_PLAYER_STATUS_PLAYING,
	MB_PLAYER_STATUS_PAUSED
};


/**
 * Status notification structure.
 */
struct avbox_player_status_data
{
	struct avbox_player *sender;
	enum avbox_player_status last_status;
	enum avbox_player_status status;
};


/* status changed callback function */
typedef void (*avbox_player_status_callback)(struct avbox_player *inst,
	enum avbox_player_status status, enum avbox_player_status last_status);

/**
 * Subscribe to receive player notifications.
 */
int
avbox_player_subscribe(struct avbox_player * const inst,
	struct avbox_object * const object);


/**
 * Unsubscribe from player events.
 */
int
avbox_player_unsubscribe(struct avbox_player * const inst,
	struct avbox_object * const object);


/**
 * Plays a list of items
 */
int
avbox_player_playlist(struct avbox_player* inst, LIST *playlist, struct avbox_playlist_item* item);


/**
 * Get the current status of a media player instance.
 */
enum avbox_player_status
avbox_player_getstatus(struct avbox_player* inst);


/**
 * Get the last played file
 */
const char *
avbox_player_getmediafile(struct avbox_player *inst);


/**
 * Gets the title of the currently playing
 * media file or NULL if nothing is playing. The result needs to be
 * freed with free().
 */
char *
avbox_player_gettitle(struct avbox_player *inst);


/**
 * Get the media duration.
 */
void
avbox_player_getduration(struct avbox_player * const inst, int64_t *duration);


/**
 * Get the media position in microseconds.
 */
void
avbox_player_gettime(struct avbox_player * const inst, int64_t *time);


/**
 * Get the state of the stream buffer
 */
unsigned int
avbox_player_bufferstate(struct avbox_player *inst);


/**
 * Seek to a chapter.
 */
int
avbox_player_seek_chapter(struct avbox_player *inst, int incr);


void
avbox_player_update(struct avbox_player* inst);


int 
avbox_player_play(struct avbox_player* inst, const char * const path);


int
avbox_player_pause(struct avbox_player* inst);


int
avbox_player_stop(struct avbox_player* inst);


/**
 * Create a new media player instance.
 */
struct avbox_player*
avbox_player_new(struct avbox_window *window);


/**
 * Destroy media player instance.
 */
void
avbox_player_destroy(struct avbox_player *inst);

#endif

