/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

/* TODO: We need a control thread or at least an internal
 * dispatch object for the worker threads to be able to
 * communicate errors among themselves */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#define HAVE_MALLOC_TRIM	(1)	/* TODO: Check for this on configure */

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifdef HAVE_MALLOC_TRIM
#include <malloc.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <pango/pangocairo.h>

#define LOG_MODULE "player"

#include "video.h"
#include "player.h"
#include "../debug.h"
#include "../log.h"
#include "../su.h"
#include "../time_util.h"
#include "../timers.h"
#include "../linkedlist.h"
#include "../audio.h"
#include "../compiler.h"
#include "../queue.h"
#include "../dispatch.h"
#include "../application.h"


/*
 * For now to change the pix format it needs to be
 * done here and on video-directfb.c avbox_window_blitbuf()
 * function. We need to implement our own enum with supported
 * formats (on video.h) and add it as an argument to that
 * function. Then use a LUT to map between those and ffmpeg's.
 */
//#define MB_DECODER_PIX_FMT 		(AV_PIX_FMT_RGB565)
//#define MB_DECODER_PIX_FMT 		(AV_PIX_FMT_RGB32)
#define MB_DECODER_PIX_FMT 		(AV_PIX_FMT_BGRA)

/* This is the # of frames to decode ahead of time */
#define MB_VIDEO_BUFFER_FRAMES  (10)
#define MB_VIDEO_BUFFER_PACKETS (1)
#define MB_AUDIO_BUFFER_PACKETS (1)

/* #define MB_DECODER_PRINT_FPS */

struct avbox_video_frame
{
	uint8_t *data;
	int64_t pts;
	AVRational time_base;
};


/**
 * Player structure.
 */
struct avbox_player
{
	struct avbox_window *window;
	struct avbox_queue *video_packets_q;
	struct avbox_queue *audio_packets_q;
	struct avbox_queue *video_frames_q;
	uint8_t *video_buffers[MB_VIDEO_BUFFER_FRAMES + 1];
	int video_buffer_index;

	const char *media_file;
	enum avbox_player_status status;
	int video_output_quit;
	int frames_rendered;
	int width;
	int height;
	int last_err;
	int have_audio;
	int have_video;
	int stream_quit;
	int stopping;
	int64_t seek_to;
	int seek_result;
	uint8_t *buf;
	int bufsz;
	struct timespec systemreftime;
	int64_t lasttime;
	int64_t systemtimeoffset;
	int64_t (*getmastertime)(struct avbox_player *inst);
	struct avbox_dispatch_object *notify_object;

	AVFormatContext *fmt_ctx;

	struct avbox_audiostream *audio_stream;

	AVCodecContext *audio_codec_ctx;
	int audio_time_set;

	int audio_stream_index;
	pthread_t audio_decoder_thread;

	/* FIXME: We shouldn't save a copy of the last frame but
	 * instead keep a reference to it. Hopefully we can come
	 * up with a solution that does not require a lock to
	 * handle avbox_player_update() */
	void *video_last_frame;
	pthread_mutex_t update_lock;

	int video_stream_index;
	AVCodecContext *video_codec_ctx;
	int video_paused;
	int video_playback_running;
	int video_flush_decoder;
	int video_flush_output;
	unsigned int video_skipframes;
	int64_t video_decoder_pts;
	int64_t video_renderer_pts;
	int video_decoder_running;
	int audio_decoder_running;
	AVRational video_decoder_timebase;
	pthread_t video_decoder_thread;
	pthread_t video_output_thread;
	pthread_cond_t stream_signal;
	pthread_mutex_t stream_lock;
	pthread_t stream_thread;


	int stream_percent;

	/* overlay stuff */
	int top_overlay_timer_id;
	char *top_overlay_text;
	enum mbv_alignment top_overlay_alignment;
	pthread_mutex_t top_overlay_lock;

	/* playlist stuff */
	LIST_DECLARE(playlist);
	struct avbox_playlist_item *playlist_item;
};


/* Pango global context */
PangoFontDescription *pango_font_desc = NULL;
static pthread_mutex_t thread_start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t thread_start_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t thread_start_cond = PTHREAD_COND_INITIALIZER;


static uint8_t *
avbox_player_getnextvideobuffer(struct avbox_player * const inst)
{
	inst->video_buffer_index = (inst->video_buffer_index + 1) %
		(MB_VIDEO_BUFFER_FRAMES + 1);
	return inst->video_buffers[inst->video_buffer_index];
}


/**
 * Free the internal playlist
 */
static void
avbox_player_freeplaylist(struct avbox_player* inst)
{
	struct avbox_playlist_item *item;

	assert(inst != NULL);

	inst->playlist_item = NULL;

	LIST_FOREACH_SAFE(struct avbox_playlist_item*, item, &inst->playlist, {

		LIST_REMOVE(item);

		if (item->filepath != NULL) {
			free((void*) item->filepath);
		}

		free(item);
	});
}


/**
 * Updates the player status and
 * calls any registered callbacks
 */
static void
avbox_player_updatestatus(struct avbox_player *inst, enum avbox_player_status status)
{
	enum avbox_player_status last_status;

	assert(inst != NULL);

	last_status = inst->status;
	inst->status = status;

	/* send status notification */
	if (inst->notify_object != NULL) {
		struct avbox_player_status_data *status_data;

		/* allocate memory for status message */
		if ((status_data = malloc(sizeof(struct avbox_player_status_data))) == NULL) {
			LOG_VPRINT_ERROR("Could not send status notification: %s",
				strerror(errno));
			return;
		}

		/* initialize it */
		status_data->sender = inst;
		status_data->last_status = last_status;
		status_data->status = status;

		/* send it */
		if (avbox_dispatch_sendmsg(-1, &inst->notify_object, AVBOX_MESSAGETYPE_PLAYER,
			AVBOX_DISPATCH_UNICAST, status_data) == NULL) {
			LOG_VPRINT_ERROR("Could not send status notification: %s",
				strerror(errno));
			free(status_data);
		}
	}
}


static inline void
avbox_player_printstatus(const struct avbox_player * const inst, const int fps)
{
	static int i = 0;

	int audio_frames = 0;;

	if (getpid() == 1) {
		return;
	}
	if ((i++ % 10) == 0) {
		if (inst->have_audio) {
			audio_frames = avbox_audiostream_count(inst->audio_stream);
		}
		fprintf(stdout, "| Fps: %03i | Video Packets: %03zd | Video Frames: %03zd | Audio Packets: %03zd | Audio Frames: %03i |\r",
			fps,
			avbox_queue_count(inst->video_packets_q),
			avbox_queue_count(inst->video_frames_q), 
			avbox_queue_count(inst->audio_packets_q),
			audio_frames);
		fflush(stdout);
	}
}


/**
 * Renders a text overlay on top of
 * the video image
 */
static int
avbox_player_rendertext(struct avbox_player *inst, cairo_t *context, char *text, PangoRectangle *rect)
{
	PangoLayout *layout;

	assert(inst != NULL);
	assert(context != NULL);
	assert(text != NULL);
	assert(rect != NULL);

	cairo_translate(context, rect->x, rect->y);

	if ((layout = pango_cairo_create_layout(context)) != NULL) {
		pango_layout_set_font_description(layout, pango_font_desc);
		pango_layout_set_width(layout, rect->width * PANGO_SCALE);
		pango_layout_set_height(layout, 400 * PANGO_SCALE);
		pango_layout_set_alignment(layout, mbv_get_pango_alignment(inst->top_overlay_alignment));
		pango_layout_set_text(layout, text, -1);

		cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);
		pango_cairo_update_layout(context, layout);
		pango_cairo_show_layout(context, layout);

		g_object_unref(layout);
	}

	return 0;
}


/**
 * Dump all video frames up to the specified
 * pts (in usecs)
 *
 * WARNING: DO NOT call this function from any thread except the
 * video output thread.
 */
static inline int
avbox_player_dumpvideo(struct avbox_player * const inst, const int64_t pts, const int flush)
{
	int ret = 0, c = 0;
	int64_t video_time;
	struct avbox_video_frame *frame;

	DEBUG_VPRINT("player", "Skipping frames until %li (flush=%i)",
		pts, flush);

	video_time = pts - 10000 - 1;

	/* tell decoder to skip frames */
	inst->video_codec_ctx->skip_frame = AVDISCARD_NONREF;

	while (flush || video_time < (pts - 10000)) {

		/* first drain the decoded frames buffer */
		if ((frame = avbox_queue_peek(inst->video_frames_q, !flush)) == NULL) {
			if (errno == EAGAIN) {
				if (flush) {
					break;
				}
				continue;
			} else if (errno == ESHUTDOWN) {
				break;
			} else {
				LOG_VPRINT_ERROR("ERROR: avbox_queue_get() returned error: %s",
					strerror(errno));
				abort();
			}
		}
		video_time = av_rescale_q(frame->pts, frame->time_base, AV_TIME_BASE_Q);
		if (!flush && pts != -1 && video_time >= (pts - 10000)) {
			goto end;
		}

		/* DEBUG_VPRINT("player", "video_time=%li, pts=%li",
			video_time, pts); */

		/* dequeue the frame */
		if (avbox_queue_get(inst->video_frames_q) != frame) {
			LOG_PRINT_ERROR("We peeked one frame but got a different one. WTF?");
			abort();
		}
		free(frame);
		c++;
		ret = 1;
	}
end:

	DEBUG_VPRINT("player", "Skipped %i frames", c);

	inst->video_codec_ctx->skip_frame = AVDISCARD_DEFAULT;
	return ret;
}


/**
 * Waits for the decoded stream buffers
 * to fill up
 */
static void
avbox_player_wait4buffers(struct avbox_player * const inst)
{
	/* wait for the buffers to fill up */
	do {
		/* TODO: This shouldn't be necessary */
		avbox_queue_wake(inst->video_frames_q);

		avbox_player_printstatus(inst, 0);
		usleep(100L * 1000L);
	}
	while (!avbox_queue_isclosed(inst->video_frames_q) && !inst->video_flush_output &&
		avbox_queue_count(inst->video_frames_q) < MB_VIDEO_BUFFER_FRAMES);
}


/**
 * Gets the time elapsed (in uSecs) since the
 * stream started playing. This clock stops when the audio stream is paused
 * or underruns.
 */
static int64_t
avbox_player_getaudiotime(struct avbox_player * const inst)
{
	assert(inst->audio_stream != NULL);
	return inst->lasttime = avbox_audiostream_gettime(inst->audio_stream);
}


static void
avbox_player_resetsystemtime(struct avbox_player *inst, int64_t upts)
{
	(void) clock_gettime(CLOCK_MONOTONIC, &inst->systemreftime);
	inst->systemtimeoffset = upts;
}


static int64_t
avbox_player_getsystemtime(struct avbox_player *inst)
{
	struct timespec tv;
	if (UNLIKELY(inst->video_paused)) {
		return inst->lasttime;
	}
	(void) clock_gettime(CLOCK_MONOTONIC, &tv);
	return (inst->lasttime = (utimediff(&tv, &inst->systemreftime) + inst->systemtimeoffset));
}


static inline void
avbox_player_postproc(struct avbox_player *inst, void *buf)
{
	/* if there is something to display in the top overlay
	 * then do it */
	if (UNLIKELY(inst->top_overlay_text != NULL)) {
		pthread_mutex_lock(&inst->top_overlay_lock);
		if (LIKELY(inst->top_overlay_text != NULL)) {
			/* create a cairo context for this frame */
			cairo_t *context;
			cairo_surface_t *surface;
			surface = cairo_image_surface_create_for_data(buf,
				CAIRO_FORMAT_ARGB32, inst->width, inst->height,
				cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, inst->width));
			if (LIKELY(surface != NULL)) {
				context = cairo_create(surface);
				cairo_surface_destroy(surface);

				if (LIKELY(context != NULL)) {
					PangoRectangle rect;
					rect.x = 15;
					rect.width = inst->width - 30;
					rect.y = 50;
					rect.height = 400;

					avbox_player_rendertext(inst, context,
						inst->top_overlay_text, &rect);

					cairo_destroy(context);
				}
			}
		}
		pthread_mutex_unlock(&inst->top_overlay_lock);
	}
}

struct avbox_player_update_context
{
	struct avbox_player *inst;
	uint8_t *buf;
};


/**
 * Update the display from main thread.
 */
static void *
avbox_player_doupdate(void *arg)
{
	struct avbox_player_update_context * const ctx = arg;
	avbox_window_blitbuf(ctx->inst->window, ctx->buf,
		ctx->inst->width, ctx->inst->height, 0, 0);
	avbox_window_update(ctx->inst->window);
	return NULL;
}


/**
 * Video rendering thread.
 */
static void *
avbox_player_video(void *arg)
{
	uint8_t *buf;
	int64_t frame_pts, delay, frame_time = 0;
	struct avbox_player *inst = (struct avbox_player*) arg;
	struct avbox_video_frame *frame;
	struct avbox_player_update_context ctx;
	struct avbox_delegate *del;

	
#ifdef MB_DECODER_PRINT_FPS
	struct timespec new_tp, last_tp, elapsed_tp;
	int frames = 0, fps = 0;
#endif

	MB_DEBUG_SET_THREAD_NAME("video_playback");
	DEBUG_PRINT("player", "Video renderer started");
	assert(inst != NULL);
	assert(inst->video_output_quit == 0);

	ctx.inst = inst;
	inst->video_playback_running = 1;

#ifdef MB_DECODER_PRINT_FPS
	(void) clock_gettime(CLOCK_MONOTONIC, &last_tp);
	new_tp = last_tp;
#endif

	if (!inst->have_audio) {
		/* save the reference timestamp */
		avbox_player_wait4buffers(inst);
		avbox_player_resetsystemtime(inst, 0);
	}

	/* signal control thread that we're ready */
	pthread_mutex_lock(&thread_start_mutex);
	pthread_cond_broadcast(&thread_start_cond);
	pthread_mutex_unlock(&thread_start_mutex);

	DEBUG_PRINT("player", "Video renderer ready");

	while (LIKELY(!inst->video_output_quit)) {

		/* if we got a flush command flush all decoded video */
		if (UNLIKELY(inst->video_flush_output)) {
			DEBUG_PRINT("player", "Flushing video output");
			avbox_player_dumpvideo(inst, INT64_MAX, 1);
			DEBUG_PRINT("player", "Video output flushed");
			inst->video_flush_output = 0;
		}

		/* if the queue is empty wait for it to fill up */
		if (UNLIKELY(avbox_queue_count(inst->video_frames_q) == 0)) {
			if (inst->have_audio) {
				avbox_audiostream_pause(inst->audio_stream);
				avbox_queue_peek(inst->video_frames_q, 1); /* wait for queue */
				avbox_player_wait4buffers(inst);
				avbox_audiostream_resume(inst->audio_stream);
			} else {
				avbox_queue_peek(inst->video_frames_q, 1);
				avbox_player_wait4buffers(inst);
				avbox_player_resetsystemtime(inst, frame_time);
			}
		}

		/* get the next decoded frame */
		if ((frame = avbox_queue_peek(inst->video_frames_q, 1)) == NULL) {
			if (errno == EAGAIN) {
				continue;
			} else if (errno == ESHUTDOWN) {
				break;
			} else {
				LOG_VPRINT_ERROR("Error!: avbox_queue_get() failed: %s",
					strerror(errno));
				goto video_exit;
			}
		}

		/* dereference the frame pointer for later use */
		buf = frame->data;

		/* for now just copy the last frame */
		memcpy(inst->video_last_frame, buf, inst->bufsz);

		/* perform post processing (overlays, etc) */
		avbox_player_postproc(inst, buf);

		/* get the frame pts */
		frame_pts = frame->pts;

		if  (LIKELY(frame_pts != AV_NOPTS_VALUE)) {
			int64_t elapsed;

			frame_time = av_rescale_q(frame_pts, frame->time_base, AV_TIME_BASE_Q);
			inst->video_renderer_pts = frame_time;

			elapsed = inst->getmastertime(inst);
			if (UNLIKELY(elapsed > frame_time)) {
				delay = 0;
				if (elapsed - frame_time > 100000) {
					/* if the decoder is lagging behind tell it to
					 * skip a few frames */
					if (UNLIKELY(avbox_player_dumpvideo(inst, elapsed, 0))) {
						continue;
					}

					/* skip frame */
					goto frame_complete;
				}
			} else {
				delay = frame_time - elapsed;
			}

			delay &= ~0xFF;	/* a small delay  will only waste time (context switch
					   time + recalc time) and we'll loose time because we're not
					   likely to wake up that quick */

			if (LIKELY(delay > 0)) {
				/* if we're paused then sleep a good while */
				if (inst->have_audio) {
					/**
					 * So we're waiting for the clock but the audio thread is waiting
					 * for more frames (so the clock will never advance). It looks like
					 * the video fell behing and the audio stream dryed out, but why
					 * is the audio clock behind the last video frame??
					 */
					if (avbox_audiostream_ispaused(inst->audio_stream) &&
						avbox_queue_count(inst->audio_packets_q) == 0 &&
						avbox_audiostream_count(inst->audio_stream) == 0) {
						avbox_player_dumpvideo(inst, elapsed, 1);
						LOG_PRINT_ERROR("Deadlock detected, recovered (I hope)");
					}
				} else {
					if (UNLIKELY(inst->video_paused)) {
						usleep(1000 * 500);
					}
				}

				/* we may get a huge delay after seeking that fixes
				 * itself after a few frames so let's skip the frame */
				if (delay > (1000L * 100L)) {
					delay = 1000L * 100L;
					/* goto frame_complete; */
				}

				usleep(delay);

				/* the clock may have been stopped while we slept */
				continue;

			}
		}

		/* perform the actual update from the main thread */
		ctx.buf = buf;
		if ((del = avbox_application_delegate(avbox_player_doupdate, &ctx)) == NULL) {
			LOG_PRINT_ERROR("Could not delegate update!");
		} else {
			avbox_delegate_wait(del, NULL);
		}

#ifdef MB_DECODER_PRINT_FPS
		/* calculate fps */
		frames++;
		(void) clock_gettime(CLOCK_MONOTONIC, &new_tp);
		elapsed_tp = timediff(&last_tp, &new_tp);
		if (UNLIKELY(elapsed_tp.tv_sec > 0)) {
			(void) clock_gettime(CLOCK_MONOTONIC, &last_tp);
			fps = frames;
			frames = 0;
		}
		avbox_player_printstatus(inst, fps);
#endif

frame_complete:
		/* update buffer state and signal decoder */
		if (avbox_queue_get(inst->video_frames_q) != frame) {
			LOG_PRINT_ERROR("We peeked one frame but got another one!");
			abort();
		}
		free(frame);
	}

video_exit:
	DEBUG_PRINT("player", "Video renderer exiting");

	/* free any frames left in the queue */
	while ((frame = avbox_queue_get(inst->video_frames_q)) != NULL) {
		free(frame);
	}

	/* clear screen */
	memset(inst->video_buffers[0], 0, inst->bufsz);
	ctx.buf = inst->video_buffers[0];
	if ((del = avbox_application_delegate(avbox_player_doupdate, &ctx)) == NULL) {
		LOG_PRINT_ERROR("Could not delegate update!");
	} else {
		avbox_delegate_wait(del, NULL);
	}

	inst->video_playback_running = 0;
	inst->video_renderer_pts = 0;
	inst->video_flush_output = 0;

	return NULL;
}


/**
 * Update the player window
 */
void
avbox_player_update(struct avbox_player *inst)
{
	void *frame_data;

	/* DEBUG_PRINT("player", "Updating surface"); */

	assert(inst != NULL);
	if ((frame_data = av_malloc(inst->bufsz)) == NULL) {
		return;
	}

	pthread_mutex_lock(&inst->update_lock);

	/* if the last frame buffer is NULL it means that we're not
	 * currently playing
	 */
	if (inst->video_last_frame == NULL || inst->status == MB_PLAYER_STATUS_READY) {
		pthread_mutex_unlock(&inst->update_lock);
		return;
	}

	memcpy(frame_data, inst->video_last_frame, inst->bufsz);

	pthread_mutex_unlock(&inst->update_lock);

	avbox_player_postproc(inst, frame_data);
	avbox_window_blitbuf(inst->window, frame_data, inst->width, inst->height, 0, 0);
	avbox_window_update(inst->window);
	free(frame_data);
}


/**
 * Initialize ffmpeg's filter graph
 */
static int
avbox_player_initvideofilters(
	AVFormatContext *fmt_ctx,
	AVCodecContext *dec_ctx,
	AVFilterContext **buffersink_ctx,
	AVFilterContext **buffersrc_ctx,
	AVFilterGraph **filter_graph,
	const char *filters_descr,
	int stream_index)
{
	char args[512];
	int ret = 0;
	AVFilter *buffersrc  = avfilter_get_by_name("buffer");
	AVFilter *buffersink = avfilter_get_by_name("buffersink");

	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs  = avfilter_inout_alloc();
	AVRational time_base = fmt_ctx->streams[stream_index]->time_base;
	enum AVPixelFormat pix_fmts[] = { MB_DECODER_PIX_FMT, AV_PIX_FMT_NONE };

	snprintf(args, sizeof(args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
		time_base.num, time_base.den,
		dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);
	DEBUG_VPRINT("player", "Video filter args: %s", args);


	*filter_graph = avfilter_graph_alloc();
	if (!outputs || !inputs || !*filter_graph) {
		ret = AVERROR(ENOMEM);
		goto end;
	}

	ret = avfilter_graph_create_filter(buffersrc_ctx, buffersrc, "in",
                                       args, NULL, *filter_graph);
	if (ret < 0) {
		LOG_PRINT_ERROR("Cannot create buffer source!");
		goto end;
	}

	/* buffer video sink: to terminate the filter chain. */
	ret = avfilter_graph_create_filter(buffersink_ctx, buffersink, "out",
                                       NULL, NULL, *filter_graph);
	if (ret < 0) {
		LOG_PRINT_ERROR("Cannot create buffer sink!");
		goto end;
	}

	ret = av_opt_set_int_list(*buffersink_ctx, "pix_fmts", pix_fmts,
                              AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
		LOG_PRINT_ERROR("Cannot set output pixel format!");
		goto end;
	}

	/*
	 * Set the endpoints for the filter graph. The filter_graph will
	 * be linked to the graph described by filters_descr.
	 */

	/*
	 * The buffer source output must be connected to the input pad of
	 * the first filter described by filters_descr; since the first
	 * filter input label is not specified, it is set to "in" by
	 * default.
	 */
	outputs->name       = av_strdup("in");
	outputs->filter_ctx = *buffersrc_ctx;
	outputs->pad_idx    = 0;
	outputs->next       = NULL;

	/*
	 * The buffer sink input must be connected to the output pad of
	 * the last filter described by filters_descr; since the last
	 * filter output label is not specified, it is set to "out" by
	 * default.
	 */
	inputs->name       = av_strdup("out");
	inputs->filter_ctx = *buffersink_ctx;
	inputs->pad_idx    = 0;
	inputs->next       = NULL;

	if ((ret = avfilter_graph_parse_ptr(*filter_graph, filters_descr,
                                    &inputs, &outputs, NULL)) < 0) {
		goto end;
	}

	if ((ret = avfilter_graph_config(*filter_graph, NULL)) < 0) {
	        goto end;
	}


end:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	return ret;
}


static int
avbox_player_initaudiofilters(
	AVFormatContext *fmt_ctx,
	AVCodecContext *dec_ctx,
	AVFilterContext **buffersink_ctx,
	AVFilterContext **buffersrc_ctx,
	AVFilterGraph **filter_graph,
	const char *filters_descr,
	int audio_stream_index)
{
	char args[512];
	int ret = 0;
	AVFilter *abuffersrc  = avfilter_get_by_name("abuffer");
	AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs  = avfilter_inout_alloc();
	static const enum AVSampleFormat out_sample_fmts[] = { AV_SAMPLE_FMT_S16, -1 };
	static const int64_t out_channel_layouts[] = { AV_CH_LAYOUT_STEREO, -1 };
	static const int out_sample_rates[] = { 48000, -1 };
	const AVFilterLink *outlink;
	AVRational time_base = fmt_ctx->streams[audio_stream_index]->time_base;

	DEBUG_PRINT("player", "Initializing audio filters");

	*filter_graph = avfilter_graph_alloc();
	if (!outputs || !inputs || !*filter_graph) {
		ret = AVERROR(ENOMEM);
		goto end;
	}

	/* buffer audio source: the decoded frames from the decoder will be inserted here. */
	if (!dec_ctx->channel_layout) {
		dec_ctx->channel_layout = av_get_default_channel_layout(dec_ctx->channels);
	}

	snprintf(args, sizeof(args),
		"time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64,
		time_base.num, time_base.den, dec_ctx->sample_rate,
		av_get_sample_fmt_name(dec_ctx->sample_fmt), dec_ctx->channel_layout);
	ret = avfilter_graph_create_filter(buffersrc_ctx, abuffersrc, "in",
		args, NULL, *filter_graph);
	if (ret < 0) {
		LOG_PRINT_ERROR("Cannot create audio buffer source!");
		goto end;
	}

	/* buffer audio sink: to terminate the filter chain. */
	ret = avfilter_graph_create_filter(buffersink_ctx, abuffersink, "out",
		NULL, NULL, *filter_graph);
	if (ret < 0) {
		LOG_PRINT_ERROR("Cannot create audio buffer sink!");
		goto end;
	}

	ret = av_opt_set_int_list(*buffersink_ctx, "sample_fmts", out_sample_fmts, -1,
		AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
		LOG_PRINT_ERROR("Cannot set output sample format!");
		goto end;
	}

	ret = av_opt_set_int_list(*buffersink_ctx, "channel_layouts", out_channel_layouts, -1,
		AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
		LOG_PRINT_ERROR("Cannot set output channel layout!");
		goto end;
	}

	ret = av_opt_set_int_list(*buffersink_ctx, "sample_rates", out_sample_rates, -1,
		AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
		LOG_PRINT_ERROR("Cannot set output sample rate!");
		goto end;
	}

	/*
	 * Set the endpoints for the filter graph. The filter_graph will
	 * be linked to the graph described by filters_descr.
	 */

	/*
	 * The buffer source output must be connected to the input pad of
	 * the first filter described by filters_descr; since the first
	 * filter input label is not specified, it is set to "in" by
	 * default.
	 */
	outputs->name       = av_strdup("in");
	outputs->filter_ctx = *buffersrc_ctx;
	outputs->pad_idx    = 0;
	outputs->next       = NULL;

	/*
	 * The buffer sink input must be connected to the output pad of
	 * the last filter described by filters_descr; since the last
	 * filter output label is not specified, it is set to "out" by
	 * default.
	 */
	inputs->name       = av_strdup("out");
	inputs->filter_ctx = *buffersink_ctx;
	inputs->pad_idx    = 0;
	inputs->next       = NULL;

	if ((ret = avfilter_graph_parse_ptr(*filter_graph,
		filters_descr, &inputs, &outputs, NULL)) < 0) {
		goto end;
	}

	if ((ret = avfilter_graph_config(*filter_graph, NULL)) < 0) {
		goto end;
	}

	/* Print summary of the sink buffer
	 * Note: args buffer is reused to store channel layout string */
	outlink = (*buffersink_ctx)->inputs[0];
	av_get_channel_layout_string(args, sizeof(args), -1, outlink->channel_layout);
	DEBUG_VPRINT("player", "Output: srate:%dHz fmt:%s chlayout:%s",
		(int) outlink->sample_rate,
		(char*) av_x_if_null(av_get_sample_fmt_name(outlink->format), "?"),
		args);

end:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	return ret;
}


static AVCodecContext *
open_codec_context(int *stream_idx,
	AVFormatContext *fmt_ctx, enum AVMediaType type)
{
	int ret;
	AVStream *st;
	AVCodecContext *dec_ctx = NULL;
	AVCodec *dec = NULL;
	AVDictionary *opts = NULL;

	ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
	if (ret < 0) {
		LOG_VPRINT_ERROR("Could not find %s stream in input file!",
			av_get_media_type_string(type));
		return NULL;
	} else {
		*stream_idx = ret;
		st = fmt_ctx->streams[*stream_idx];

		/* find decoder for the stream */
		dec = avcodec_find_decoder(st->codecpar->codec_id);
		if (!dec) {
			LOG_VPRINT_ERROR("Failed to find '%s' codec!",
				av_get_media_type_string(type));
			return NULL;
		}

		/* allocate decoder context */
		if ((dec_ctx = avcodec_alloc_context3(dec)) == NULL) {
			LOG_PRINT_ERROR("Could not allocate decoder context!");
			return NULL;
		}
		if ((ret = avcodec_parameters_to_context(dec_ctx, st->codecpar)) < 0) {
			LOG_VPRINT_ERROR("Could not convert decoder params to context: %d!",
				ret);
			return NULL;
		}

		/* Init the video decoder */
		av_dict_set(&opts, "flags2", "+export_mvs", 0);
		if ((ret = avcodec_open2(dec_ctx, dec, &opts)) < 0) {
			LOG_VPRINT_ERROR("Failed to open '%s' codec!",
				av_get_media_type_string(type));
			return NULL;
		}
	}
	return dec_ctx;
}


/**
 * Decodes video frames in the background.
 */
static void *
avbox_player_video_decode(void *arg)
{
	int i, video_time_set = 0;
	struct avbox_player *inst = (struct avbox_player*) arg;
	struct avbox_video_frame *frame;
	char video_filters[512];
	AVPacket *packet;
	AVFrame *video_frame_nat = NULL, *video_frame_flt = NULL;
	AVFilterGraph *video_filter_graph = NULL;
	AVFilterContext *video_buffersink_ctx = NULL;
	AVFilterContext *video_buffersrc_ctx = NULL;

	MB_DEBUG_SET_THREAD_NAME("video_decode");
	DEBUG_PRINT("player", "Video decoder starting");

	assert(inst != NULL);
	assert(inst->fmt_ctx != NULL);
	assert(inst->video_stream_index == -1);
	assert(inst->video_decoder_pts == 0);
	assert(inst->video_codec_ctx == NULL);
	assert(!inst->video_decoder_running);

	/* initialize all frame data buffers to NULL */
	inst->video_last_frame = NULL;
	for (i = 0; i < (MB_VIDEO_BUFFER_FRAMES + 1); i++) {
		inst->video_buffers[i] = NULL;
	}

	/* open the video codec */
	if ((inst->video_codec_ctx = open_codec_context(&inst->video_stream_index, inst->fmt_ctx, AVMEDIA_TYPE_VIDEO)) == NULL) {
		LOG_PRINT_ERROR("Could not open video codec context");
		goto decoder_exit;
	}

	/* initialize video filter graph */
	snprintf(video_filters, sizeof(video_filters),
		"scale='if(lt(in_w,in_h),-1,%i)':'if(lt(in_w,in_h),%i,-1)',"
		"pad=%i:%i:'((out_w - in_w) / 2)':'((out_h - in_h) / 2)'",
		inst->width, inst->height, inst->width, inst->height);

	DEBUG_VPRINT("player", "Video filters: %s", video_filters);

	if (avbox_player_initvideofilters(inst->fmt_ctx, inst->video_codec_ctx,
		&video_buffersink_ctx, &video_buffersrc_ctx, &video_filter_graph,
		video_filters, inst->video_stream_index) < 0) {
		LOG_PRINT_ERROR("Could not initialize filtergraph!");
		goto decoder_exit;
	}

	/* calculate the size of each frame and allocate buffer for it */
	inst->bufsz = av_image_get_buffer_size(MB_DECODER_PIX_FMT, inst->width, inst->height,
		av_image_get_linesize(MB_DECODER_PIX_FMT, inst->width, 0));
	inst->video_last_frame = av_malloc(inst->bufsz * sizeof(int8_t));
	if (inst->video_last_frame == NULL) {
		goto decoder_exit;
	}
	for (i = 0; i < (MB_VIDEO_BUFFER_FRAMES + 1); i++) {
		inst->video_buffers[i] = av_malloc(inst->bufsz * sizeof(int8_t));
		if (inst->video_buffers[i] == NULL) {
			LOG_PRINT_ERROR("Could not allocate video buffers!");
			goto decoder_exit;
		}
	}

	DEBUG_VPRINT("player", "video_codec_ctx: width=%i height=%i pix_fmt=%i",
		inst->width, inst->height, inst->video_codec_ctx->pix_fmt);

	/* allocate video frames */
	video_frame_nat = av_frame_alloc(); /* native */
	video_frame_flt = av_frame_alloc(); /* filtered */
	if (video_frame_nat == NULL || video_frame_flt == NULL) {
		LOG_PRINT_ERROR("Could not allocate frames!");
		goto decoder_exit;
	}

	DEBUG_PRINT("player", "Video decoder ready");

	/* signal control trhead that we're ready */
	inst->video_decoder_running = 1;
	pthread_mutex_lock(&thread_start_mutex);
	pthread_cond_signal(&thread_start_cond);
	pthread_mutex_unlock(&thread_start_mutex);


	while (1) {

		/* if we got a flush command forward it to the output
		 * thread and wait for it to be done */
		if (UNLIKELY(inst->video_flush_decoder)) {
			DEBUG_PRINT("player", "Flushing video decoder");
			assert(avbox_queue_count(inst->video_packets_q) == 0);
			inst->video_flush_output = 1;

			avcodec_flush_buffers(inst->video_codec_ctx);

			while (inst->video_flush_output) {
				avbox_queue_wake(inst->video_frames_q);
				usleep(100L * 1000L);
			}
			video_time_set = 0;

			DEBUG_PRINT("player", "Video decoder flushed");

			inst->video_flush_decoder = 0;
		}

		/* get next packet from queue */
		if ((packet = avbox_queue_peek(inst->video_packets_q, 1)) == NULL) {
			if (errno == EAGAIN) {
				continue;
			} else if (errno == ESHUTDOWN) {
				break;
			}
			LOG_VPRINT_ERROR("ERROR!: avbox_queue_get() returned error: %s",
				strerror(errno));
			break;
		}

		/* send packet to codec for decoding */
		if (UNLIKELY((i = avcodec_send_packet(inst->video_codec_ctx, packet)) < 0)) {
			if (i == AVERROR(EAGAIN)) {
				/* fall through */
			} else {
				LOG_VPRINT_ERROR("Error decoding video packet (ret=%i)", i);
				goto decoder_exit;
			}
		} else {
			if (avbox_queue_get(inst->video_packets_q) != packet) {
				LOG_VPRINT_ERROR("BUG: avbox_queue_get() returned an unexpected result: %s",
					strerror(errno));
				goto decoder_exit;
			}
			/* free packet */
			av_packet_unref(packet);
			free(packet);
		}

		/* read decoded frames from codec */
		while (LIKELY((i = avcodec_receive_frame(inst->video_codec_ctx, video_frame_nat))) == 0) {
			int64_t frame_pts;

			frame_pts = video_frame_nat->pts =
				av_frame_get_best_effort_timestamp(video_frame_nat);

			/* push the decoded frame into the filtergraph */
			if (UNLIKELY(av_buffersrc_add_frame_flags(video_buffersrc_ctx,
				video_frame_nat, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)) {
				LOG_PRINT_ERROR("Error feeding video filtergraph");
				goto decoder_exit;
			}

			/* pull filtered frames from the filtergraph */
			while (1) {
				i = av_buffersink_get_frame(video_buffersink_ctx, video_frame_flt);
				if (UNLIKELY(i == AVERROR(EAGAIN) || i == AVERROR_EOF)) {
					break;
				}
				if (UNLIKELY(i < 0)) {
					LOG_VPRINT_ERROR("Could not get video frame from filtergraph (ret=%i)",
						i);
					goto decoder_exit;
				}


				if ((frame = malloc(sizeof(struct avbox_video_frame))) == NULL) {
					LOG_PRINT_ERROR("Could not allocate video frame!");
					goto decoder_exit;
				}
				frame->data = avbox_player_getnextvideobuffer(inst);
				assert(frame->data != NULL);

				if (video_frame_flt->repeat_pict) {
					DEBUG_VPRINT("player", "repeat_pict = %i",
						video_frame_flt->repeat_pict);
				}

				/* copy picture to buffer */
				av_image_copy_to_buffer(frame->data,
					inst->bufsz, (const uint8_t **) video_frame_flt->data, video_frame_flt->linesize,
					MB_DECODER_PIX_FMT, inst->width, inst->height,
					av_image_get_linesize(MB_DECODER_PIX_FMT, inst->width, 0));

				frame->pts = frame_pts;
				frame->time_base = video_buffersink_ctx->inputs[0]->time_base;

				/* add frame to decoded frames queue */
				while (avbox_queue_put(inst->video_frames_q, frame) == -1) {
					if (errno == EAGAIN) {
						continue;
					} else if (errno == ESHUTDOWN) {
						LOG_PRINT_ERROR("Queue closed unexpectedly!");
					} else {
						LOG_VPRINT_ERROR("Error: avbox_queue_put() failed: %s",
							strerror(errno));
					}
					goto decoder_exit;
				}

				/* update the video decoder pts */
				inst->video_decoder_pts = frame_pts;
				inst->video_decoder_timebase = video_buffersink_ctx->inputs[0]->time_base;

				/* if this is the first frame then set the audio stream
				 * clock to it's pts. This is needed because not all streams
				 * start at pts 0 */
				if (UNLIKELY(!video_time_set)) {
					int64_t pts;
					pts = av_rescale_q(frame_pts,
						video_buffersink_ctx->inputs[0]->time_base,
						AV_TIME_BASE_Q);
					avbox_player_resetsystemtime(inst, pts);
					DEBUG_VPRINT("player", "First video pts: %li (unscaled=%li)",
						pts, frame_pts);
					video_time_set = 1;
				}

				av_frame_unref(video_frame_flt);
			}
			av_frame_unref(video_frame_nat);
		}
		if (i != 0 && i != AVERROR(EAGAIN)) {
			LOG_VPRINT_ERROR("ERROR: avcodec_receive_frame() returned %d (video)",
				i);
		}
	}
decoder_exit:
	DEBUG_PRINT("player", "Video decoder exiting");

	/* signal the video thread to exit and join it */
	if (inst->video_frames_q != NULL) {
		avbox_queue_close(inst->video_frames_q);
		if (inst->video_playback_running) {
			inst->video_output_quit = 1;
			pthread_join(inst->video_output_thread, NULL);
			DEBUG_PRINT("player", "Video playback thread exited");
		}

		while (avbox_queue_count(inst->video_frames_q) > 0) {
			if ((frame = avbox_queue_get(inst->video_frames_q)) == NULL) {
				LOG_VPRINT_ERROR("avbox_queue_get() returned error: %s",
					strerror(errno));
			} else {
				free(frame);
			}
		}
	}

	if (inst->video_last_frame != NULL) {
		/* make sure we're not in the middle of a
		 * repaint */
		pthread_mutex_lock(&inst->update_lock);
		free(inst->video_last_frame);
		inst->video_last_frame = NULL;
		pthread_mutex_unlock(&inst->update_lock);
	}

	/* clear the video frames queue */
	/* clear the video buffers */
	for (i = 0; i < (MB_VIDEO_BUFFER_FRAMES + 1); i++) {
		if (inst->video_buffers[i] != NULL) {
			free(inst->video_buffers[i]);
			inst->video_buffers[i] = NULL;
		}
	}
	if (video_frame_nat != NULL) {
		av_free(video_frame_nat);
	}
	if (video_frame_flt != NULL) {
		av_free(video_frame_flt);
	}
	if (video_buffersink_ctx != NULL) {
		avfilter_free(video_buffersink_ctx);
	}
	if (video_buffersrc_ctx != NULL) {
		avfilter_free(video_buffersrc_ctx);
	}
	if (video_filter_graph != NULL) {
		avfilter_graph_free(&video_filter_graph);
	}
	if (inst->video_codec_ctx != NULL) {
		avcodec_close(inst->video_codec_ctx);
		avcodec_free_context(&inst->video_codec_ctx);
		inst->video_codec_ctx = NULL; /* avcodec_free_context() already does this */
	}

	inst->video_flush_decoder = 0;

	/* signal that we're exiting */
	inst->video_decoder_running = 0;
	pthread_mutex_lock(&thread_start_mutex);
	pthread_cond_signal(&thread_start_cond);
	pthread_mutex_unlock(&thread_start_mutex);

	DEBUG_PRINT("player", "Video decoder bailing out");

	return NULL;
}


/**
 * Decodes the audio stream.
 */
static void *
avbox_player_audio_decode(void * arg)
{
	int ret;
	struct avbox_player * const inst = (struct avbox_player * const) arg;
	const char *audio_filters ="aresample=48000,aformat=sample_fmts=s16:channel_layouts=stereo";
	AVFrame *audio_frame_nat = NULL;
	AVFrame *audio_frame = NULL;
	AVFilterGraph *audio_filter_graph = NULL;
	AVFilterContext *audio_buffersink_ctx = NULL;
	AVFilterContext *audio_buffersrc_ctx = NULL;
	AVPacket *packet;

	MB_DEBUG_SET_THREAD_NAME("audio_decoder");

	assert(inst != NULL);
	assert(inst->fmt_ctx != NULL);
	assert(inst->audio_codec_ctx == NULL);
	assert(inst->audio_stream_index == -1);
	assert(inst->audio_time_set == 0);
	assert(inst->audio_packets_q != NULL);
	assert(!inst->audio_decoder_running);

	DEBUG_PRINT("player", "Audio decoder starting");

	/* open the audio codec */
	if ((inst->audio_codec_ctx = open_codec_context(&inst->audio_stream_index, inst->fmt_ctx, AVMEDIA_TYPE_AUDIO)) == NULL) {
		LOG_PRINT_ERROR("Could not open audio codec!");
		goto end;
	}

	/* allocate audio frames */
	audio_frame_nat = av_frame_alloc();
	audio_frame = av_frame_alloc();
	if (audio_frame_nat == NULL || audio_frame == NULL) {
		LOG_PRINT_ERROR("Could not allocate audio frames");
		goto end;
	}

	/* initialize audio filter graph */
	DEBUG_VPRINT("player", "Audio filters: %s", audio_filters);
	if (avbox_player_initaudiofilters(inst->fmt_ctx, inst->audio_codec_ctx,
		&audio_buffersink_ctx, &audio_buffersrc_ctx, &audio_filter_graph,
		audio_filters, inst->audio_stream_index) < 0) {
		LOG_PRINT_ERROR("Could not init filter graph!");
		goto end;
	}

	DEBUG_PRINT("player", "Audio decoder ready");

	/* signl control thread that we're ready */
	inst->audio_decoder_running = 1;
	pthread_mutex_lock(&thread_start_mutex);
	pthread_cond_signal(&thread_start_cond);
	pthread_mutex_unlock(&thread_start_mutex);


	while (1) {
		/* wait for the stream decoder to give us some packets */
		if ((packet = avbox_queue_peek(inst->audio_packets_q, 1)) == NULL) {
			if (errno == EAGAIN) {
				continue;
			} else if (errno == ESHUTDOWN) {
				break;
			}
			LOG_VPRINT_ERROR("ERROR!: avbox_queue_get() returned error: %s",
				strerror(errno));
			goto end;
		}

		/* send packets to codec for decoding */
		if (UNLIKELY(ret = avcodec_send_packet(inst->audio_codec_ctx, packet) != 0)) {
			if (ret == AVERROR(EAGAIN)) {
				/* fall through */
			} else {
				LOG_VPRINT_ERROR("Error decoding audio (ret=%i)", ret);
				goto end;
			}
		} else {
			/* remove packet from queue */
			if (avbox_queue_get(inst->audio_packets_q) != packet) {
				LOG_VPRINT_ERROR("BUG: avbox_queue_get() returned an unexpected result (%p): %s",
					packet, strerror(errno));
				goto end;
			}
			/* free packet */
			av_packet_unref(packet);
			free(packet);
		}

		/* read decoded frames from codec */
		while ((ret = avcodec_receive_frame(inst->audio_codec_ctx, audio_frame_nat)) == 0) {
			audio_frame_nat->pts =
				av_frame_get_best_effort_timestamp(audio_frame_nat);

			/* push the audio data from decoded frame into the filtergraph */
			if (UNLIKELY(av_buffersrc_add_frame_flags(audio_buffersrc_ctx,
				audio_frame_nat, 0) < 0)) {
				LOG_PRINT_ERROR("Error while feeding the audio filtergraph");
				break;
			}

			/* pull filtered audio from the filtergraph */
			while (1) {

				ret = av_buffersink_get_frame(audio_buffersink_ctx, audio_frame);
				if (UNLIKELY(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)) {
					av_frame_unref(audio_frame);
					break;
				}
				if (UNLIKELY(ret < 0)) {
					av_frame_unref(audio_frame);
					goto end;
				}

				/* if this is the first frame then set the audio stream
				 * clock to it's pts. This is needed because not all streams
				 * start at pts 0 */
				if (UNLIKELY(!inst->audio_time_set)) {
					int64_t pts;
					pts = av_rescale_q(audio_frame->pts,
						inst->fmt_ctx->streams[inst->audio_stream_index]->time_base,
						AV_TIME_BASE_Q);
					avbox_audiostream_setclock(inst->audio_stream, pts);
					DEBUG_VPRINT("player", "First audio pts: %li",
						pts);
					inst->audio_time_set = 1;
				}

				/* write frame to audio stream and free it */
				avbox_audiostream_write(inst->audio_stream, audio_frame->data[0],
					audio_frame->nb_samples);
				av_frame_unref(audio_frame);
			}
		}
		if (ret != 0 && ret != AVERROR(EAGAIN)) {
			LOG_VPRINT_ERROR("ERROR!: avcodec_receive_frame() returned %d (audio)",
				AVERROR(ret));
		}
	}
end:
	DEBUG_PRINT("player", "Audio decoder exiting");

	if (audio_frame_nat != NULL) {
		av_free(audio_frame_nat);
	}
	if (audio_frame != NULL) {
		av_free(audio_frame);
	}
	if (audio_buffersink_ctx != NULL) {
		avfilter_free(audio_buffersink_ctx);
	}
	if (audio_buffersink_ctx != NULL) {
		avfilter_free(audio_buffersrc_ctx);
	}
	if (audio_filter_graph != NULL) {
		avfilter_graph_free(&audio_filter_graph);
	}
	if (inst->audio_codec_ctx != NULL) {
		avcodec_close(inst->audio_codec_ctx);
		avcodec_free_context(&inst->audio_codec_ctx);
		inst->audio_codec_ctx = NULL; /* avcodec_free_context() already does this */
	}

	/* signal that we're exiting */
	inst->audio_decoder_running = 0;
	pthread_mutex_lock(&thread_start_mutex);
	pthread_cond_signal(&thread_start_cond);
	pthread_mutex_unlock(&thread_start_mutex);

	DEBUG_PRINT("player", "Audio decoder bailing out");

	return NULL;
}


/**
 * This is the main decoding loop. It reads the stream and feeds
 * encoded frames to the decoder threads.
 */
static void*
avbox_player_stream_parse(void *arg)
{
	int i;
	AVPacket packet, *ppacket;
	AVDictionary *stream_opts = NULL;
	struct avbox_player *inst = (struct avbox_player*) arg;

	MB_DEBUG_SET_THREAD_NAME("stream_parser");

	/* detach thread */
	pthread_detach(pthread_self());

	assert(inst != NULL);
	assert(inst->media_file != NULL);
	assert(inst->window != NULL);
	assert(inst->status == MB_PLAYER_STATUS_PLAYING || inst->status == MB_PLAYER_STATUS_BUFFERING);
	assert(inst->fmt_ctx == NULL);
	assert(inst->audio_stream == NULL);
	assert(inst->audio_time_set == 0);
	assert(inst->video_flush_decoder == 0);
	assert(inst->video_flush_output == 0);
	assert(inst->video_packets_q == NULL);
	assert(inst->video_frames_q == NULL);
	assert(inst->audio_packets_q == NULL);
	assert(!inst->audio_decoder_running);
	assert(!inst->video_decoder_running);

	inst->have_audio = 0;
	inst->have_video = 0;
	inst->video_output_quit = 0;
	inst->video_paused = 0;
	inst->audio_stream_index = -1;
	inst->video_stream_index = -1;
	inst->lasttime = 0;
	inst->seek_to = -1;

	/* get the size of the window */
	avbox_window_getcanvassize(inst->window, &inst->width, &inst->height);

	DEBUG_VPRINT("player", "Attempting to play (%ix%i) '%s'",
		inst->width, inst->height, inst->media_file);

	/* open file */
	av_dict_set(&stream_opts, "timeout", "30000000", 0);
	if (avformat_open_input(&inst->fmt_ctx, inst->media_file, NULL, &stream_opts) != 0) {
		LOG_VPRINT_ERROR("Could not open stream '%s'",
			inst->media_file);
		goto decoder_exit;
	}

	if (avformat_find_stream_info(inst->fmt_ctx, NULL) < 0) {
		LOG_PRINT_ERROR("Could not find stream info!");
		goto decoder_exit;
	}

	/* if there's an audio stream start the audio decoder */
	if (av_find_best_stream(inst->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0) >= 0) {

		DEBUG_PRINT("player", "Audio stream found");

		/* allocate filtered audio frames */
		inst->audio_stream_index = -1;
		inst->getmastertime = avbox_player_getaudiotime; /* video is slave to audio */
		inst->have_audio = 1;

		/* create audio stream */
		if ((inst->audio_stream = avbox_audiostream_new()) == NULL) {
			goto decoder_exit;
		}

		if ((inst->audio_packets_q = avbox_queue_new(MB_AUDIO_BUFFER_PACKETS)) == NULL) {
			LOG_VPRINT_ERROR("Could not create audio packets queue: %s!",
				strerror(errno));
			goto decoder_exit;
		}

		/* start the audio decoder thread and wait until it's ready to decode */
		pthread_mutex_lock(&thread_start_lock);
		pthread_mutex_lock(&thread_start_mutex);
		if (pthread_create(&inst->audio_decoder_thread, NULL, avbox_player_audio_decode, inst) != 0) {
			abort();
		}
		pthread_cond_wait(&thread_start_cond, &thread_start_mutex);
		pthread_mutex_unlock(&thread_start_mutex);
		pthread_mutex_unlock(&thread_start_lock);

		/* check that the audio thread started successfuly */
		if (!inst->audio_decoder_running) {
			LOG_PRINT_ERROR("Could not start audio decoder!");
			goto decoder_exit;
		}
	}

	/* if the file contains a video stream fire the video decoder */
	if (av_find_best_stream(inst->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0) >= 0) {
		inst->have_video = 1;
		inst->video_skipframes = 0;
		inst->video_decoder_pts = 0;
		if (!inst->have_audio) {
			inst->getmastertime = avbox_player_getsystemtime;
		}

		/* create a video packets queue */
		if ((inst->video_packets_q = avbox_queue_new(MB_VIDEO_BUFFER_PACKETS)) == NULL) {
			LOG_VPRINT_ERROR("Could not create video packets queue: %s!",
				strerror(errno));
			goto decoder_exit;
		}

		/* create a decoded frames queue */
		if ((inst->video_frames_q = avbox_queue_new(MB_VIDEO_BUFFER_FRAMES)) == NULL) {
			LOG_VPRINT_ERROR("Could not create frames queue: %s!",
				strerror(errno));
			goto decoder_exit;
		}

		/* fire the video decoder thread */
		pthread_mutex_lock(&thread_start_lock);
		pthread_mutex_lock(&thread_start_mutex);
		if (pthread_create(&inst->video_decoder_thread, NULL, avbox_player_video_decode, inst) != 0) {
			LOG_PRINT_ERROR("Could not create video decoder thread!");
			abort();
		}
		pthread_cond_wait(&thread_start_cond, &thread_start_mutex);
		pthread_mutex_unlock(&thread_start_mutex);
		pthread_mutex_unlock(&thread_start_lock);

		/* check that the video decoder started */
		if (!inst->video_decoder_running) {
			LOG_PRINT_ERROR("Could not start video decoder!");
			goto decoder_exit;
		}

		DEBUG_VPRINT("player", "Video stream %i selected",
			inst->video_stream_index);
	}

	DEBUG_PRINT("player", "Stream decoder ready");

	pthread_mutex_lock(&inst->stream_lock);
	pthread_cond_signal(&inst->stream_signal);
	pthread_mutex_unlock(&inst->stream_lock);

	/* if there's no streams to decode then exit */
	if (!inst->have_audio && !inst->have_video) {
		LOG_PRINT_ERROR("No streams to decode!");
		goto decoder_exit;
	}

	/* make sure that all queues are empty */
	assert(avbox_queue_count(inst->audio_packets_q) == 0);
	assert(avbox_queue_count(inst->video_packets_q) == 0);
	assert(avbox_queue_count(inst->video_frames_q) == 0);

	/* start decoding */
	while (LIKELY(!inst->stream_quit && av_read_frame(inst->fmt_ctx, &packet) >= 0)) {
		if (packet.stream_index == inst->video_stream_index) {
			if ((ppacket = malloc(sizeof(AVPacket))) == NULL) {
				LOG_PRINT_ERROR("Could not allocate memory for packet!");
				av_packet_unref(&packet);
				goto decoder_exit;
			}
			memcpy(ppacket, &packet, sizeof(AVPacket));
			while (1) {
				if (avbox_queue_put(inst->video_packets_q, ppacket) == -1) {
					if (errno == EAGAIN) {
						continue;
					} else if (errno == ESHUTDOWN) {
						av_packet_unref(ppacket);
						free(ppacket);
						break;
					}
					LOG_VPRINT_ERROR("Could not add packet to queue: %s",
						strerror(errno));
					av_packet_unref(ppacket);
					free(ppacket);
					goto decoder_exit;
				}
				break;
			}

		} else if (packet.stream_index == inst->audio_stream_index) {
			if ((ppacket = malloc(sizeof(AVPacket))) == NULL) {
				LOG_PRINT_ERROR("Could not allocate memory for packet!");
				av_packet_unref(&packet);
				goto decoder_exit;
			}
			memcpy(ppacket, &packet, sizeof(AVPacket));
			while (1) {
				if (avbox_queue_put(inst->audio_packets_q, ppacket) == -1) {
					if (errno == EAGAIN) {
						continue;
					} else if (errno == ESHUTDOWN) {
						av_packet_unref(ppacket);
						free(ppacket);
						break;
					}
					LOG_VPRINT_ERROR("Could not enqueue packet: %s",
						strerror(errno));
					av_packet_unref(ppacket);
					free(ppacket);
					goto decoder_exit;
				}
				break;
			}
		} else {
			av_packet_unref(&packet);
		}

		/* handle seek request */
		if (UNLIKELY(inst->seek_to != -1)) {

			int flags = 0;
			const int64_t seek_from = inst->getmastertime(inst);

			if (inst->seek_to < seek_from) {
				flags |= AVSEEK_FLAG_BACKWARD;
			}

			DEBUG_VPRINT("player", "Seeking %s from %li to %li...",
				(flags & AVSEEK_FLAG_BACKWARD) ? "BACKWARD" : "FORWARD",
				seek_from, inst->seek_to);

			/* do the seeking */
			if ((i = av_seek_frame(inst->fmt_ctx, -1, inst->seek_to, flags)) < 0) {
				char buf[256];
				buf[0] = '\0';
				av_strerror(i, buf, sizeof(buf));
				LOG_VPRINT_ERROR("Error seeking stream: %s", buf);
				inst->seek_result = -1;
			} else {
				/* drop all video packets */
				avbox_queue_lock(inst->video_packets_q);
				while (avbox_queue_count(inst->video_packets_q) > 0) {
					ppacket = avbox_queue_get(inst->video_packets_q);
					av_packet_unref(ppacket);
					free(ppacket);
				}
				avbox_queue_unlock(inst->video_packets_q);

				/* drop all audio packets */
				avbox_queue_lock(inst->audio_packets_q);
				while (avbox_queue_count(inst->audio_packets_q) > 0) {
					ppacket = avbox_queue_get(inst->audio_packets_q);
					av_packet_unref(ppacket);
					free(ppacket);
				}
				avbox_queue_unlock(inst->audio_packets_q);

				/* drop all decoded video frames */
				if (inst->have_video) {
					inst->video_flush_decoder = 1;
					while (inst->video_flush_decoder) {
						/* TODO: This is probably not necessary */
						avbox_queue_wake(inst->video_frames_q);
						usleep(100L * 1000L);
					}

					avbox_player_resetsystemtime(inst, inst->seek_to);
				}

				/* drop all decoded audio frames */
				if (inst->have_audio) {
					avbox_audiostream_pause(inst->audio_stream);
					avbox_audiostream_drop(inst->audio_stream);
					avbox_audiostream_setclock(inst->audio_stream, inst->seek_to);
					avbox_audiostream_resume(inst->audio_stream);
					avcodec_flush_buffers(inst->audio_codec_ctx);
					inst->audio_time_set = 0;

					DEBUG_VPRINT("player", "Audio time: %li",
						avbox_audiostream_gettime(inst->audio_stream));
				}

				DEBUG_VPRINT("player", "Frames dropped. (time=%li,v_packets=%i,a_packets=%i,v_frames=%i)",
					inst->getmastertime(inst), avbox_queue_count(inst->video_packets_q),
					avbox_queue_count(inst->audio_packets_q), avbox_queue_count(inst->video_frames_q));

				/* make sure everything is ok */
				assert(avbox_queue_count(inst->video_packets_q) == 0);
				assert(avbox_queue_count(inst->audio_packets_q) == 0);
				assert(avbox_queue_count(inst->video_frames_q) == 0);
				assert(avbox_audiostream_count(inst->audio_stream) == 0);
				assert(inst->getmastertime(inst) == inst->seek_to);

				DEBUG_VPRINT("player", "Seeking (newpos=%li)",
					inst->getmastertime(inst));

				/* flush stream buffers */
				avformat_flush(inst->fmt_ctx);

				inst->seek_result = 0;
			}

			inst->seek_to = -1;
			DEBUG_PRINT("player", "Seek complete");
		}
	}

decoder_exit:
	DEBUG_PRINT("player", "Stream parser exiting");

	/* clean video stuff */
	if (inst->have_video) {
		/* close the decoded frames queue */
		if (inst->video_frames_q != NULL) {
			avbox_queue_close(inst->video_frames_q);
		}

		/* signal the video decoder thread to exit and join it */
		/* NOTE: Since this thread it's a midleman it waits on both locks */
		if (inst->video_packets_q != NULL) {
			avbox_queue_close(inst->video_packets_q);
			pthread_join(inst->video_decoder_thread, NULL);

			/* free video packets */
			while (avbox_queue_count(inst->video_packets_q) > 0) {
				ppacket = avbox_queue_get(inst->video_packets_q);
				assert(ppacket != NULL);
				av_packet_unref(ppacket);
				free(ppacket);
			}

			avbox_queue_destroy(inst->video_packets_q);
			inst->video_packets_q = NULL;
		}

		if (inst->video_frames_q != NULL) {
			avbox_queue_destroy(inst->video_frames_q);
			inst->video_frames_q = NULL;
		}

		DEBUG_PRINT("player", "Video decoder thread exited");
	}

	/* clean audio stuff */
	if (inst->have_audio) {
		/* clear the have_audio flag and set the timer to system */
		inst->have_audio = 0;
		inst->audio_stream_index = -1;
		inst->getmastertime = avbox_player_getsystemtime;

		if (inst->audio_packets_q != NULL) {
			avbox_queue_close(inst->audio_packets_q);
			pthread_join(inst->audio_decoder_thread, NULL);

			/* free any remaining audio packets */
			while (avbox_queue_count(inst->audio_packets_q) > 0) {
				ppacket = avbox_queue_get(inst->audio_packets_q);
				assert(ppacket != NULL);
				av_packet_unref(ppacket);
				free(ppacket);
			}

			avbox_queue_destroy(inst->audio_packets_q);
			inst->audio_packets_q = NULL;
		}

		DEBUG_PRINT("player", "Audio decoder exiting");

		/* destroy the audio stream */
		if (inst->audio_stream != NULL) {
			avbox_audiostream_destroy(inst->audio_stream);
			inst->audio_stream = NULL;
		}
		inst->audio_time_set = 0;

	}

	/* clean other stuff */
	if (inst->fmt_ctx != NULL) {
		avformat_close_input(&inst->fmt_ctx);
		inst->fmt_ctx = NULL;
	}

	if (stream_opts != NULL) {
		av_dict_free(&stream_opts);
	}

	inst->video_stream_index = -1;
	inst->audio_stream_index = -1;
	inst->stream_quit = 1;

	avbox_player_updatestatus(inst, MB_PLAYER_STATUS_READY);

	/* I don't think there's any benefit in doing this always
	 * but it helps in debugging as all freed memory is returned to
	 * the kernel so we get a better picture */
#if !defined(NDEBUG) && defined(HAVE_MALLOC_TRIM)
	malloc_trim(0);
#endif

	/* if we're playing a playlist try to play the next
	 * item unless stop() has been called */
	if (!inst->stopping && inst->playlist_item != NULL) {
		inst->playlist_item = LIST_NEXT(struct avbox_playlist_item*,
			inst->playlist_item);
		if (!LIST_ISNULL(&inst->playlist, inst->playlist_item)) {
			avbox_player_play(inst, inst->playlist_item->filepath);
		}
	}

	inst->stopping = 0;

	/* signal that we're exitting */
	pthread_mutex_lock(&inst->stream_lock);
	pthread_cond_signal(&inst->stream_signal);
	pthread_mutex_unlock(&inst->stream_lock);

	DEBUG_PRINT("player", "Stream parser thread bailing out");

	return NULL;
}


/**
 * Get the player status
 */
enum avbox_player_status
avbox_player_getstatus(struct avbox_player *inst)
{
	return inst->status;
}


/**
 * Register a queue for status notifications.
 */
int
avbox_player_registernotificationqueue(struct avbox_player *inst,
	struct avbox_dispatch_object *notify_object)
{
	if (inst->notify_object != NULL) {
		abort();
	}
	inst->notify_object = notify_object;
	return 0;
}


/**
 * Seek to a chapter.
 */
int
avbox_player_seek_chapter(struct avbox_player *inst, int incr)
{
	int i;
	int64_t pos;

	assert(inst != NULL);

	DEBUG_VPRINT("player", "Seeking (incr=%i)", incr);

	if (inst->status != MB_PLAYER_STATUS_PLAYING &&
		inst->status != MB_PLAYER_STATUS_PAUSED) {
		return -1;
	}

	assert(inst->fmt_ctx != NULL);
	assert(inst->getmastertime != NULL);

	pos = inst->getmastertime(inst);

	/* find the current chapter */
	for (i = 0; i < inst->fmt_ctx->nb_chapters; i++) {
		AVChapter *ch = inst->fmt_ctx->chapters[i];
		if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
			i--;
			break;
		}
	}

	/* if we're seeking past the current playlist iten find the
	 * next/prev item and play it */
	if (inst->playlist_item != NULL) {
		if (incr > 0 && (inst->fmt_ctx->nb_chapters == 0 || i == (inst->fmt_ctx->nb_chapters - 1))) {
			/* seek to next playlist item */
			struct avbox_playlist_item *next_item =
				inst->playlist_item;

			while (incr--) {
				struct avbox_playlist_item *next =
					LIST_NEXT(struct avbox_playlist_item*,
					inst->playlist_item);
				if (LIST_ISNULL(&inst->playlist, next)) {
					break;
				}
				next_item = next;
			}

			if (next_item != inst->playlist_item) {
				inst->playlist_item = next_item;
				return avbox_player_play(inst, inst->playlist_item->filepath);
			} else {
				return -1;
			}
		} else if (incr < 0 && i == 0) {
			/* seek to previous playlist item */
			struct avbox_playlist_item *next_item =
				inst->playlist_item;

			while (incr++) {
				struct avbox_playlist_item *next =
					LIST_PREV(struct avbox_playlist_item*,
					inst->playlist_item);
				if (LIST_ISNULL(&inst->playlist, next)) {
					break;
				}
				next_item = next;
			}

			if (next_item != inst->playlist_item) {
				inst->playlist_item = next_item;
				return avbox_player_play(inst, inst->playlist_item->filepath);
			} else {
				return -1;
			}
		}
	}

	i += incr;
	if (i < 0 || i > inst->fmt_ctx->nb_chapters) {
		return -1;
	}

	const int64_t seek_to = av_rescale_q(inst->fmt_ctx->chapters[i]->start,
		inst->fmt_ctx->chapters[i]->time_base, AV_TIME_BASE_Q);

	DEBUG_VPRINT("player", "Seeking (pos=%li, seek_to=%li, offset=%li)",
		pos, seek_to, (seek_to - pos));

	inst->seek_to = seek_to;

	if (inst->status == MB_PLAYER_STATUS_PAUSED) {
		avbox_player_play(inst, NULL);
	}

	/* signal and wait for seek to happen */
	while (inst->seek_to != -1) {
		usleep(100L * 1000L);
	}

	return inst->seek_result;
}


unsigned int
avbox_player_bufferstate(struct avbox_player *inst)
{
	assert(inst != NULL);
	return inst->stream_percent;
}


const char *
avbox_player_getmediafile(struct avbox_player *inst)
{
	assert(inst != NULL);
	return inst->media_file;
}


/**
 * Callback function to dismiss
 * overlay text
 */
static enum avbox_timer_result
avbox_player_dismiss_top_overlay(int timer_id, void *data)
{
	struct avbox_player *inst = (struct avbox_player*) data;

	assert(inst != NULL);

	pthread_mutex_lock(&inst->top_overlay_lock);

	if (inst->top_overlay_timer_id != 0) {
		DEBUG_VPRINT("player", "Dismissing top overlay for %s",
			inst->media_file);

		if (inst->top_overlay_text != NULL) {
			free(inst->top_overlay_text);
		}

		inst->top_overlay_text = NULL;
		inst->top_overlay_timer_id = 0;
	}

	pthread_mutex_unlock(&inst->top_overlay_lock);

	return AVBOX_TIMER_CALLBACK_RESULT_CONTINUE; /* doesn't matter for ONESHOT timers */
}


/**
 * Shows overlay text on the top of the
 * screen.
 */
void
avbox_player_showoverlaytext(struct avbox_player *inst,
	const char *text, int duration, enum mbv_alignment alignment)
{
	struct timespec tv;

	pthread_mutex_lock(&inst->top_overlay_lock);

	/* if there's an overlay text being displayed then  dismiss it first */
	if (inst->top_overlay_timer_id != 0) {
		DEBUG_PRINT("player", "Cancelling existing overlay");
		avbox_timer_cancel(inst->top_overlay_timer_id);
		if (inst->top_overlay_text != NULL) {
			free(inst->top_overlay_text);
		}
		inst->top_overlay_timer_id = 0;
		inst->top_overlay_text = NULL;
	}

	/* register the top overlay */
	tv.tv_sec = duration;
	tv.tv_nsec = 0;
	inst->top_overlay_alignment = alignment;
	inst->top_overlay_text = strdup(text);
	inst->top_overlay_timer_id = avbox_timer_register(&tv, AVBOX_TIMER_TYPE_ONESHOT,
		NULL, &avbox_player_dismiss_top_overlay, inst);

	pthread_mutex_unlock(&inst->top_overlay_lock);
}


/**
 * Gets the title of the currently playing
 * media file or NULL if nothing is playing. The result needs to be
 * freed with free().
 */
char *
avbox_player_gettitle(struct avbox_player *inst)
{
	AVDictionaryEntry *title_entry;

	if (inst == NULL || inst->fmt_ctx == NULL || inst->fmt_ctx->metadata == NULL) {
		return NULL;
	}

	if ((title_entry = av_dict_get(inst->fmt_ctx->metadata, "title", NULL, 0)) != NULL) {
		if (title_entry->value != NULL) {
			return strdup(title_entry->value);
		}
	}

	return strdup(inst->media_file);
}


/**
 * If path is not NULL it opens the file
 * specified by path and starts playing it. If path is NULL
 * it resumes playback if we're on the PAUSED state and return
 * failure code (-1) if we're on any other state.
 */
int 
avbox_player_play(struct avbox_player *inst, const char * const path)
{
	int last_percent;

	assert(inst != NULL);

	/* if no path argument was provided but we're already
	 * playing a file and we're paused then just resume
	 * playback */
	if (path == NULL) {
		if (inst->status == MB_PLAYER_STATUS_PAUSED) {
			avbox_player_updatestatus(inst, MB_PLAYER_STATUS_PLAYING);
			if (inst->have_audio) {
				if (avbox_audiostream_ispaused(inst->audio_stream)) {
					avbox_audiostream_resume(inst->audio_stream);
				}
			} else {
				avbox_player_resetsystemtime(inst, inst->video_decoder_pts);
				inst->video_paused = 0;
			}
			return 0;
		}
		LOG_PRINT_ERROR("Playback failed: NULL path!");
		return -1;
	}

	/* if the audio stream is paused unpause it */
	if (inst->have_audio && avbox_audiostream_ispaused(inst->audio_stream)) {
		avbox_audiostream_resume(inst->audio_stream);
	}

	/* if we're already playing a file stop it first */
	if (inst->status != MB_PLAYER_STATUS_READY) {
		avbox_player_stop(inst);
	}

	/* initialize player object */
	const char *old_media_file = inst->media_file;
	inst->media_file = strdup(path);
	if (old_media_file != NULL) {
		free((void*) old_media_file);
	}

	/* update status */
	inst->stream_percent = last_percent = 0;
	avbox_player_updatestatus(inst, MB_PLAYER_STATUS_BUFFERING);

	/* start the main decoder thread */
	pthread_mutex_lock(&inst->stream_lock);
	inst->stream_quit = 0;
	if (pthread_create(&inst->stream_thread, NULL, avbox_player_stream_parse, inst) != 0) {
		LOG_PRINT_ERROR("Could not fire decoder thread");
		avbox_player_updatestatus(inst, MB_PLAYER_STATUS_READY);
		return -1;
	}
	pthread_cond_wait(&inst->stream_signal, &inst->stream_lock);
	pthread_mutex_unlock(&inst->stream_lock);

	/* check if decoder initialization failed */
	if (inst->stream_quit) {
		LOG_VPRINT_ERROR("Playback of '%s' failed", path);
		avbox_player_stop(inst);
		return -1;
	}

	/* if there's no audio or video to decode then return error and exit.
	 * The decoder thread will shut itself down so no cleanup is necessary */
	if (!inst->have_audio && !inst->have_video) {
		avbox_player_stop(inst);
		return -1;
	}

	/* wait for the buffers to fill up */
	while (avbox_queue_count(inst->video_frames_q) < MB_VIDEO_BUFFER_FRAMES) {

		/* update progressbar */
		int avail = avbox_queue_count(inst->video_frames_q);
		const int wanted = MB_VIDEO_BUFFER_FRAMES;
		inst->stream_percent = (((avail * 100) / wanted) * 100) / 100;

		if (inst->stream_percent != last_percent) {
			avbox_player_updatestatus(inst, MB_PLAYER_STATUS_BUFFERING);
			last_percent = inst->stream_percent;
		}

		avbox_player_printstatus(inst, 0);
		usleep(5000);
	}

	/* we're done buffering, set state to PLAYING */
	avbox_player_updatestatus(inst, MB_PLAYER_STATUS_PLAYING);

	/* show title on top overlay */
	char *title = avbox_player_gettitle(inst);
	if (title != NULL) {
		avbox_player_showoverlaytext(inst, title, 15,
			MBV_ALIGN_CENTER);
		free(title);
	}

	DEBUG_PRINT("player", "Firing rendering threads");

	/* fire the audio output thread */
	if (inst->have_audio) {
		if (avbox_audiostream_start(inst->audio_stream) == -1) {
			LOG_PRINT_ERROR("Could not start audio stream");
			inst->have_audio = 0;
		}
	}

	/* fire the video threads */
	pthread_mutex_lock(&thread_start_lock);
	pthread_mutex_lock(&thread_start_mutex);
	if (pthread_create(&inst->video_output_thread, NULL, avbox_player_video, inst) != 0) {
		LOG_PRINT_ERROR("Could not start renderer thread!");
		pthread_mutex_unlock(&thread_start_mutex);
		pthread_mutex_unlock(&thread_start_lock);
		if (!inst->have_video) {
			avbox_player_stop(inst);
			return -1;
		}
		return -1;
	}
	pthread_cond_wait(&thread_start_cond, &thread_start_mutex);
	pthread_mutex_unlock(&thread_start_mutex);
	pthread_mutex_unlock(&thread_start_lock);

	return 0;
}


/**
 * Plays a playlist.
 */
int
avbox_player_playlist(struct avbox_player* inst, LIST *playlist, struct avbox_playlist_item* selected_item)
{
	struct avbox_playlist_item *item, *item_copy;

	/* if our local list is not empty then free it first */
	if (!LIST_EMPTY(&inst->playlist)) {
		avbox_player_freeplaylist(inst);
	}

	/* copy the playlist */
	LIST_FOREACH(struct avbox_playlist_item*, item, playlist) {
		if ((item_copy = malloc(sizeof(struct avbox_playlist_item))) == NULL) {
			avbox_player_freeplaylist(inst);
			errno = ENOMEM;
			return -1;
		}
		if ((item_copy->filepath = strdup(item->filepath)) == NULL) {
			avbox_player_freeplaylist(inst);
			errno = ENOMEM;
			return -1;
		}

		LIST_ADD(&inst->playlist, item_copy);

		if (item == selected_item) {
			inst->playlist_item = item_copy;
		}
	}

	/* play the selected item */
	avbox_player_play(inst, inst->playlist_item->filepath);

	return 0;
}


/**
 * Pause the stream.
 */
int
avbox_player_pause(struct avbox_player* inst)
{
	assert(inst != NULL);

	/* can't pause if we're not playing */
	if (inst->status != MB_PLAYER_STATUS_PLAYING) {
		LOG_PRINT_ERROR("Cannot pause: Not playing!");
		return -1;
	}

	/* update status */
	avbox_player_updatestatus(inst, MB_PLAYER_STATUS_PAUSED);

	/* wait for player to pause */
	if (inst->have_audio) {
		avbox_audiostream_pause(inst->audio_stream);
	} else {
		inst->video_paused = 1;
	}

	return 0;
}


/**
 * Stop playback.
 */
int
avbox_player_stop(struct avbox_player* inst)
{
	assert(inst != NULL);

	/* if the video is paused then unpause it first. */
	if (inst->status == MB_PLAYER_STATUS_PAUSED) {
		DEBUG_PRINT("player", "Unpausing stream");
		avbox_player_play(inst, NULL);
	}

	if (inst->status != MB_PLAYER_STATUS_READY) {
		inst->stopping = 1;
		inst->stream_quit = 1;
		if (inst->video_packets_q != NULL) {
			avbox_queue_close(inst->video_packets_q);
		}
		if (inst->audio_packets_q != NULL) {
			avbox_queue_close(inst->audio_packets_q);
		}

		if (inst->have_audio) {
			if (avbox_audiostream_ispaused(inst->audio_stream)) {
				avbox_audiostream_resume(inst->audio_stream);
			}
		} else {
			/* video should have been taken care of by stop() */
			//inst->video_paused = 0;
		}

		while (inst->status != MB_PLAYER_STATUS_READY) {
			if (!avbox_application_doevents()) {
				usleep(1000);
			}
		}
		return 0;
	}
	LOG_PRINT_ERROR("Cannot stop: Nothing to stop!");
	return -1;
}


/**
 * Create a new player object.
 */
struct avbox_player*
avbox_player_new(struct avbox_window *window)
{
	struct avbox_player* inst;
	static int initialized = 0;

	/* initialize libav */
	if (!initialized) {
		av_register_all();
		avfilter_register_all();

		pango_font_desc = pango_font_description_from_string("Sans Bold 36px");
		assert(pango_font_desc != NULL);
		initialized = 1;
	}

	/* allocate memory for the player object */
	inst = malloc(sizeof(struct avbox_player));
	if (inst == NULL) {
		LOG_PRINT_ERROR("Cannot create new player instance. Out of memory");
		return NULL;
	}

	memset(inst, 0, sizeof(struct avbox_player));

	/* if no window argument was provided then use the root window */
	if (window == NULL) {
		window = avbox_video_getrootwindow(0);
		if (window == NULL) {
			LOG_PRINT_ERROR("Could not get root window");
			free(inst);
			return NULL;
		}
	}

	inst->window = window;
	inst->audio_packets_q = NULL;
	inst->video_packets_q = NULL;
	inst->video_frames_q = NULL;
	inst->video_stream_index = -1;
	inst->status = MB_PLAYER_STATUS_READY;
	inst->notify_object = NULL;
	inst->video_decoder_running = 0;
	inst->audio_decoder_running = 0;

	LIST_INIT(&inst->playlist);

	/* get the size of the window */
	avbox_window_getcanvassize(inst->window, &inst->width, &inst->height);

	/* initialize pthreads primitives */
	if (pthread_mutex_init(&inst->stream_lock, NULL) != 0 ||
		pthread_cond_init(&inst->stream_signal, NULL) != 0 ||
		pthread_mutex_init(&inst->top_overlay_lock, NULL) != 0 ||
		pthread_mutex_init(&inst->update_lock, NULL) != 0) {
		LOG_PRINT_ERROR("Cannot create player instance. Pthreads error");
		avbox_queue_destroy(inst->video_packets_q);
		free(inst);
		return NULL;
	}

	return inst;
}


/**
 * Destroy this player instance
 */
void
avbox_player_destroy(struct avbox_player *inst)
{
	assert(inst != NULL);

	DEBUG_PRINT("player", "Destroying object");

	/* this just fails if we're not playing */
	(void) avbox_player_stop(inst);

	avbox_player_freeplaylist(inst);

	if (inst->media_file != NULL) {
		free((void*) inst->media_file);
	}
	free(inst);
}

void
avbox_player_shutdown()
{
	if (LIKELY(pango_font_desc != NULL)) {
		pango_font_description_free(pango_font_desc);
	}
}