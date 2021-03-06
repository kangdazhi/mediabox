/**
 * Copyright (c) 2016-2017 Fernando Rodriguez - All rights reserved
 * This file is part of mediabox.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define LOG_MODULE "mainmenu"

#include "lib/log.h"
#include "lib/debug.h"
#include "lib/dispatch.h"
#include "lib/ui/video.h"
#include "lib/ui/listview.h"
#include "lib/ui/input.h"
#include "library.h"
#include "lib/su.h"
#include "shell.h"
#include "about.h"
#include "downloads.h"
#include "mediasearch.h"
#include "a2dp.h"


/**
 * Main menu.
 */
struct mbox_mainmenu
{
	struct avbox_window *window;
	struct avbox_object *notify_object;
	struct avbox_listview *menu;
	struct mbox_library *library;
	struct mbox_about *about;
	struct mbox_downloads *downloads;
	struct mbox_mediasearch *search;
	struct mbox_a2dp *a2dp;
};


static void
mbox_mainmenu_dismiss(struct mbox_mainmenu *inst)
{
	avbox_listview_releasefocus(inst->menu);
	avbox_window_hide(inst->window);

	/* send dismissed message to parent */
	if (avbox_object_sendmsg(&inst->notify_object,
		AVBOX_MESSAGETYPE_DISMISSED, AVBOX_DISPATCH_UNICAST, inst) == NULL) {
		LOG_VPRINT_ERROR("Could not send dismissed message: %s",
			strerror(errno));
	}
}


/**
 * Handle incoming messages.
 */
static int
mbox_mainmenu_messagehandler(void *context, struct avbox_message *msg)
{
	struct mbox_mainmenu * const inst = context;

	switch (avbox_message_id(msg)) {
	case AVBOX_MESSAGETYPE_SELECTED:
	{
		void * const payload =
			avbox_message_payload(msg);

		DEBUG_PRINT("mainmenu", "Received SELECTED message");

		if (payload == inst->menu) {
			char *selected = avbox_listview_getselected(inst->menu);

			assert(selected != NULL);

			if (!memcmp("LIB", selected, 4)) {
				if ((inst->library = mbox_library_new(avbox_window_object(inst->window))) == NULL) {
					LOG_PRINT_ERROR("Could not initialize library!");
				} else {
					if (mbox_library_show(inst->library) == -1) {
						LOG_PRINT_ERROR("Could not show library!");
						mbox_library_destroy(inst->library);
						inst->library = NULL;
					}
				}
			} else if (!memcmp("REBOOT", selected, 6)) {
				mbox_shell_reboot();

			} else if (!memcmp("ABOUT", selected, 5)) {
				if (inst->about != NULL) {
					DEBUG_PRINT("mainmenu", "About dialog already visible!");
				} else {
					if ((inst->about = mbox_about_new(avbox_window_object(inst->window))) == NULL) {
						LOG_PRINT_ERROR("Could not create about box!");
					} else {
						if (mbox_about_show(inst->about) == -1) {
							LOG_PRINT_ERROR("Could not show about box!");
							mbox_about_destroy(inst->about);
							inst->about = NULL;
						}
					}
				}
			} else if (!memcmp("DOWN", selected, 4)) {
				if (inst->downloads != NULL) {
					DEBUG_PRINT("mainmenu", "Downloads already visible!");
				} else {
					if ((inst->downloads = mbox_downloads_new(avbox_window_object(inst->window))) == NULL) {
						LOG_PRINT_ERROR("Could not create downloads window!");
					} else {
						if (mbox_downloads_show(inst->downloads) == -1) {
							LOG_PRINT_ERROR("Could not show downloads window!");
							mbox_downloads_destroy(inst->downloads);
							inst->downloads = NULL;
						}
					}
				}
			} else if (!memcmp("MEDIASEARCH", selected, 11)) {
				if (inst->search != NULL) {
					DEBUG_PRINT("mainmenu", "Search already visible!");
				} else {
					if ((inst->search = mbox_mediasearch_new(avbox_window_object(inst->window))) == NULL) {
						LOG_PRINT_ERROR("Could not create search window!");
					} else {
						if (mbox_mediasearch_show(inst->search) == -1) {
							LOG_PRINT_ERROR("Could not show search window!");
							mbox_mediasearch_destroy(inst->search);
							inst->search = NULL;
						}
					}
				}
			} else if (!strcmp("A2DP", selected)) {
				DEBUG_PRINT("mainmenu", "Selected bluetooth audio");

				if (inst->a2dp != NULL) {
					DEBUG_PRINT("mainmenu", "A2DP Already Active!!");
				} else {
					if ((inst->a2dp = mbox_a2dp_new(avbox_window_object(inst->window))) == NULL) {
						LOG_PRINT_ERROR("Could not create a2dp window!");
					} else {
						if (mbox_a2dp_show(inst->a2dp) == -1) {
							LOG_PRINT_ERROR("Could not show a2dp window!");
							mbox_a2dp_destroy(inst->a2dp);
							inst->a2dp = NULL;
						}
					}
				}
			} else {
				DEBUG_VPRINT("mainmenu", "Selected %s", selected);
			}
		} else {
			DEBUG_VABORT("mainmenu", "Received SELECTED message with invalid payload: %p",
				payload);
		}

		break;
	}
	case AVBOX_MESSAGETYPE_DISMISSED:
	{
		DEBUG_PRINT("mainmenu", "Received DISMISSED message");

		void * const payload =
			avbox_message_payload(msg);

		assert(payload != NULL);

		if (payload == inst->menu) {
			mbox_mainmenu_dismiss(inst);
		} else {
			if (payload == inst->library) {
				assert(inst->library != NULL);
				mbox_library_destroy(inst->library);
				inst->library = NULL;
				mbox_mainmenu_dismiss(inst);

			} else if (payload == inst->about) {
				DEBUG_PRINT("mainmenu", "Destroying about box");
				assert(inst->about != NULL);
				mbox_about_destroy(inst->about);
				inst->about = NULL;
			} else if (payload == inst->downloads) {
				assert(inst->downloads != NULL);
				mbox_downloads_destroy(inst->downloads);
				inst->downloads = NULL;
			} else if (payload == inst->search) {
				assert(inst->search != NULL);
				mbox_mediasearch_destroy(inst->search);
				inst->search = NULL;
			} else if (payload == inst->a2dp) {
				DEBUG_PRINT("mainmenu", "Destroying a2dp window");
				assert(inst->a2dp != NULL);
				mbox_a2dp_destroy(inst->a2dp);
				inst->a2dp = NULL;

			} else {
				DEBUG_VABORT("mediasearch", "Unexpected DISMISSED message: %p",
					payload);
			}

			/* the window compositor is not complete yet
			 * and will not properly redraw this window after
			 * the child window has been dismissed so for now
			 * we just dismiss this one to */
			avbox_window_update(inst->window);

#if 0
			/* send dismissed message to parent */
			if (avbox_object_sendmsg(&inst->notify_object,
				AVBOX_MESSAGETYPE_DISMISSED, AVBOX_DISPATCH_UNICAST, inst) == NULL) {
				LOG_VPRINT_ERROR("Could not send dismissed message: %s",
					strerror(errno));
			}
#endif
		}
		break;
	}
	case AVBOX_MESSAGETYPE_DESTROY:
	{
		DEBUG_PRINT("mainmenu", "Destroying mainmenu");
		if (inst->library != NULL) {
			mbox_library_destroy(inst->library);
		}
		if (inst->search != NULL) {
			mbox_mediasearch_destroy(inst->search);
		}
		if (inst->downloads != NULL) {
			mbox_downloads_destroy(inst->downloads);
		}
		if (inst->about != NULL) {
			mbox_about_destroy(inst->about);
		}
		if (inst->menu != NULL) {
			avbox_listview_destroy(inst->menu);
		}
		break;
	}
	case AVBOX_MESSAGETYPE_CLEANUP:
	{
		DEBUG_PRINT("mainmenu", "Cleaning up mainmenu");
		free(inst);
		return AVBOX_DISPATCH_OK;
	}
	default:
		return AVBOX_DISPATCH_CONTINUE;
	}
	return AVBOX_DISPATCH_OK;
}


/**
 * Initialize the MediaBox menu
 */
struct mbox_mainmenu *
mbox_mainmenu_new(struct avbox_object *notify_object)
{
	struct mbox_mainmenu *inst;
	int xres, yres;
	int font_height;
	int window_height, window_width;

#ifdef ENABLE_BLUETOOTH
	int n_entries = 9;
#else
	int n_entries = 8;
#endif

	/* allocate memory for main menu */
	if ((inst = malloc(sizeof(struct mbox_mainmenu))) == NULL) {
		assert(errno == ENOMEM);
		return NULL;
	}

#ifdef ENABLE_REBOOT
	if (mb_su_canroot()) {
		n_entries++;
	}
#endif

	/* set height according to font size */
	avbox_window_getcanvassize(avbox_video_getrootwindow(0), &xres, &yres);
	font_height = mbv_getdefaultfontheight();
	window_height = 3 + font_height + ((font_height + 3) * n_entries);

	DEBUG_VPRINT("mainmenu", "Default font size: %i", font_height);

	/* set width according to screen size */
	switch (xres) {
	case 1024: window_width =  400; break;
	case 1280: window_width =  800; break;
	case 1920: window_width = 1024; break;
	case 640:
	default:   window_width = 300; break;
	}

	/* create a new window for the menu dialog */
	inst->window = avbox_window_new(NULL, "mainmenu",
		AVBOX_WNDFLAGS_DECORATED,
		(xres / 2) - (window_width / 2),
		(yres / 2) - (window_height / 2),
		window_width, window_height, mbox_mainmenu_messagehandler, NULL, inst);
	if (inst->window == NULL) {
		LOG_PRINT_ERROR("Could not create new window!");
		free(inst);
		return NULL;
	}
	if (avbox_window_settitle(inst->window, "MAIN MENU") == -1) {
		LOG_VPRINT_ERROR("Could not set window title: %s",
			strerror(errno));
		avbox_window_destroy(inst->window);
		return NULL;
	}

	/* create a new menu widget inside main window */
	if ((inst->menu = avbox_listview_new(inst->window, avbox_window_object(inst->window))) == NULL) {
		LOG_VPRINT_ERROR("Could not create menu widget (errno=%i)",
			errno);
		avbox_window_destroy(inst->window);
		return NULL;
	}

	/* initialize instance object */
	inst->notify_object = notify_object;
	inst->library = NULL;
	inst->search = NULL;
	inst->about = NULL;
	inst->downloads = NULL;
	inst->a2dp = NULL;

	/* populate the list */
	if (avbox_listview_additem(inst->menu, "MEDIA LIBRARY", "LIB") == -1 ||
#ifdef ENABLE_BLUETOOTH
		avbox_listview_additem(inst->menu, "BLUETOOTH AUDIO", "A2DP") == -1 ||
#endif
		avbox_listview_additem(inst->menu, "OPTICAL DISC", "DVD") == -1 ||
		avbox_listview_additem(inst->menu, "TV TUNNER", "DVR") == -1 ||
		avbox_listview_additem(inst->menu, "DOWNLOADS", "DOWN") == -1 ||
		avbox_listview_additem(inst->menu, "MEDIA SEARCH", "MEDIASEARCH") == -1 ||
		avbox_listview_additem(inst->menu, "GAMING CONSOLES", "CONSOLES") == -1 ||
		avbox_listview_additem(inst->menu, "SETTINGS", "SETTINGS") == -1 ||
		avbox_listview_additem(inst->menu, "ABOUT MEDIABOX", "ABOUT") == -1) {
		LOG_PRINT_ERROR("Could not populate list!");
		avbox_window_destroy(inst->window);
		return NULL;
	}

#ifdef ENABLE_REBOOT
	if (mb_su_canroot()) {
		avbox_listview_additem(menu, "REBOOT", "REBOOT");
	}
#endif

	return inst;
}


/**
 * Show window.
 */
int
mbox_mainmenu_show(struct mbox_mainmenu * const inst)
{
	/* show the menu window */
        avbox_window_show(inst->window);

	/* give focus to the menu */
	if (avbox_listview_focus(inst->menu) == -1) {
		DEBUG_PRINT("mainmenu", "Could not show dialog!");
		return -1;
	}

	return 0;
}


/**
 * Destroy the main menu.
 */
void
mbox_mainmenu_destroy(struct mbox_mainmenu * const inst)
{
	DEBUG_PRINT("mainmenu", "Destructor for main menu called");
	avbox_window_destroy(inst->window);
}
