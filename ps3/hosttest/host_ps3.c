/*
 * Copyright (C) 2026 the ROTT-PS3 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * =======================================================================
 *
 * Host harness: the PS3's video and pad, replaced so the whole game (the
 * same engine objects, the same SDL shim, launcher and mixer) runs on a
 * PC -- natively, or as powerpc64 big-endian under qemu-user, which is
 * the point: the PS3's byte order and 64 bit pointers without a console.
 *
 *  - Video: frames kept in memory; the ones asked for in ROTT_HOST_SHOTS
 *    ("ms,ms,...") are written as PPM files to ROTT_HOST_OUT.
 *  - Pad: a script in ROTT_HOST_PAD, "ms:BUTTON|BUTTON,ms:,..." (the state
 *    holds until the next entry). Buttons: UP DOWN LEFT RIGHT CROSS
 *    CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 L3 R3 START SELECT, and sticks
 *    LX- LX+ LY- LY+ RX- RX+ RY- RY+ (all the way).
 *  - ROTT_HOST_QUIT_MS: the XMB's "Quit Game" at that time.
 *
 * Time is real time: under qemu the game runs slower, but it's the same
 * code the console runs.
 *
 * =======================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "ps3_platform.h"
#include "ps3_pad.h"
#include "ps3_video.h"
#include "ps3_font.h"

static unsigned long long
NowMs(void)
{
	static unsigned long long base;
	struct timespec ts;
	unsigned long long now;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;

	if (!base)
	{
		base = now;
	}

	return now - base;
}

/* ================================================================ */
/* Video                                                              */
/* ================================================================ */

static int ready;
static uint32_t *src;
static int src_w, src_h;
static int frames;
static int shots_taken;

int PS3_Video_Init(void) { ready = 1; return 0; }
void PS3_Video_Shutdown(void) { ready = 0; }
int PS3_Video_Ready(void) { return ready; }
void PS3_Video_Finish(void) { }
int PS3_Video_Width(void) { return 1280; }
int PS3_Video_Height(void) { return 720; }
void PS3_Video_SetFit(int percent) { (void)percent; }
void PS3_Video_SetFilter(int linear) { (void)linear; }
void PS3_Video_SetLock30(int lock) { (void)lock; }
void PS3_Video_SetPerfLog(int on) { (void)on; }
void PS3_Video_ProfileCopy(long long usec) { (void)usec; }
void PS3_Video_ProfileIdle(long long usec) { (void)usec; }
void PS3_Video_SetDisplayAspect(int num, int den) { (void)num; (void)den; }

/* the damage flash goes to the TV's bars on the console: log it here */
void
PS3_Video_SetBorderColor(unsigned int argb)
{
	static unsigned int last = 0xff000000u;

	if (argb != last)
	{
		PS3_Log("[host] border color %08x", argb);
		last = argb;
	}
}

int
PS3_Video_SetSource(int w, int h)
{
	if (src && w == src_w && h == src_h)
	{
		return 0;
	}

	free(src);
	src = calloc((size_t)w * h, 4);
	src_w = w;
	src_h = h;
	PS3_Log("[video] source image %dx%d", w, h);

	return src ? 0 : -1;
}

uint32_t *
PS3_Video_Source(void)
{
	return src;
}

void
PS3_Video_FreeSource(void)
{
	free(src);
	src = NULL;
	src_w = src_h = 0;
}

static void
SavePPM(const char *tag)
{
	const char *dir = getenv("ROTT_HOST_OUT");
	char path[512];
	FILE *f;
	int i;

	if (!src)
	{
		return;
	}

	snprintf(path, sizeof(path), "%s/%s.ppm", dir ? dir : ".", tag);
	f = fopen(path, "wb");

	if (!f)
	{
		return;
	}

	fprintf(f, "P6\n%d %d\n255\n", src_w, src_h);

	for (i = 0; i < src_w * src_h; i++)
	{
		uint32_t p = src[i];

		fputc((int)((p >> 16) & 0xff), f);
		fputc((int)((p >> 8) & 0xff), f);
		fputc((int)(p & 0xff), f);
	}

	fclose(f);
	PS3_Log("[host] frame %d saved: %s", frames, path);
}

void
PS3_Video_Present(void)
{
	const char *shots = getenv("ROTT_HOST_SHOTS");
	unsigned long long now = NowMs();

	frames++;

	if (shots)
	{
		/* the n-th time in the list that has passed and isn't taken yet */
		const char *p = shots;
		int n = 0;

		while (*p)
		{
			unsigned long long t = strtoull(p, (char **)&p, 10);

			if (n == shots_taken && now >= t)
			{
				char tag[32];

				snprintf(tag, sizeof(tag), "shot%02d_%llums", n, t);
				SavePPM(tag);
				shots_taken++;
				break;
			}

			n++;

			if (*p == ',')
			{
				p++;
			}
			else
			{
				break;
			}
		}
	}
}

void
PS3_Video_TextScreen(const char *title, const char *const *lines, int nlines)
{
	int i;

	PS3_Log("[host] TEXT SCREEN: %s", title ? title : "");

	for (i = 0; i < nlines; i++)
	{
		PS3_Log("[host]   %s", lines[i] ? lines[i] : "");
	}
}

/* ================================================================ */
/* Pad                                                                */
/* ================================================================ */

static const struct
{
	const char *name;
	unsigned bit;
} names[] = {
	{"LEFT", PS3_PAD_LEFT}, {"DOWN", PS3_PAD_DOWN}, {"RIGHT", PS3_PAD_RIGHT},
	{"UP", PS3_PAD_UP}, {"START", PS3_PAD_START}, {"R3", PS3_PAD_R3},
	{"L3", PS3_PAD_L3}, {"SELECT", PS3_PAD_SELECT}, {"SQUARE", PS3_PAD_SQUARE},
	{"CROSS", PS3_PAD_CROSS}, {"CIRCLE", PS3_PAD_CIRCLE},
	{"TRIANGLE", PS3_PAD_TRIANGLE}, {"R1", PS3_PAD_R1}, {"L1", PS3_PAD_L1},
	{"R2", PS3_PAD_R2}, {"L2", PS3_PAD_L2},
};

int PS3_Pad_Init(void) { return 0; }
void PS3_Pad_Shutdown(void) { }

void
PS3_Pad_Poll(ps3_padstate_t *out)
{
	const char *script = getenv("ROTT_HOST_PAD");
	const char *quit = getenv("ROTT_HOST_QUIT_MS");
	unsigned long long now = NowMs();
	char state[256] = "";
	const char *p = script;
	char *tok;
	static char last[256] = "~";

	if (quit && now >= strtoull(quit, NULL, 10))
	{
		PS3_Host_RequestExit();
	}

	out->connected = 1;
	out->buttons = 0;
	out->lx = out->ly = out->rx = out->ry = 128;

	/* the last entry whose time has come */
	while (p && *p)
	{
		unsigned long long t = strtoull(p, (char **)&p, 10);
		const char *end;

		if (*p != ':')
		{
			break;
		}

		p++;
		end = strchr(p, ',');

		if (t <= now)
		{
			size_t len = end ? (size_t)(end - p) : strlen(p);

			if (len >= sizeof(state))
			{
				len = sizeof(state) - 1;
			}

			memcpy(state, p, len);
			state[len] = '\0';
		}

		p = end ? end + 1 : NULL;
	}

	if (strcmp(state, last))
	{
		PS3_Log("[host] pad at %llu ms: '%s'", now, state);
		snprintf(last, sizeof(last), "%s", state);
	}

	for (tok = strtok(state, "|"); tok; tok = strtok(NULL, "|"))
	{
		size_t i;

		for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		{
			if (!strcmp(tok, names[i].name))
			{
				out->buttons |= names[i].bit;
			}
		}

		if (!strcmp(tok, "LX-")) out->lx = 0;
		if (!strcmp(tok, "LX+")) out->lx = 255;
		if (!strcmp(tok, "LY-")) out->ly = 0;
		if (!strcmp(tok, "LY+")) out->ly = 255;
		if (!strcmp(tok, "RX-")) out->rx = 0;
		if (!strcmp(tok, "RX+")) out->rx = 255;
		if (!strcmp(tok, "RY-")) out->ry = 0;
		if (!strcmp(tok, "RY+")) out->ry = 255;
	}
}
